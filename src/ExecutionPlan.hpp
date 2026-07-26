#pragma once

#include "PathTypes.hpp"

#include <chrono>
#include <cstddef>
#include <vector>

namespace vrcdraw {

struct MousePoint {
    int x{};
    int y{};

    friend bool operator==(const MousePoint&, const MousePoint&) = default;
};

struct MouseDelta {
    int dx{};
    int dy{};
};

enum class MouseCommandType {
    Move,
    LeftDown,
    LeftUp,
    Wait,
};

struct MouseCommand {
    MouseCommandType type{MouseCommandType::Wait};
    int dx{};
    int dy{};
    std::chrono::milliseconds duration{};
};

struct ExecutionPlanMetrics {
    std::size_t sourceSegments{};
    std::size_t strokesBeforeOptimization{};
    std::size_t strokesAfterOptimization{};
    std::size_t leftDownCount{};
    std::size_t penDownMoves{};
    std::size_t penUpMoves{};
    std::chrono::milliseconds penDownMoveTime{};
    std::chrono::milliseconds penUpMoveTime{};
    std::chrono::milliseconds buttonGuardWait{};
    std::chrono::milliseconds fixedButtonWait{};
    std::chrono::milliseconds totalTime{};
    std::size_t fittedSpanCount{};
    std::size_t acceptedFittedSpanCount{};
    std::size_t scaleFallbackSpanCount{};
    std::size_t finalPenDownSamplePoints{};
};

struct ExecutionPlan {
    int canvasWidth{};
    int canvasHeight{};
    float requestedMouseScale{1.0F};
    float mouseScale{1.0F};
    MousePoint minimumMousePosition{};
    MousePoint maximumMousePosition{};
    MousePoint minimumDrawingPosition{};
    MousePoint maximumDrawingPosition{};
    bool hasDrawingPosition{};
    std::size_t strokeCount{};
    std::chrono::milliseconds estimatedDuration{};
    std::chrono::milliseconds minimumButtonDownInterval{600};
    std::vector<MouseCommand> commands;
    std::vector<std::chrono::milliseconds> remainingDurationByCommand;
    ExecutionPlanMetrics metrics;
};

struct ExecutionOptions {
    float mouseScale{1.0F};
    int maximumDrawingDimension{768};
    int minimumPenDownStep{2};
    int maximumPenDownStep{6};
    int maximumPenUpStep{6};
    std::chrono::milliseconds penDownMoveInterval{16};
    std::chrono::milliseconds penUpMoveInterval{16};
    std::chrono::milliseconds beforeButtonDown{16};
    std::chrono::milliseconds afterButtonDown{16};
    std::chrono::milliseconds afterButtonUp{32};
    std::chrono::milliseconds minimumButtonDownInterval{600};
};

[[nodiscard]] std::vector<MouseDelta> InterpolateMouseLine(
    MousePoint start,
    MousePoint target,
    int maximumStep);

[[nodiscard]] ExecutionPlan BuildExecutionPlan(
    const DrawingPath& path,
    const ExecutionOptions& options = {});

[[nodiscard]] ExecutionPlan BuildExecutionPlan(
    const VectorDrawingPath& path,
    const BinaryImage& allowedInk,
    const ExecutionOptions& options = {});

[[nodiscard]] std::chrono::milliseconds RemainingPlannedDuration(
    const ExecutionPlan& plan,
    std::size_t completedCommands);

[[nodiscard]] float PlannedProgress(
    const ExecutionPlan& plan,
    std::size_t completedCommands);

[[nodiscard]] std::chrono::milliseconds RemainingPlannedDuration(
    const ExecutionPlan& plan,
    std::chrono::milliseconds completedDuration);

[[nodiscard]] float PlannedProgress(
    const ExecutionPlan& plan,
    std::chrono::milliseconds completedDuration);

} // namespace vrcdraw
