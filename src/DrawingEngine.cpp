#include "DrawingEngine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#ifndef MOUSEEVENTF_MOVE_NOCOALESCE
#define MOUSEEVENTF_MOVE_NOCOALESCE 0x2000
#endif

namespace vrcdraw {

DrawingEngine::DrawingEngine()
    : plan_(std::make_shared<const ExecutionPlan>()),
      waitableTimer_(CreateWaitableTimerExW(
          nullptr,
          nullptr,
          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
          TIMER_ALL_ACCESS)),
      worker_([this](const std::stop_token stopToken) { Worker(stopToken); })
{
    if (waitableTimer_ == nullptr) {
        waitableTimer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
}

DrawingEngine::~DrawingEngine()
{
    runRequested_.store(false);
    emergencyRequested_.store(true);
    worker_.request_stop();
    wakeCondition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    ReleaseLeft();
    if (waitableTimer_ != nullptr) {
        CloseHandle(waitableTimer_);
        waitableTimer_ = nullptr;
    }
}

void DrawingEngine::LoadPlan(const ExecutionPlan& plan)
{
    runRequested_.store(false);
    emergencyLatched_.store(false);
    plan_.store(std::make_shared<const ExecutionPlan>(plan));
    totalStrokes_.store(plan.strokeCount);
    totalCommands_.store(plan.commands.size());
    completedPlannedMilliseconds_.store(0);
    resetRequested_.store(true);
    state_.store(plan.commands.empty() ? DrawingState::Idle : DrawingState::Ready);
    wakeCondition_.notify_all();
}

void DrawingEngine::ClearPlan()
{
    runRequested_.store(false);
    plan_.store(std::make_shared<const ExecutionPlan>());
    totalStrokes_.store(0);
    totalCommands_.store(0);
    completedPlannedMilliseconds_.store(0);
    resetRequested_.store(true);
    state_.store(DrawingState::Idle);
    wakeCondition_.notify_all();
}

void DrawingEngine::SetRunRequested(
    const bool requested,
    const DrawingStartContext& context)
{
    if (requested) {
        requestedTargetWindow_.store(context.foregroundWindow);
        requestedCursorX_.store(context.cursorPosition.x);
        requestedCursorY_.store(context.cursorPosition.y);
        requestedCursorValid_.store(context.hasCursorPosition);
        startContextPending_.store(true);
    }
    const bool previous = runRequested_.exchange(requested);
    if (!requested) {
        emergencyLatched_.store(false);
        wakeCondition_.notify_all();
        return;
    }
    if (emergencyLatched_.load()) {
        runRequested_.store(false);
        return;
    }
    if (!previous && state_.load() == DrawingState::Completed) {
        resetRequested_.store(true);
    }
    if (!plan_.load()->commands.empty()) {
        wakeCondition_.notify_all();
    }
}

void DrawingEngine::EmergencyStop()
{
    runRequested_.store(false);
    emergencyLatched_.store(true);
    emergencyRequested_.store(true);
    wakeCondition_.notify_all();
}

DrawingState DrawingEngine::State() const noexcept
{
    return state_.load();
}

bool DrawingEngine::RunRequested() const noexcept
{
    return runRequested_.load();
}

MouseInputMode DrawingEngine::InputMode() const noexcept
{
    return inputMode_.load();
}

DrawingIssue DrawingEngine::Issue() const noexcept
{
    return issue_.load();
}

std::size_t DrawingEngine::CompletedStrokes() const noexcept
{
    return completedStrokes_.load();
}

std::size_t DrawingEngine::TotalStrokes() const noexcept
{
    return totalStrokes_.load();
}

std::size_t DrawingEngine::CompletedCommands() const noexcept
{
    return completedCommands_.load();
}

std::size_t DrawingEngine::TotalCommands() const noexcept
{
    return totalCommands_.load();
}

std::chrono::milliseconds DrawingEngine::CompletedPlannedDuration() const noexcept
{
    return std::chrono::milliseconds(completedPlannedMilliseconds_.load());
}

void DrawingEngine::Worker(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested()) {
        if (emergencyRequested_.exchange(false)) {
            ReleaseLeft();
            ResetWorkerPosition();
            state_.store(plan_.load()->commands.empty() ? DrawingState::Idle : DrawingState::Stopped);
        }
        if (resetRequested_.exchange(false)) {
            ReleaseLeft();
            ResetWorkerPosition();
            state_.store(plan_.load()->commands.empty() ? DrawingState::Idle : DrawingState::Ready);
        }

        if (!runRequested_.load()) {
            ReleaseLeft();
            if (state_.load() == DrawingState::Drawing) {
                state_.store(DrawingState::Paused);
            }
            std::unique_lock lock(wakeMutex_);
            wakeCondition_.wait_for(
                lock,
                stopToken,
                std::chrono::milliseconds(50),
                [this] {
                    return runRequested_.load() || emergencyRequested_.load() ||
                           resetRequested_.load();
                });
            continue;
        }

        const auto plan = plan_.load();
        if (plan->commands.empty()) {
            runRequested_.store(false);
            state_.store(DrawingState::Idle);
            continue;
        }
        if ((!sessionPrepared_ || startContextPending_.load()) &&
            !PrepareInputSession(*plan, stopToken)) {
            continue;
        }
        if (commandIndex_ >= plan->commands.size()) {
            ReleaseLeft();
            logicalPenDown_ = false;
            runRequested_.store(false);
            state_.store(DrawingState::Completed);
            continue;
        }

        state_.store(DrawingState::Drawing);
        if (ExecuteCommand(
                plan->commands[commandIndex_],
                plan->minimumButtonDownInterval,
                stopToken)) {
            ++commandIndex_;
            completedCommands_.store(commandIndex_);
        }
    }
    ReleaseLeft();
}

bool DrawingEngine::ExecuteCommand(
    const MouseCommand& command,
    const std::chrono::milliseconds minimumButtonDownInterval,
    const std::stop_token stopToken)
{
    if (!CanContinue(stopToken)) {
        ReleaseLeft();
        return false;
    }

    switch (command.type) {
    case MouseCommandType::Move:
        if (logicalPenDown_ && !leftPressed_ &&
            !PressLeft(minimumButtonDownInterval, stopToken)) {
            return false;
        }
        if (!logicalPenDown_) {
            ReleaseLeft();
        }
        if (!SendMove(command.dx, command.dy)) {
            ReleaseLeft();
            StopWithIssue(DrawingIssue::InputSendFailed);
            return false;
        }
        return true;

    case MouseCommandType::LeftDown:
        logicalPenDown_ = true;
        if (!PressLeft(minimumButtonDownInterval, stopToken)) {
            return false;
        }
        return true;

    case MouseCommandType::LeftUp:
        logicalPenDown_ = false;
        ReleaseLeft();
        completedStrokes_.fetch_add(1);
        return true;

    case MouseCommandType::Wait:
    {
        const auto remaining = std::max(
            std::chrono::milliseconds{}, command.duration - currentCommandWaitCompleted_);
        if (!WaitInterruptibly(remaining, stopToken, true)) {
            return false;
        }
        currentCommandWaitCompleted_ = std::chrono::milliseconds{};
        return true;
    }
    }
    return false;
}

bool DrawingEngine::WaitInterruptibly(
    const std::chrono::milliseconds duration,
    const std::stop_token stopToken,
    const bool countTowardsPlan)
{
    constexpr auto slice = std::chrono::milliseconds(16);
    auto remaining = duration;
    while (remaining.count() > 0) {
        if (!CanContinue(stopToken)) {
            ReleaseLeft();
            return false;
        }
        const auto currentSlice = std::min(remaining, slice);
        if (waitableTimer_ != nullptr) {
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -static_cast<LONGLONG>(currentSlice.count()) * 10'000LL;
            if (SetWaitableTimer(waitableTimer_, &dueTime, 0, nullptr, nullptr, FALSE) == FALSE ||
                WaitForSingleObject(
                    waitableTimer_,
                    static_cast<DWORD>(currentSlice.count() + 50)) != WAIT_OBJECT_0) {
                state_.store(DrawingState::Error);
                runRequested_.store(false);
                ReleaseLeft();
                return false;
            }
        } else {
            std::this_thread::sleep_for(currentSlice);
        }
        if (countTowardsPlan) {
            currentCommandWaitCompleted_ += currentSlice;
            completedPlannedMilliseconds_.fetch_add(currentSlice.count());
        }
        remaining -= currentSlice;
    }
    return CanContinue(stopToken);
}

bool DrawingEngine::CanContinue(const std::stop_token stopToken)
{
    if (stopToken.stop_requested() || !runRequested_.load() ||
        emergencyRequested_.load()) {
        return false;
    }
    if (sessionPrepared_ && !TargetStillForeground()) {
        ReleaseLeft();
        StopWithIssue(DrawingIssue::TargetChanged, DrawingState::Paused);
        return false;
    }
    return true;
}

bool DrawingEngine::PrepareInputSession(
    const ExecutionPlan& plan,
    const std::stop_token stopToken)
{
    const bool hasPendingContext = startContextPending_.exchange(false);
    HWND requestedWindow = hasPendingContext
        ? requestedTargetWindow_.load()
        : GetForegroundWindow();
    if (requestedWindow == nullptr) {
        requestedWindow = GetForegroundWindow();
    }
    DWORD requestedProcessId = 0;
    GetWindowThreadProcessId(requestedWindow, &requestedProcessId);
    if (requestedProcessId == 0) {
        StopWithIssue(DrawingIssue::InputDetectionFailed);
        return false;
    }

    if (sessionPrepared_) {
        if (requestedProcessId != targetProcessId_) {
            StopWithIssue(DrawingIssue::TargetChanged, DrawingState::Paused);
            return false;
        }
        issue_.store(DrawingIssue::None);
        if (!TargetStillForeground()) {
            StopWithIssue(DrawingIssue::TargetChanged, DrawingState::Paused);
            return false;
        }
        if (inputMode_.load() == MouseInputMode::DesktopAbsolute &&
            !RestoreAbsolutePosition()) {
            StopWithIssue(DrawingIssue::InputSendFailed);
            return false;
        }
        return true;
    }

    targetWindow_ = requestedWindow;
    targetProcessId_ = requestedProcessId;
    POINT cursor{};
    if (GetCursorPos(&cursor) != FALSE) {
        // Use the position at the instant probing starts. The hotkey snapshot remains
        // a fallback for the rare case where Windows refuses the live query.
    } else if (hasPendingContext && requestedCursorValid_.load()) {
        cursor.x = requestedCursorX_.load();
        cursor.y = requestedCursorY_.load();
    } else {
        StopWithIssue(DrawingIssue::InputDetectionFailed);
        return false;
    }
    anchorPosition_ = {cursor.x, cursor.y};
    sessionPrepared_ = true;
    issue_.store(DrawingIssue::None);
    inputMode_.store(MouseInputMode::Undetermined);
    if (!TargetStillForeground()) {
        StopWithIssue(DrawingIssue::TargetChanged, DrawingState::Paused);
        return false;
    }
    if (!DetectInputMode(plan, stopToken)) {
        if (!runRequested_.load() && issue_.load() == DrawingIssue::None) {
            ResetWorkerPosition();
            state_.store(plan.commands.empty() ? DrawingState::Idle : DrawingState::Ready);
            return false;
        }
        if (issue_.load() == DrawingIssue::None) {
            StopWithIssue(DrawingIssue::InputDetectionFailed);
        }
        return false;
    }
    return true;
}

bool DrawingEngine::DetectInputMode(
    const ExecutionPlan& plan,
    const std::stop_token stopToken)
{
    int systemPointerSpeed = 10;
    if (SystemParametersInfoW(
            SPI_GETMOUSESPEED, 0, &systemPointerSpeed, 0) == FALSE) {
        systemPointerSpeed = 10;
    }
    const int minimumProbeDistance = RecommendedProbeDistance(systemPointerSpeed);
    MousePoint prefixOffset{};
    std::size_t prefixEnd = 0;
    bool usablePrefix = false;
    for (std::size_t index = 0; index < plan.commands.size(); ++index) {
        const MouseCommand& command = plan.commands[index];
        if (command.type == MouseCommandType::LeftDown ||
            command.type == MouseCommandType::LeftUp) {
            break;
        }
        prefixEnd = index + 1;
        if (command.type == MouseCommandType::Move) {
            prefixOffset.x += command.dx;
            prefixOffset.y += command.dy;
            if (std::max(std::abs(prefixOffset.x), std::abs(prefixOffset.y)) >=
                minimumProbeDistance) {
                if (prefixEnd < plan.commands.size() &&
                    plan.commands[prefixEnd].type == MouseCommandType::Wait) {
                    ++prefixEnd;
                }
                usablePrefix = true;
                break;
            }
        }
    }

    const DesktopRect desktop{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
    const MousePoint probeMinimum{
        std::min(0, prefixOffset.x) - 2,
        std::min(0, prefixOffset.y) - 2,
    };
    const MousePoint probeMaximum{
        std::max(0, prefixOffset.x) + 2,
        std::max(0, prefixOffset.y) + 2,
    };
    usablePrefix = usablePrefix &&
        FitsDesktopRect(anchorPosition_, probeMinimum, probeMaximum, desktop);

    MouseInputMode detectedMode = MouseInputMode::Undetermined;
    if (usablePrefix) {
        if (!ExecutePlannedProbePrefix(plan, prefixEnd, stopToken) ||
            !ObserveCursorBehavior(
                anchorPosition_, prefixOffset, detectedMode, stopToken)) {
            return false;
        }
        if (detectedMode == MouseInputMode::DesktopAbsolute) {
            if (!SendAbsoluteMove(anchorPosition_)) {
                StopWithIssue(DrawingIssue::InputSendFailed);
                return false;
            }
            commandIndex_ = 0;
            currentCommandWaitCompleted_ = std::chrono::milliseconds{};
            logicalPosition_ = {};
            completedStrokes_.store(0);
            completedCommands_.store(0);
            completedPlannedMilliseconds_.store(0);
        }
    } else {
        const int rightSpace = desktop.left + desktop.width - 1 - anchorPosition_.x;
        const int leftSpace = anchorPosition_.x - desktop.left;
        const int bottomSpace = desktop.top + desktop.height - 1 - anchorPosition_.y;
        const int topSpace = anchorPosition_.y - desktop.top;
        const std::array spaces{rightSpace, leftSpace, bottomSpace, topSpace};
        const auto best = std::ranges::max_element(spaces);
        if (best == spaces.end() || *best < minimumProbeDistance + 4) {
            StopWithIssue(DrawingIssue::InputDetectionFailed);
            return false;
        }
        MousePoint probe{};
        switch (static_cast<int>(best - spaces.begin())) {
        case 0: probe.x = minimumProbeDistance; break;
        case 1: probe.x = -minimumProbeDistance; break;
        case 2: probe.y = minimumProbeDistance; break;
        default: probe.y = -minimumProbeDistance; break;
        }
        if (!SendRelativeMove(probe.x, probe.y)) {
            StopWithIssue(DrawingIssue::InputSendFailed);
            return false;
        }
        if (!ObserveCursorBehavior(anchorPosition_, probe, detectedMode, stopToken)) {
            if (issue_.load() == DrawingIssue::None && runRequested_.load()) {
                StopWithIssue(DrawingIssue::InputDetectionFailed);
            }
            return false;
        }
        if (detectedMode == MouseInputMode::DesktopAbsolute) {
            if (!SendAbsoluteMove(anchorPosition_)) {
                StopWithIssue(DrawingIssue::InputSendFailed);
                return false;
            }
        } else {
            detectedMode = MouseInputMode::Relative;
            if (!SendRelativeMove(-probe.x, -probe.y)) {
                StopWithIssue(DrawingIssue::InputSendFailed);
                return false;
            }
            if (!WaitInterruptibly(std::chrono::milliseconds(16), stopToken, false)) {
                if (issue_.load() == DrawingIssue::None && runRequested_.load()) {
                    StopWithIssue(DrawingIssue::InputDetectionFailed);
                }
                return false;
            }
        }
    }

    if (detectedMode == MouseInputMode::Undetermined) {
        detectedMode = MouseInputMode::Relative;
    }
    inputMode_.store(detectedMode);
    if (detectedMode == MouseInputMode::DesktopAbsolute &&
        !ValidateDesktopBounds(plan)) {
        return false;
    }
    return true;
}

bool DrawingEngine::ExecutePlannedProbePrefix(
    const ExecutionPlan& plan,
    const std::size_t endCommand,
    const std::stop_token stopToken)
{
    while (commandIndex_ < std::min(endCommand, plan.commands.size())) {
        const MouseCommand& command = plan.commands[commandIndex_];
        if (command.type == MouseCommandType::Move) {
            if (!SendRelativeMove(command.dx, command.dy)) {
                StopWithIssue(DrawingIssue::InputSendFailed);
                return false;
            }
            logicalPosition_.x += command.dx;
            logicalPosition_.y += command.dy;
        } else if (command.type == MouseCommandType::Wait) {
            if (!WaitInterruptibly(command.duration, stopToken, true)) {
                return false;
            }
            currentCommandWaitCompleted_ = std::chrono::milliseconds{};
        } else {
            break;
        }
        ++commandIndex_;
        completedCommands_.store(commandIndex_);
    }
    return true;
}

bool DrawingEngine::ObserveCursorBehavior(
    const MousePoint origin,
    const MousePoint injectedOffset,
    MouseInputMode& detectedMode,
    const std::stop_token stopToken)
{
    std::vector<MousePoint> samples;
    constexpr int maximumSamples = 6;
    samples.reserve(maximumSamples);
    for (int sample = 0; sample < maximumSamples; ++sample) {
        if (!WaitInterruptibly(std::chrono::milliseconds(16), stopToken, false)) {
            return false;
        }
        POINT cursor{};
        if (GetCursorPos(&cursor) == FALSE) {
            return false;
        }
        samples.push_back({cursor.x, cursor.y});
        detectedMode = ClassifyCursorBehavior(origin, injectedOffset, samples);
        if (detectedMode == MouseInputMode::Relative && sample >= 2) {
            return true;
        }
        if (detectedMode == MouseInputMode::DesktopAbsolute &&
            sample + 1 == maximumSamples) {
            return true;
        }
    }
    return true;
}

bool DrawingEngine::ValidateDesktopBounds(const ExecutionPlan& plan)
{
    const DesktopRect desktop{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
    if (FitsDesktopRect(
            anchorPosition_,
            plan.minimumMousePosition,
            plan.maximumMousePosition,
            desktop)) {
        return true;
    }
    StopWithIssue(DrawingIssue::DrawingOutsideDesktop);
    return false;
}

bool DrawingEngine::TargetStillForeground() const
{
    if (targetProcessId_ == 0) {
        return true;
    }
    DWORD foregroundProcessId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcessId);
    return foregroundProcessId == targetProcessId_;
}

void DrawingEngine::StopWithIssue(
    const DrawingIssue issue,
    const DrawingState state)
{
    issue_.store(issue);
    runRequested_.store(false);
    state_.store(state);
}

bool DrawingEngine::SendMove(const int deltaX, const int deltaY)
{
    const MousePoint target{
        logicalPosition_.x + deltaX,
        logicalPosition_.y + deltaY,
    };
    const bool sent = inputMode_.load() == MouseInputMode::DesktopAbsolute
        ? SendAbsoluteMove({
              anchorPosition_.x + target.x,
              anchorPosition_.y + target.y,
          })
        : SendRelativeMove(deltaX, deltaY);
    if (sent) {
        logicalPosition_ = target;
    }
    return sent;
}

bool DrawingEngine::SendRelativeMove(const int deltaX, const int deltaY)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = deltaX;
    input.mi.dy = deltaY;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool DrawingEngine::SendAbsoluteMove(const MousePoint screenPoint)
{
    const DesktopRect desktop{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
    const MousePoint normalized = NormalizeDesktopPoint(screenPoint, desktop);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = normalized.x;
    input.mi.dy = normalized.y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
        MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool DrawingEngine::RestoreAbsolutePosition()
{
    return SendAbsoluteMove({
        anchorPosition_.x + logicalPosition_.x,
        anchorPosition_.y + logicalPosition_.y,
    });
}

bool DrawingEngine::PressLeft(
    const std::chrono::milliseconds minimumButtonDownInterval,
    const std::stop_token stopToken)
{
    if (leftPressed_) {
        return true;
    }
    if (hasPreviousButtonDown_) {
        const auto safeInterval = std::max(
            std::chrono::milliseconds{}, minimumButtonDownInterval);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - previousButtonDownAt_);
        if (elapsed < safeInterval &&
            !WaitInterruptibly(safeInterval - elapsed, stopToken, false)) {
            return false;
        }
    }
    if (!CanContinue(stopToken)) {
        return false;
    }
    if (inputMode_.load() == MouseInputMode::DesktopAbsolute &&
        !RestoreAbsolutePosition()) {
        StopWithIssue(DrawingIssue::InputSendFailed);
        return false;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        StopWithIssue(DrawingIssue::InputSendFailed);
        return false;
    }
    leftPressed_ = true;
    previousButtonDownAt_ = std::chrono::steady_clock::now();
    hasPreviousButtonDown_ = true;
    return true;
}

void DrawingEngine::ReleaseLeft() noexcept
{
    if (!leftPressed_) {
        return;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
    leftPressed_ = false;
}

void DrawingEngine::ResetWorkerPosition()
{
    commandIndex_ = 0;
    currentCommandWaitCompleted_ = std::chrono::milliseconds{};
    logicalPenDown_ = false;
    logicalPosition_ = {};
    sessionPrepared_ = false;
    targetWindow_ = nullptr;
    targetProcessId_ = 0;
    inputMode_.store(MouseInputMode::Undetermined);
    issue_.store(DrawingIssue::None);
    completedStrokes_.store(0);
    completedCommands_.store(0);
    completedPlannedMilliseconds_.store(0);
}

} // namespace vrcdraw
