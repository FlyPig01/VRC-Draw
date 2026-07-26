#pragma once

#include "PathTypes.hpp"

namespace vrcdraw {

struct VectorFittingOptions {
    float lineTolerance{0.45F};
    float curveTolerance{0.85F};
    float flatteningTolerance{0.25F};
    float cornerArmLength{4.0F};
    float cornerCosineThreshold{-0.45F};
};

[[nodiscard]] VectorDrawingPath FitVectorDrawingPath(
    const DrawingPath& path,
    const BinaryImage& allowedInk,
    const VectorFittingOptions& options = {});

[[nodiscard]] Stroke FlattenFittedRouteSpan(
    const FittedRouteSpan& span,
    float tolerance = 0.25F);

[[nodiscard]] DrawingPath FlattenVectorDrawingPath(
    const VectorDrawingPath& path,
    float tolerance = 0.25F);

} // namespace vrcdraw
