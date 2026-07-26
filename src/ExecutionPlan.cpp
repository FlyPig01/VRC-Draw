#include "ExecutionPlan.hpp"

#include "PathMath.hpp"
#include "VectorPath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <ranges>
#include <utility>

namespace vrcdraw {
namespace {

void AppendWait(
    ExecutionPlan& plan,
    const std::chrono::milliseconds duration,
    std::chrono::milliseconds* const category = nullptr)
{
    if (duration.count() <= 0) {
        return;
    }
    plan.commands.push_back(MouseCommand{
        .type = MouseCommandType::Wait,
        .duration = duration,
    });
    plan.estimatedDuration += duration;
    if (category != nullptr) {
        *category += duration;
    }
}

void IncludeDrawingPoint(ExecutionPlan& plan, const MousePoint point)
{
    if (!plan.hasDrawingPosition) {
        plan.minimumDrawingPosition = point;
        plan.maximumDrawingPosition = point;
        plan.hasDrawingPosition = true;
        return;
    }
    plan.minimumDrawingPosition.x = std::min(plan.minimumDrawingPosition.x, point.x);
    plan.minimumDrawingPosition.y = std::min(plan.minimumDrawingPosition.y, point.y);
    plan.maximumDrawingPosition.x = std::max(plan.maximumDrawingPosition.x, point.x);
    plan.maximumDrawingPosition.y = std::max(plan.maximumDrawingPosition.y, point.y);
}

void AppendMove(
    ExecutionPlan& plan,
    MousePoint& current,
    const MousePoint target,
    const int maximumStep,
    const std::chrono::milliseconds moveInterval,
    const bool penDown)
{
    for (const MouseDelta delta :
         InterpolateMouseLine(current, target, maximumStep)) {
        plan.commands.push_back(MouseCommand{
            .type = MouseCommandType::Move,
            .dx = delta.dx,
            .dy = delta.dy,
        });
        if (penDown) {
            ++plan.metrics.penDownMoves;
        } else {
            ++plan.metrics.penUpMoves;
        }
        current.x += delta.dx;
        current.y += delta.dy;
        if (penDown) {
            IncludeDrawingPoint(plan, current);
        }
        plan.minimumMousePosition.x = std::min(plan.minimumMousePosition.x, current.x);
        plan.minimumMousePosition.y = std::min(plan.minimumMousePosition.y, current.y);
        plan.maximumMousePosition.x = std::max(plan.maximumMousePosition.x, current.x);
        plan.maximumMousePosition.y = std::max(plan.maximumMousePosition.y, current.y);
        AppendWait(
            plan,
            moveInterval,
            penDown ? &plan.metrics.penDownMoveTime : &plan.metrics.penUpMoveTime);
    }
}

int CurvatureStep(
    const Stroke& stroke,
    const std::size_t pointIndex,
    const std::size_t sourceEdgeStart,
    const std::size_t sourceEdgeEnd,
    const ExecutionOptions& options)
{
    const int maximumStep = std::max(1, options.maximumPenDownStep);
    const int minimumStep = std::clamp(options.minimumPenDownStep, 1, maximumStep);
    if (pointIndex <= sourceEdgeStart || pointIndex >= sourceEdgeEnd ||
        pointIndex + 1 >= stroke.size()) {
        return maximumStep;
    }

    const PointF previous = stroke[pointIndex - 1];
    const PointF current = stroke[pointIndex];
    const PointF next = stroke[pointIndex + 1];
    const float incomingX = current.x - previous.x;
    const float incomingY = current.y - previous.y;
    const float outgoingX = next.x - current.x;
    const float outgoingY = next.y - current.y;
    const float denominator =
        std::hypot(incomingX, incomingY) * std::hypot(outgoingX, outgoingY);
    if (denominator <= 1.0e-5F) {
        return minimumStep;
    }

    const float directionCosine =
        (incomingX * outgoingX + incomingY * outgoingY) / denominator;
    if (directionCosine < 0.55F) {
        return minimumStep;
    }
    if (directionCosine < 0.9F) {
        return std::max(minimumStep, maximumStep - 1);
    }
    return maximumStep;
}

int AdaptivePenDownStep(
    const Stroke& stroke,
    const std::size_t targetIndex,
    const std::size_t sourceEdgeStart,
    const std::size_t sourceEdgeEnd,
    const ExecutionOptions& options)
{
    return std::min(
        CurvatureStep(
            stroke, targetIndex - 1, sourceEdgeStart, sourceEdgeEnd, options),
        CurvatureStep(
            stroke, targetIndex, sourceEdgeStart, sourceEdgeEnd, options));
}

using MousePolyline = std::vector<MousePoint>;

MousePolyline ConvertPolyline(
    const Stroke& points,
    const float centerX,
    const float centerY,
    const float scale)
{
    MousePolyline result;
    result.reserve(points.size());
    for (const PointF point : points) {
        const MousePoint converted{
            static_cast<int>(std::lround((point.x - centerX) * scale)),
            static_cast<int>(std::lround((point.y - centerY) * scale)),
        };
        if (result.empty() || result.back() != converted) {
            result.push_back(converted);
        }
    }
    return result;
}

float MousePointSegmentDistance(
    const MousePoint point,
    const MousePoint start,
    const MousePoint end)
{
    return PointSegmentDistance(
        PointF{static_cast<float>(point.x), static_cast<float>(point.y)},
        PointF{static_cast<float>(start.x), static_cast<float>(start.y)},
        PointF{static_cast<float>(end.x), static_cast<float>(end.y)});
}

float MousePointPolylineDistance(
    const MousePoint point,
    const MousePolyline& polyline)
{
    if (polyline.empty()) {
        return std::numeric_limits<float>::infinity();
    }
    if (polyline.size() == 1) {
        return std::hypot(
            static_cast<float>(point.x - polyline.front().x),
            static_cast<float>(point.y - polyline.front().y));
    }
    float distance = std::numeric_limits<float>::infinity();
    for (std::size_t index = 1; index < polyline.size(); ++index) {
        distance = std::min(
            distance,
            MousePointSegmentDistance(point, polyline[index - 1], polyline[index]));
    }
    return distance;
}

float MouseBidirectionalDistance(
    const MousePolyline& left,
    const MousePolyline& right)
{
    float distance = 0.0F;
    for (const MousePoint point : left) {
        distance = std::max(distance, MousePointPolylineDistance(point, right));
    }
    for (const MousePoint point : right) {
        distance = std::max(distance, MousePointPolylineDistance(point, left));
    }
    return distance;
}

int MouseOrientation(
    const MousePoint a,
    const MousePoint b,
    const MousePoint c)
{
    const std::int64_t value =
        static_cast<std::int64_t>(b.x - a.x) * (c.y - a.y) -
        static_cast<std::int64_t>(b.y - a.y) * (c.x - a.x);
    return value == 0 ? 0 : (value > 0 ? 1 : -1);
}

std::size_t MouseSelfIntersections(const MousePolyline& points)
{
    std::size_t count = 0;
    for (std::size_t left = 1; left < points.size(); ++left) {
        for (std::size_t right = left + 2; right < points.size(); ++right) {
            if (points.front() == points.back() && left == 1 &&
                right + 1 == points.size()) {
                continue;
            }
            const int firstLeft = MouseOrientation(
                points[left - 1], points[left], points[right - 1]);
            const int firstRight = MouseOrientation(
                points[left - 1], points[left], points[right]);
            const int secondLeft = MouseOrientation(
                points[right - 1], points[right], points[left - 1]);
            const int secondRight = MouseOrientation(
                points[right - 1], points[right], points[left]);
            count += firstLeft * firstRight < 0 && secondLeft * secondRight < 0;
        }
    }
    return count;
}

std::size_t DirectionReversalCount(const MousePolyline& points)
{
    if (points.size() < 3) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t index = 2; index < points.size(); ++index) {
        const int leftX = points[index - 1].x - points[index - 2].x;
        const int leftY = points[index - 1].y - points[index - 2].y;
        const int rightX = points[index].x - points[index - 1].x;
        const int rightY = points[index].y - points[index - 1].y;
        count += (leftX != 0 && rightX != 0 && ((leftX < 0) != (rightX < 0))) ||
                 (leftY != 0 && rightY != 0 && ((leftY < 0) != (rightY < 0)));
    }
    return count;
}

std::size_t DiscreteCurvatureSpikeCount(const MousePolyline& points)
{
    std::size_t count = 0;
    for (std::size_t index = 2; index < points.size(); ++index) {
        const float incomingX = static_cast<float>(
            points[index - 1].x - points[index - 2].x);
        const float incomingY = static_cast<float>(
            points[index - 1].y - points[index - 2].y);
        const float outgoingX = static_cast<float>(
            points[index].x - points[index - 1].x);
        const float outgoingY = static_cast<float>(
            points[index].y - points[index - 1].y);
        const float denominator = std::hypot(incomingX, incomingY) *
                                  std::hypot(outgoingX, outgoingY);
        if (denominator > 1.0e-5F &&
            (incomingX * outgoingX + incomingY * outgoingY) / denominator < 0.35F) {
            ++count;
        }
    }
    return count;
}

bool QuantizedPolylineSupported(
    const MousePolyline& points,
    const BinaryImage& ink,
    const float centerX,
    const float centerY,
    const float scale)
{
    if (points.empty() || scale <= 0.0F || ink.width == 0 || ink.height == 0 ||
        ink.pixels.size() != static_cast<std::size_t>(ink.width) * ink.height) {
        return false;
    }
    const auto supported = [&](const MousePoint point) {
        const float imageX = static_cast<float>(point.x) / scale + centerX;
        const float imageY = static_cast<float>(point.y) / scale + centerY;
        const int x = static_cast<int>(std::lround(imageX));
        const int y = static_cast<int>(std::lround(imageY));
        for (int offsetY = -1; offsetY <= 1; ++offsetY) {
            for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                const int sampleX = x + offsetX;
                const int sampleY = y + offsetY;
                if (sampleX >= 0 && sampleY >= 0 &&
                    sampleX < static_cast<int>(ink.width) &&
                    sampleY < static_cast<int>(ink.height) &&
                    ink.pixels[static_cast<std::size_t>(sampleY) * ink.width +
                               static_cast<std::size_t>(sampleX)] != 0) {
                    return true;
                }
            }
        }
        return false;
    };
    if (!supported(points.front())) {
        return false;
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        MousePoint current = points[index - 1];
        for (const MouseDelta delta : InterpolateMouseLine(current, points[index], 1)) {
            current.x += delta.dx;
            current.y += delta.dy;
            if (!supported(current)) {
                return false;
            }
        }
    }
    return true;
}

