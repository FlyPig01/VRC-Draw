#pragma once

#include "ExecutionPlan.hpp"
#include "MouseInput.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace vrcdraw {

enum class DrawingState {
    Idle,
    Ready,
    Drawing,
    Paused,
    Stopped,
    Completed,
    Error,
};

enum class DrawingIssue {
    None,
    TargetChanged,
    DrawingOutsideDesktop,
    InputDetectionFailed,
    InputSendFailed,
};

struct DrawingStartContext {
    HWND foregroundWindow{};
    POINT cursorPosition{};
    bool hasCursorPosition{};
};

class DrawingEngine {
public:
    DrawingEngine();
    ~DrawingEngine();

    DrawingEngine(const DrawingEngine&) = delete;
    DrawingEngine& operator=(const DrawingEngine&) = delete;

    void LoadPlan(const ExecutionPlan& plan);
    void ClearPlan();
    void SetRunRequested(bool requested, const DrawingStartContext& context = {});
    void EmergencyStop();

    [[nodiscard]] DrawingState State() const noexcept;
    [[nodiscard]] bool RunRequested() const noexcept;
    [[nodiscard]] MouseInputMode InputMode() const noexcept;
    [[nodiscard]] DrawingIssue Issue() const noexcept;
    [[nodiscard]] std::size_t CompletedStrokes() const noexcept;
    [[nodiscard]] std::size_t TotalStrokes() const noexcept;
    [[nodiscard]] std::size_t CompletedCommands() const noexcept;
    [[nodiscard]] std::size_t TotalCommands() const noexcept;
    [[nodiscard]] std::chrono::milliseconds CompletedPlannedDuration() const noexcept;

private:
    void Worker(std::stop_token stopToken);
    bool ExecuteCommand(
        const MouseCommand& command,
        std::chrono::milliseconds minimumButtonDownInterval,
        std::stop_token stopToken);
    bool WaitInterruptibly(
        std::chrono::milliseconds duration,
        std::stop_token stopToken,
        bool countTowardsPlan = false);
    bool CanContinue(std::stop_token stopToken);
    bool PrepareInputSession(
        const ExecutionPlan& plan,
        std::stop_token stopToken);
    bool DetectInputMode(
        const ExecutionPlan& plan,
        std::stop_token stopToken);
    bool ExecutePlannedProbePrefix(
        const ExecutionPlan& plan,
        std::size_t endCommand,
        std::stop_token stopToken);
    bool ObserveCursorBehavior(
        MousePoint origin,
        MousePoint injectedOffset,
        MouseInputMode& detectedMode,
        std::stop_token stopToken);
    bool ValidateDesktopBounds(const ExecutionPlan& plan);
    bool TargetStillForeground() const;
    void StopWithIssue(DrawingIssue issue, DrawingState state = DrawingState::Error);
    bool SendMove(int deltaX, int deltaY);
    bool SendRelativeMove(int deltaX, int deltaY);
    bool SendAbsoluteMove(MousePoint screenPoint);
    bool RestoreAbsolutePosition();
    bool PressLeft(
        std::chrono::milliseconds minimumButtonDownInterval,
        std::stop_token stopToken);
    void ReleaseLeft() noexcept;
    void ResetWorkerPosition();

    std::atomic<std::shared_ptr<const ExecutionPlan>> plan_;
    std::atomic<DrawingState> state_{DrawingState::Idle};
    std::atomic<MouseInputMode> inputMode_{MouseInputMode::Undetermined};
    std::atomic<DrawingIssue> issue_{DrawingIssue::None};
    std::atomic<bool> runRequested_{false};
    std::atomic<bool> emergencyRequested_{false};
    std::atomic<bool> resetRequested_{false};
    std::atomic<bool> emergencyLatched_{false};
    std::atomic<std::size_t> completedStrokes_{0};
    std::atomic<std::size_t> completedCommands_{0};
    std::atomic<std::size_t> totalStrokes_{0};
    std::atomic<std::size_t> totalCommands_{0};
    std::atomic<std::int64_t> completedPlannedMilliseconds_{0};
    std::atomic<HWND> requestedTargetWindow_{nullptr};
    std::atomic<LONG> requestedCursorX_{0};
    std::atomic<LONG> requestedCursorY_{0};
    std::atomic<bool> requestedCursorValid_{false};
    std::atomic<bool> startContextPending_{false};

    std::mutex wakeMutex_;
    std::condition_variable_any wakeCondition_;
    HANDLE waitableTimer_{};
    std::jthread worker_;

    std::size_t commandIndex_{};
    std::chrono::milliseconds currentCommandWaitCompleted_{};
    bool logicalPenDown_{};
    bool leftPressed_{};
    bool hasPreviousButtonDown_{};
    std::chrono::steady_clock::time_point previousButtonDownAt_{};
    bool sessionPrepared_{};
    HWND targetWindow_{};
    DWORD targetProcessId_{};
    MousePoint anchorPosition_{};
    MousePoint logicalPosition_{};
};

} // namespace vrcdraw
