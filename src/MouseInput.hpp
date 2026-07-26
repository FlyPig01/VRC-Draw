#pragma once

#include "ExecutionPlan.hpp"

#include <span>

namespace vrcdraw {

enum class MouseInputMode {
    Undetermined,
    Relative,
    DesktopAbsolute,
};

struct DesktopRect {
    int left{};
    int top{};
    int width{};
    int height{};
};

[[nodiscard]] int RecommendedProbeDistance(int systemPointerSpeed);

[[nodiscard]] MouseInputMode ClassifyCursorBehavior(
    MousePoint origin,
    MousePoint injectedOffset,
    std::span<const MousePoint> samples);

[[nodiscard]] bool FitsDesktopRect(
    MousePoint anchor,
    MousePoint minimumOffset,
    MousePoint maximumOffset,
    DesktopRect desktop);

[[nodiscard]] MousePoint NormalizeDesktopPoint(
    MousePoint screenPoint,
    DesktopRect desktop);

} // namespace vrcdraw