bool ScaleCandidateIsSafe(
    const Stroke& candidate,
    const Stroke& fallback,
    const BinaryImage& ink,
    const float centerX,
    const float centerY,
    const float scale)
{
    const MousePolyline candidateMouse =
        ConvertPolyline(candidate, centerX, centerY, scale);
    const MousePolyline fallbackMouse =
        ConvertPolyline(fallback, centerX, centerY, scale);
    if (candidateMouse.empty() || fallbackMouse.empty() ||
        candidateMouse.front() != fallbackMouse.front() ||
        candidateMouse.back() != fallbackMouse.back()) {
        return false;
    }
    if (candidateMouse.size() == 1 && fallbackMouse.size() == 1) {
        return true;
    }
    const float maximumDeviation = std::max(1.0F, 0.9F * scale);
    return MouseBidirectionalDistance(candidateMouse, fallbackMouse) <= maximumDeviation &&
           MouseSelfIntersections(candidateMouse) <= MouseSelfIntersections(fallbackMouse) &&
           DirectionReversalCount(candidateMouse) <= DirectionReversalCount(fallbackMouse) &&
           DiscreteCurvatureSpikeCount(candidateMouse) <=
               DiscreteCurvatureSpikeCount(fallbackMouse) &&
           QuantizedPolylineSupported(
               candidateMouse, ink, centerX, centerY, scale);
}

