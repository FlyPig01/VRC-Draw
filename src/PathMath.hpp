#pragma once

#include "PathTypes.hpp"

#include <vector>

namespace vrcdraw {

[[nodiscard]] float PointSegmentDistance(PointF point, PointF start, PointF end);
[[nodiscard]] Stroke SimplifyRdp(const Stroke& points, float epsilon);

} // namespace vrcdraw

