#include "DrawingEngine.hpp"

#include <algorithm>
#include <chrono>
#include <memory>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
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
    resetRequested_.store(true);
    state_.store(DrawingState::Idle);
    wakeCondition_.notify_all();
}

void DrawingEngine::SetRunRequested(const bool requested)
{
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
        if (!SendRelativeMove(command.dx, command.dy)) {
            ReleaseLeft();
            state_.store(DrawingState::Error);
            runRequested_.store(false);
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
        return WaitInterruptibly(command.duration, stopToken);
    }
    return false;
}

bool DrawingEngine::WaitInterruptibly(
    const std::chrono::milliseconds duration,
    const std::stop_token stopToken)
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
        remaining -= currentSlice;
    }
    return CanContinue(stopToken);
}

bool DrawingEngine::CanContinue(const std::stop_token stopToken) const
{
    return !stopToken.stop_requested() && runRequested_.load() &&
           !emergencyRequested_.load();
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
            !WaitInterruptibly(safeInterval - elapsed, stopToken)) {
            return false;
        }
    }
    if (!CanContinue(stopToken)) {
        return false;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        state_.store(DrawingState::Error);
        runRequested_.store(false);
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
    logicalPenDown_ = false;
    completedStrokes_.store(0);
    completedCommands_.store(0);
}

} // namespace vrcdraw