DrawingPath BuildScaleValidatedPath(
    const VectorDrawingPath& path,
    const BinaryImage& allowedInk,
    const float effectiveScale,
    ExecutionPlanMetrics& vectorMetrics)
{
    DrawingPath result{
        .width = path.width,
        .height = path.height,
        .strokes = {},
        .sourceEdgeEnds = {},
        .optimization = path.optimization,
        .routeMetadata = {},
    };
    const float centerX = static_cast<float>(path.width) * 0.5F;
    const float centerY = static_cast<float>(path.height) * 0.5F;
    const float sourceTolerance = std::clamp(
        0.25F / std::max(effectiveScale, 0.01F), 0.08F, 0.75F);
    result.strokes.reserve(path.strokes.size());
    result.sourceEdgeEnds.reserve(path.strokes.size());
    result.routeMetadata.reserve(path.strokes.size());
    for (const VectorStroke& vectorStroke : path.strokes) {
        Stroke stroke;
        std::vector<std::size_t> sourceEnds;
        StrokeRouteMetadata metadata{.spans = {}, .closed = vectorStroke.closed};
        bool validStroke = true;
        for (const FittedRouteSpan& span : vectorStroke.spans) {
            Stroke selected = span.fallbackPolyline;
            if (span.fitted) {
                ++vectorMetrics.fittedSpanCount;
                Stroke candidate = FlattenFittedRouteSpan(span, sourceTolerance);
                if (ScaleCandidateIsSafe(
                        candidate,
                        span.fallbackPolyline,
                        allowedInk,
                        centerX,
                        centerY,
                        effectiveScale)) {
                    selected = std::move(candidate);
                    ++vectorMetrics.acceptedFittedSpanCount;
                } else {
                    ++vectorMetrics.scaleFallbackSpanCount;
                }
            }
            if (selected.size() < 2) {
                continue;
            }
            const std::size_t offset = stroke.empty() ? 0 : stroke.size() - 1;
            if (stroke.empty()) {
                stroke = std::move(selected);
            } else if (stroke.back() == selected.front()) {
                stroke.insert(stroke.end(), selected.begin() + 1, selected.end());
            } else {
                validStroke = false;
                break;
            }
            metadata.spans.push_back(RouteSpan{
                .pointBegin = offset,
                .pointEnd = stroke.size() - 1,
                .regionType = span.regionType,
                .beginAnchorFlags = span.beginAnchorFlags,
                .endAnchorFlags = span.endAnchorFlags,
            });
            sourceEnds.push_back(stroke.size() - 1);
        }
        if (validStroke && stroke.size() >= 2) {
            result.strokes.push_back(std::move(stroke));
            result.sourceEdgeEnds.push_back(std::move(sourceEnds));
            result.routeMetadata.push_back(std::move(metadata));
        }
    }
    return result;
}

} // namespace

