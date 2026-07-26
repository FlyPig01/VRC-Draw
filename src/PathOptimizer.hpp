#pragma once

#include "PathTypes.hpp"

namespace vrcdraw {

[[nodiscard]] bool HaveIdenticalPathSegments(
    const DrawingPath& baseline,
    const DrawingPath& candidate);

[[nodiscard]] bool HaveConsistentRouteMetadata(const DrawingPath& path);

[[nodiscard]] DrawingPath OptimizeDrawingPathLossless(DrawingPath baseline);

} // namespace vrcdraw
