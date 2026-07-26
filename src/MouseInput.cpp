#include "MouseInput.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vrcdraw {
namespace {

int MaximumDistance(const MousePoint first, const MousePoint second)
{
    return std::max(std::abs(first.x - second.x), std::abs(first.y - second.y));
}

} // namespace

int RecommendedProbeDistance(const int systemPointerSpeed)
{
    const int safeSpeed = std::clamp(systemPointerSpeed, 1, 20);
    return std::clamp((96 + safeSpeed - 1) / safeSpeed, 8, 96);
}

MouseInputMode ClassifyCursorBehavior(
    const MousePoint origin,
    const MousePoint injectedOffset,
    const std::span<const MousePoint> samples)
{
    constexpr int returnTolerance = 2;
    constexpr int stableTolerance = 2;
    constexpr int minimumMovement = 3;
    if (samples.size() < 2) {
        return MouseInputMode::Undetermined;
    }

    const MousePoint last = samples.back();
    const MousePoint previous = samples[samples.size() - 2];
    const MousePoint displacement{last.x - origin.x, last.y - origin.y};
    if (MaximumDistance(last, origin) <= returnTolerance) {
        return MouseInputMode::Relative;
    }
    if (MaximumDistance(last, origin) < minimumMovement ||
        MaximumDistance(last, previous) > stableTolerance) {
        return MouseInputMode::Undetermined;
    }

    const std::int64_t alignment =
        static_cast<std::int64_t>(displacement.x) * injectedOffset.x +
        static_cast<std::int64_t>(displacement.y) * injectedOffset.y;
    return alignment > 0
        ? MouseInputMode::DesktopAbsolute
        : MouseInputMode::Relative;
}

bool FitsDesktopRect(
    const MousePoint anchor,
    const MousePoint minimumOffset,
    const MousePoint maximumOffset,
    const DesktopRect desktop)
{
    if (desktop.width <= 0 || desktop.height <= 0) {
        return false;
    }
    const std::int64_t right =
        static_cast<std::int64_t>(desktop.left) + desktop.width - 1;
    const std::int64_t bottom =
        static_cast<std::int64_t>(desktop.top) + desktop.height - 1;
    return static_cast<std::int64_t>(anchor.x) + minimumOffset.x >= desktop.left &&
           static_cast<std::int64_t>(anchor.y) + minimumOffset.y >= desktop.top &&
           static_cast<std::int64_t>(anchor.x) + maximumOffset.x <= right &&
           static_cast<std::int64_t>(anchor.y) + maximumOffset.y <= bottom;
}

MousePoint NormalizeDesktopPoint(
    const MousePoint screenPoint,
    const DesktopRect desktop)
{
    if (desktop.width <= 1 || desktop.height <= 1) {
        return {};
    }
    const auto normalize = [](const int value, const int origin, const int extent) {
        const double ratio = static_cast<double>(value - origin) /
            static_cast<double>(extent - 1);
        return static_cast<int>(std::lround(std::clamp(ratio, 0.0, 1.0) * 65535.0));
    };
    return {
        normalize(screenPoint.x, desktop.left, desktop.width),
        normalize(screenPoint.y, desktop.top, desktop.height),
    };
}

} // namespace vrcdraw
