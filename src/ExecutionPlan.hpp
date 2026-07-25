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

struct ExecutionPlan {
    int canvasWidth{};
    int canvasHeight{};
    float mouseScale{1.0F};
    std::size_t strokeCount{};
    std::chrono::milliseconds estimatedDuration{};
    std::chrono::milliseconds minimumButtonDownInterval{600};
    std::vector<MouseCommand> commands;
};

struct ExecutionOptions {
    float mouseScale{1.0F};
    int maximumMouseStep{2};
    std::chrono::milliseconds moveInterval{16};
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

} // namespace vrcdraw
