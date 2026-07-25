#include "PathMath.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace vrcdraw {
namespace {

void SimplifyRange(
    const Stroke& points,
    const std::size_t first,
    const std::size_t last,
    const float epsilon,
    std::vector<bool>& keep)
{
    if (last <= first + 1) {
        return;
    }

    float maximumDistance = 0.0F;
    std::size_t maximumIndex = first;

    for (std::size_t index = first + 1; index < last; ++index) {
        const float distance = PointSegmentDistance(points[index], points[first], points[last]);
        if (distance > maximumDistance) {
            maximumDistance = distance;
            maximumIndex = index;
        }
    }

    if (maximumDistance <= epsilon) {
        return;
    }

    keep[maximumIndex] = true;
    SimplifyRange(points, first, maximumIndex, epsilon, keep);
    SimplifyRange(points, maximumIndex, last, epsilon, keep);
}

} // namespace

float PointSegmentDistance(const PointF point, const PointF start, const PointF end)
{
    const float deltaX = end.x - start.x;
    const float deltaY = end.y - start.y;
    const float lengthSquared = (deltaX * deltaX) + (deltaY * deltaY);

    if (lengthSquared <= 1.0e-8F) {
        return std::hypot(point.x - start.x, point.y - start.y);
    }

    const float projection = std::clamp(
        (((point.x - start.x) * deltaX) + ((point.y - start.y) * deltaY)) / lengthSquared,
        0.0F,
        1.0F);

    const PointF closest{
        start.x + (projection * deltaX),
        start.y + (projection * deltaY),
    };

    return std::hypot(point.x - closest.x, point.y - closest.y);
}

Stroke SimplifyRdp(const Stroke& points, const float epsilon)
{
    if (points.size() <= 2) {
        return points;
    }

    std::vector<bool> keep(points.size(), false);
    keep.front() = true;
    keep.back() = true;
    SimplifyRange(points, 0, points.size() - 1, epsilon, keep);

    Stroke simplified;
    simplified.reserve(points.size());
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (keep[index]) {
            simplified.push_back(points[index]);
        }
    }
    return simplified;
}

} // namespace vrcdraw

