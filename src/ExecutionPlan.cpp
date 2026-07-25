#include "ExecutionPlan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vrcdraw {
namespace {

void AppendWait(
    ExecutionPlan& plan,
    const std::chrono::milliseconds duration)
{
    if (duration.count() <= 0) {
        return;
    }
    plan.commands.push_back(MouseCommand{
        .type = MouseCommandType::Wait,
        .duration = duration,
    });
    plan.estimatedDuration += duration;
}

void AppendMove(
    ExecutionPlan& plan,
    MousePoint& current,
    const MousePoint target,
    const int maximumStep,
    const std::chrono::milliseconds moveInterval)
{
    for (const MouseDelta delta :
         InterpolateMouseLine(current, target, maximumStep)) {
        plan.commands.push_back(MouseCommand{
            .type = MouseCommandType::Move,
            .dx = delta.dx,
            .dy = delta.dy,
        });
        current.x += delta.dx;
        current.y += delta.dy;
        AppendWait(plan, moveInterval);
    }
}

} // namespace

std::vector<MouseDelta> InterpolateMouseLine(
    const MousePoint start,
    const MousePoint target,
    const int maximumStep)
{
    const int safeMaximumStep = std::max(1, maximumStep);
    const int deltaX = target.x - start.x;
    const int deltaY = target.y - start.y;
    const int maximumDistance = std::max(std::abs(deltaX), std::abs(deltaY));
    if (maximumDistance == 0) {
        return {};
    }

    const int steps = std::max(
        1,
        static_cast<int>(std::ceil(
            static_cast<double>(maximumDistance) / static_cast<double>(safeMaximumStep))));

    std::vector<MouseDelta> result;
    result.reserve(static_cast<std::size_t>(steps));
    MousePoint previous = start;

    for (int step = 1; step <= steps; ++step) {
        const double ratio = static_cast<double>(step) / static_cast<double>(steps);
        const MousePoint current{
            static_cast<int>(std::lround(start.x + static_cast<double>(deltaX) * ratio)),
            static_cast<int>(std::lround(start.y + static_cast<double>(deltaY) * ratio)),
        };
        const MouseDelta delta{current.x - previous.x, current.y - previous.y};
        if (delta.dx != 0 || delta.dy != 0) {
            result.push_back(delta);
        }
        previous = current;
    }
    return result;
}

ExecutionPlan BuildExecutionPlan(
    const DrawingPath& path,
    const ExecutionOptions& options)
{
    const auto minimumButtonDownInterval = std::max(
        std::chrono::milliseconds{}, options.minimumButtonDownInterval);
    ExecutionPlan plan{
        .canvasWidth = static_cast<int>(path.width),
        .canvasHeight = static_cast<int>(path.height),
        .mouseScale = options.mouseScale,
        .minimumButtonDownInterval = minimumButtonDownInterval,
        .commands = {},
    };
    MousePoint current{};
    bool hasPreviousButtonDown = false;
    std::chrono::milliseconds previousButtonDownAt{};
    const float centerX = static_cast<float>(path.width) * 0.5F;
    const float centerY = static_cast<float>(path.height) * 0.5F;

    const auto convert = [&](const PointF point) {
        return MousePoint{
            static_cast<int>(std::lround((point.x - centerX) * options.mouseScale)),
            static_cast<int>(std::lround((point.y - centerY) * options.mouseScale)),
        };
    };

    for (const Stroke& stroke : path.strokes) {
        if (stroke.size() < 2) {
            continue;
        }

        const MousePoint start = convert(stroke.front());
        AppendMove(
            plan,
            current,
            start,
            options.maximumMouseStep,
            options.moveInterval);
        AppendWait(plan, options.beforeButtonDown);
        if (hasPreviousButtonDown) {
            const auto elapsed = plan.estimatedDuration - previousButtonDownAt;
            if (elapsed < minimumButtonDownInterval) {
                AppendWait(plan, minimumButtonDownInterval - elapsed);
            }
        }
        plan.commands.push_back(MouseCommand{.type = MouseCommandType::LeftDown});
        previousButtonDownAt = plan.estimatedDuration;
        hasPreviousButtonDown = true;
        AppendWait(plan, options.afterButtonDown);

        for (std::size_t pointIndex = 1; pointIndex < stroke.size(); ++pointIndex) {
            AppendMove(
                plan,
                current,
                convert(stroke[pointIndex]),
                options.maximumMouseStep,
                options.moveInterval);
        }

        plan.commands.push_back(MouseCommand{.type = MouseCommandType::LeftUp});
        AppendWait(plan, options.afterButtonUp);
        ++plan.strokeCount;
    }
    return plan;
}

} // namespace vrcdraw