std::vector<MouseDelta> InterpolateMouseLine(
    const MousePoint start,
    const MousePoint target,
    const int maximumStep)
{
    const int safeMaximumStep = std::max(1, maximumStep);
    const bool reverse = std::pair{target.x, target.y} < std::pair{start.x, start.y};
    const MousePoint canonicalStart = reverse ? target : start;
    const MousePoint canonicalTarget = reverse ? start : target;
    const int deltaX = canonicalTarget.x - canonicalStart.x;
    const int deltaY = canonicalTarget.y - canonicalStart.y;
    const int maximumDistance = std::max(std::abs(deltaX), std::abs(deltaY));
    if (maximumDistance == 0) {
        return {};
    }

    const int steps = std::max(
        1,
        static_cast<int>(std::ceil(
            static_cast<double>(maximumDistance) / static_cast<double>(safeMaximumStep))));

    std::vector<MousePoint> points;
    points.reserve(static_cast<std::size_t>(steps) + 1);
    points.push_back(canonicalStart);

    for (int step = 1; step <= steps; ++step) {
        const double ratio = static_cast<double>(step) / static_cast<double>(steps);
        points.push_back(MousePoint{
            static_cast<int>(std::lround(
                canonicalStart.x + static_cast<double>(deltaX) * ratio)),
            static_cast<int>(std::lround(
                canonicalStart.y + static_cast<double>(deltaY) * ratio)),
        });
    }
    if (reverse) {
        std::ranges::reverse(points);
    }

    std::vector<MouseDelta> result;
    result.reserve(static_cast<std::size_t>(steps));
    MousePoint previous = points.front();
    for (std::size_t index = 1; index < points.size(); ++index) {
        const MousePoint current = points[index];
        const MouseDelta delta{current.x - previous.x, current.y - previous.y};
        if (delta.dx != 0 || delta.dy != 0) {
            result.push_back(delta);
        };
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
    const int pathMaximumDimension = static_cast<int>(std::max(path.width, path.height));
    const float extentScale = options.maximumDrawingDimension > 0 && pathMaximumDimension > 0
        ? std::min(
              1.0F,
              static_cast<float>(options.maximumDrawingDimension) /
                  static_cast<float>(pathMaximumDimension))
        : 1.0F;
    const float requestedMouseScale = std::clamp(options.mouseScale, 0.3F, 3.0F);
    const float effectiveMouseScale = requestedMouseScale * extentScale;
    ExecutionPlan plan{
        .canvasWidth = static_cast<int>(path.width),
        .canvasHeight = static_cast<int>(path.height),
        .requestedMouseScale = requestedMouseScale,
        .mouseScale = effectiveMouseScale,
        .minimumButtonDownInterval = minimumButtonDownInterval,
        .commands = {},
        .remainingDurationByCommand = {},
        .metrics = ExecutionPlanMetrics{
            .sourceSegments = path.optimization.sourceSegments,
            .strokesBeforeOptimization = path.optimization.strokesBefore != 0
                ? path.optimization.strokesBefore
                : path.strokes.size(),
            .strokesAfterOptimization = path.strokes.size(),
        },
    };
    MousePoint current{};
    bool hasPreviousButtonDown = false;
    std::chrono::milliseconds previousButtonDownAt{};
    const float centerX = static_cast<float>(path.width) * 0.5F;
    const float centerY = static_cast<float>(path.height) * 0.5F;

    const auto convert = [&](const PointF point) {
        return MousePoint{
            static_cast<int>(std::lround((point.x - centerX) * effectiveMouseScale)),
            static_cast<int>(std::lround((point.y - centerY) * effectiveMouseScale)),
        };
    };

    for (std::size_t strokeIndex = 0; strokeIndex < path.strokes.size(); ++strokeIndex) {
        const Stroke& stroke = path.strokes[strokeIndex];
        if (stroke.size() < 2) {
            continue;
        }

        const MousePoint start = convert(stroke.front());
        IncludeDrawingPoint(plan, start);
        AppendMove(
            plan,
            current,
            start,
            options.maximumPenUpStep,
            options.penUpMoveInterval,
            false);
        AppendWait(plan, options.beforeButtonDown, &plan.metrics.fixedButtonWait);
        if (hasPreviousButtonDown) {
            const auto elapsed = plan.estimatedDuration - previousButtonDownAt;
            if (elapsed < minimumButtonDownInterval) {
                AppendWait(
                    plan,
                    minimumButtonDownInterval - elapsed,
                    &plan.metrics.buttonGuardWait);
            }
        }
        plan.commands.push_back(MouseCommand{.type = MouseCommandType::LeftDown});
        ++plan.metrics.leftDownCount;
        previousButtonDownAt = plan.estimatedDuration;
        hasPreviousButtonDown = true;
        AppendWait(plan, options.afterButtonDown, &plan.metrics.fixedButtonWait);

        for (std::size_t pointIndex = 1; pointIndex < stroke.size(); ++pointIndex) {
            std::size_t sourceEdgeStart = 0;
            std::size_t sourceEdgeEnd = stroke.size() - 1;
            if (strokeIndex < path.sourceEdgeEnds.size() &&
                !path.sourceEdgeEnds[strokeIndex].empty()) {
                const auto& ends = path.sourceEdgeEnds[strokeIndex];
                const auto edge = std::ranges::lower_bound(ends, pointIndex);
                if (edge != ends.end()) {
                    sourceEdgeEnd = *edge;
                    if (edge != ends.begin()) {
                        sourceEdgeStart = *(edge - 1);
                    }
                }
            }
            AppendMove(
                plan,
                current,
                convert(stroke[pointIndex]),
                AdaptivePenDownStep(
                    stroke,
                    pointIndex,
                    sourceEdgeStart,
                    sourceEdgeEnd,
                    options),
                options.penDownMoveInterval,
                true);
        }

        plan.commands.push_back(MouseCommand{.type = MouseCommandType::LeftUp});
        AppendWait(plan, options.afterButtonUp, &plan.metrics.fixedButtonWait);
        ++plan.strokeCount;
    }
    plan.metrics.totalTime = plan.estimatedDuration;
    plan.remainingDurationByCommand.assign(
        plan.commands.size() + 1, std::chrono::milliseconds{});
    for (std::size_t index = plan.commands.size(); index-- > 0;) {
        plan.remainingDurationByCommand[index] = plan.remainingDurationByCommand[index + 1];
        if (plan.commands[index].type == MouseCommandType::Wait) {
            plan.remainingDurationByCommand[index] += plan.commands[index].duration;
        }
    }
    return plan;
}

ExecutionPlan BuildExecutionPlan(
    const VectorDrawingPath& path,
    const BinaryImage& allowedInk,
    const ExecutionOptions& options)
{
    const int pathMaximumDimension = static_cast<int>(std::max(path.width, path.height));
    const float extentScale = options.maximumDrawingDimension > 0 && pathMaximumDimension > 0
        ? std::min(
              1.0F,
              static_cast<float>(options.maximumDrawingDimension) /
                  static_cast<float>(pathMaximumDimension))
        : 1.0F;
    const float requestedMouseScale = std::clamp(options.mouseScale, 0.3F, 3.0F);
    const float effectiveMouseScale = requestedMouseScale * extentScale;
    ExecutionPlanMetrics vectorMetrics{};
    const DrawingPath sampled = BuildScaleValidatedPath(
        path, allowedInk, effectiveMouseScale, vectorMetrics);
    ExecutionPlan plan = BuildExecutionPlan(sampled, options);
    plan.metrics.fittedSpanCount = vectorMetrics.fittedSpanCount;
    plan.metrics.acceptedFittedSpanCount = vectorMetrics.acceptedFittedSpanCount;
    plan.metrics.scaleFallbackSpanCount = vectorMetrics.scaleFallbackSpanCount;
    plan.metrics.finalPenDownSamplePoints =
        plan.metrics.penDownMoves + plan.strokeCount;
    return plan;
}

std::chrono::milliseconds RemainingPlannedDuration(
    const ExecutionPlan& plan,
    const std::size_t completedCommands)
{
    if (plan.remainingDurationByCommand.size() == plan.commands.size() + 1) {
        return plan.remainingDurationByCommand[
            std::min(completedCommands, plan.commands.size())];
    }
    std::chrono::milliseconds remaining{};
    for (std::size_t index = std::min(completedCommands, plan.commands.size());
         index < plan.commands.size();
         ++index) {
        if (plan.commands[index].type == MouseCommandType::Wait) {
            remaining += plan.commands[index].duration;
        }
    }
    return remaining;
}

float PlannedProgress(
    const ExecutionPlan& plan,
    const std::size_t completedCommands)
{
    if (plan.estimatedDuration.count() <= 0) {
        return plan.commands.empty() || completedCommands == 0 ? 0.0F : 1.0F;
    }
    const auto remaining = RemainingPlannedDuration(plan, completedCommands);
    return std::clamp(
        1.0F - static_cast<float>(remaining.count()) /
                     static_cast<float>(plan.estimatedDuration.count()),
        0.0F,
        1.0F);
}

std::chrono::milliseconds RemainingPlannedDuration(
    const ExecutionPlan& plan,
    const std::chrono::milliseconds completedDuration)
{
    return std::max(
        std::chrono::milliseconds{}, plan.estimatedDuration - completedDuration);
}

float PlannedProgress(
    const ExecutionPlan& plan,
    const std::chrono::milliseconds completedDuration)
{
    if (plan.estimatedDuration.count() <= 0) {
        return completedDuration.count() > 0 ? 1.0F : 0.0F;
    }
    return std::clamp(
        static_cast<float>(completedDuration.count()) /
            static_cast<float>(plan.estimatedDuration.count()),
        0.0F,
        1.0F);
}

} // namespace vrcdraw
