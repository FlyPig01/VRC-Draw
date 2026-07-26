#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace vrcdraw {

struct PointF {
    float x{};
    float y{};

    friend bool operator==(const PointF&, const PointF&) = default;
};

using Stroke = std::vector<PointF>;

enum class LineRegionType : std::uint8_t {
    ThinLine,
    ElongatedThickStroke,
    CompactFill,
    Junction,
    Ambiguous,
};

enum RouteAnchorFlags : std::uint8_t {
    AnchorNone = 0,
    AnchorGraphEndpoint = 1U << 0U,
    AnchorJunctionPort = 1U << 1U,
    AnchorRegionBoundary = 1U << 2U,
    AnchorClosedSeam = 1U << 3U,
    AnchorProtectedCorner = 1U << 4U,
};

struct RouteSpan {
    // Inclusive point indexes in the optimized Stroke. Consecutive spans share
    // their boundary point so every source segment belongs to exactly one span.
    std::size_t pointBegin{};
    std::size_t pointEnd{};
    LineRegionType regionType{LineRegionType::ThinLine};
    std::uint8_t beginAnchorFlags{AnchorNone};
    std::uint8_t endAnchorFlags{AnchorNone};
};

struct StrokeRouteMetadata {
    std::vector<RouteSpan> spans;
    bool closed{};
};

struct PathOptimizationStats {
    std::size_t strokesBefore{};
    std::size_t strokesAfter{};
    std::size_t sourceSegments{};
    float penUpDistanceBefore{};
    float penUpDistanceAfter{};
    std::int64_t estimatedMillisecondsBefore{};
    std::int64_t estimatedMillisecondsAfter{};
    bool exactSegmentMatch{};
    bool exactExecutionMatch{};
    bool applied{};
};

struct DrawingPath {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Stroke> strokes;
    std::vector<std::vector<std::size_t>> sourceEdgeEnds{};
    PathOptimizationStats optimization{};
    std::vector<StrokeRouteMetadata> routeMetadata{};
};

// 0 is background, 1 is a drawable black line pixel.
struct BinaryImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels;
};

enum class VectorSegmentType : std::uint8_t {
    Line,
    CubicBezier,
};

struct CubicBezier {
    PointF p0;
    PointF p1;
    PointF p2;
    PointF p3;
};

struct VectorSegment {
    VectorSegmentType type{VectorSegmentType::Line};
    PointF lineEnd{};
    CubicBezier cubic{};
};

struct FittedRouteSpan {
    PointF start{};
    std::vector<VectorSegment> segments;
    Stroke fallbackPolyline;
    LineRegionType regionType{LineRegionType::ThinLine};
    std::uint8_t beginAnchorFlags{AnchorNone};
    std::uint8_t endAnchorFlags{AnchorNone};
    bool fitted{};
};

struct VectorStroke {
    std::vector<FittedRouteSpan> spans;
    bool closed{};
};

struct VectorFitStats {
    std::size_t sourceSpans{};
    std::size_t cornerSplitSpans{};
    std::size_t fittedSpans{};
    std::size_t fallbackSpans{};
    std::size_t vectorSegments{};
    std::size_t sourcePolylineSegments{};
};

struct VectorDrawingPath {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<VectorStroke> strokes;
    PathOptimizationStats optimization{};
    VectorFitStats fitting{};
};

// 0 is white background, 255 is fully black ink, intermediate values preserve
// subpixel line coverage for previews and exports.
struct GrayImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels;
};

struct LineArtDocument {
    GrayImage coverageLineArt;
    BinaryImage cleanLineArt;
    DrawingPath strokes;
    std::optional<VectorDrawingPath> vectorPath;
};

struct ProcessedImage {
    std::uint32_t originalWidth{};
    std::uint32_t originalHeight{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> bgra;
    LineArtDocument lineArt;
    std::filesystem::path sourcePath;
};

} // namespace vrcdraw
