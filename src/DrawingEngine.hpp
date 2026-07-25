#pragma once

#include "ExecutionPlan.hpp"

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

class DrawingEngine {
public:
    DrawingEngine();
    ~DrawingEngine();

    DrawingEngine(const DrawingEngine&) = delete;
    DrawingEngine& operator=(const DrawingEngine&) = delete;

    void LoadPlan(const ExecutionPlan& plan);
    void ClearPlan();
    void SetRunRequested(bool requested);
    void EmergencyStop();

    [[nodiscard]] DrawingState State() const noexcept;
    [[nodiscard]] std::size_t CompletedStrokes() const noexcept;
    [[nodiscard]] std::size_t TotalStrokes() const noexcept;
    [[nodiscard]] std::size_t CompletedCommands() const noexcept;
    [[nodiscard]] std::size_t TotalCommands() const noexcept;

private:
    void Worker(std::stop_token stopToken);
    bool ExecuteCommand(
        const MouseCommand& command,
        std::chrono::milliseconds minimumButtonDownInterval,
        std::stop_token stopToken);
    bool WaitInterruptibly(std::chrono::milliseconds duration, std::stop_token stopToken);
    bool CanContinue(std::stop_token stopToken) const;
    bool SendRelativeMove(int deltaX, int deltaY);
    bool PressLeft(
        std::chrono::milliseconds minimumButtonDownInterval,
        std::stop_token stopToken);
    void ReleaseLeft() noexcept;
    void ResetWorkerPosition();

    std::atomic<std::shared_ptr<const ExecutionPlan>> plan_;
    std::atomic<DrawingState> state_{DrawingState::Idle};
    std::atomic<bool> runRequested_{false};
    std::atomic<bool> emergencyRequested_{false};
    std::atomic<bool> resetRequested_{false};
    std::atomic<bool> emergencyLatched_{false};
    std::atomic<std::size_t> completedStrokes_{0};
    std::atomic<std::size_t> completedCommands_{0};
    std::atomic<std::size_t> totalStrokes_{0};
    std::atomic<std::size_t> totalCommands_{0};

    std::mutex wakeMutex_;
    std::condition_variable_any wakeCondition_;
    HANDLE waitableTimer_{};
    std::jthread worker_;

    std::size_t commandIndex_{};
    bool logicalPenDown_{};
    bool leftPressed_{};
    bool hasPreviousButtonDown_{};
    std::chrono::steady_clock::time_point previousButtonDownAt_{};
};

} // namespace vrcdraw
