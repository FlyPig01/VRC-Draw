#include "VectorPath.hpp"

#include "PathMath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace vrcdraw {
namespace {

constexpr float kGeometryEpsilon = 1.0e-5F;

PointF Add(const PointF left, const PointF right)
{
    return PointF{left.x + right.x, left.y + right.y};
}

PointF Subtract(const PointF left, const PointF right)
{
    return PointF{left.x - right.x, left.y - right.y};
}

PointF Multiply(const PointF point, const float value)
{
    return PointF{point.x * value, point.y * value};
}

float Dot(const PointF left, const PointF right)
{
    return left.x * right.x + left.y * right.y;
}

float Length(const PointF point)
{
    return std::hypot(point.x, point.y);
}

PointF Normalize(const PointF point)
{
    const float length = Length(point);
    return length > kGeometryEpsilon ? Multiply(point, 1.0F / length) : PointF{};
}

bool LexicographicallyLess(const PointF left, const PointF right)
{
    return std::pair{left.x, left.y} < std::pair{right.x, right.y};
}

PointF Evaluate(const CubicBezier& curve, const float t)
{
    const float u = 1.0F - t;
    const float b0 = u * u * u;
    const float b1 = 3.0F * u * u * t;
    const float b2 = 3.0F * u * t * t;
    const float b3 = t * t * t;
    return PointF{
        curve.p0.x * b0 + curve.p1.x * b1 + curve.p2.x * b2 + curve.p3.x * b3,
        curve.p0.y * b0 + curve.p1.y * b1 + curve.p2.y * b2 + curve.p3.y * b3,
    };
}

std::pair<CubicBezier, CubicBezier> Split(const CubicBezier& curve)
{
    const PointF p01 = Multiply(Add(curve.p0, curve.p1), 0.5F);
    const PointF p12 = Multiply(Add(curve.p1, curve.p2), 0.5F);
    const PointF p23 = Multiply(Add(curve.p2, curve.p3), 0.5F);
    const PointF p012 = Multiply(Add(p01, p12), 0.5F);
    const PointF p123 = Multiply(Add(p12, p23), 0.5F);
    const PointF center = Multiply(Add(p012, p123), 0.5F);
    return {
        CubicBezier{curve.p0, p01, p012, center},
        CubicBezier{center, p123, p23, curve.p3},
    };
}

void FlattenCubic(
    const CubicBezier& curve,
    const float tolerance,
    const int depth,
    Stroke& output)
{
    const float flatness = std::max(
        PointSegmentDistance(curve.p1, curve.p0, curve.p3),
        PointSegmentDistance(curve.p2, curve.p0, curve.p3));
    if (flatness <= tolerance || depth >= 16) {
        if (output.empty() || output.back() != curve.p3) {
            output.push_back(curve.p3);
        }
        return;
    }
    const auto [left, right] = Split(curve);
    FlattenCubic(left, tolerance, depth + 1, output);
    FlattenCubic(right, tolerance, depth + 1, output);
}

float PointPolylineDistance(const PointF point, const Stroke& polyline)
{
    if (polyline.empty()) {
        return std::numeric_limits<float>::infinity();
    }
    if (polyline.size() == 1) {
        return Length(Subtract(point, polyline.front()));
    }
    float distance = std::numeric_limits<float>::infinity();
    for (std::size_t index = 1; index < polyline.size(); ++index) {
        distance = std::min(
            distance,
            PointSegmentDistance(point, polyline[index - 1], polyline[index]));
    }
    return distance;
}

float BidirectionalDistance(const Stroke& left, const Stroke& right)
{
    float distance = 0.0F;
    for (const PointF point : left) {
        distance = std::max(distance, PointPolylineDistance(point, right));
    }
    for (const PointF point : right) {
        distance = std::max(distance, PointPolylineDistance(point, left));
    }
    return distance;
}

int Orientation(const PointF a, const PointF b, const PointF c)
{
    const float value = (b.x - a.x) * (c.y - a.y) -
                        (b.y - a.y) * (c.x - a.x);
    if (std::abs(value) <= 1.0e-4F) {
        return 0;
    }
    return value > 0.0F ? 1 : -1;
}

bool ProperlyIntersects(
    const PointF a,
    const PointF b,
    const PointF c,
    const PointF d)
{
    return Orientation(a, b, c) * Orientation(a, b, d) < 0 &&
           Orientation(c, d, a) * Orientation(c, d, b) < 0;
}

std::size_t SelfIntersectionCount(const Stroke& points)
{
    if (points.size() < 4) {
        return 0;
    }
    const bool closed = points.front() == points.back();
    std::size_t count = 0;
    for (std::size_t left = 1; left < points.size(); ++left) {
        for (std::size_t right = left + 2; right < points.size(); ++right) {
            if (closed && left == 1 && right + 1 == points.size()) {
                continue;
            }
            count += ProperlyIntersects(
                points[left - 1], points[left], points[right - 1], points[right]);
        }
    }
    return count;
}

std::size_t CurvatureSpikeCount(const Stroke& points)
{
    std::size_t count = 0;
    for (std::size_t index = 2; index < points.size(); ++index) {
        const PointF incoming = Subtract(points[index - 1], points[index - 2]);
        const PointF outgoing = Subtract(points[index], points[index - 1]);
        const float denominator = Length(incoming) * Length(outgoing);
        if (denominator > kGeometryEpsilon &&
            Dot(incoming, outgoing) / denominator < 0.35F) {
            ++count;
        }
    }
    return count;
}

bool PointSupportedByInk(const PointF point, const BinaryImage& ink)
{
    if (ink.width == 0 || ink.height == 0 ||
        ink.pixels.size() != static_cast<std::size_t>(ink.width) * ink.height) {
        return false;
    }
    const int centerX = static_cast<int>(std::lround(point.x));
    const int centerY = static_cast<int>(std::lround(point.y));
    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            const int x = centerX + offsetX;
            const int y = centerY + offsetY;
            if (x >= 0 && y >= 0 && x < static_cast<int>(ink.width) &&
                y < static_cast<int>(ink.height) &&
                ink.pixels[static_cast<std::size_t>(y) * ink.width +
                           static_cast<std::size_t>(x)] != 0) {
                return true;
            }
        }
    }
    return false;
}

bool PolylineSupportedByInk(const Stroke& points, const BinaryImage& ink)
{
    if (points.size() < 2) {
        return false;
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        const PointF start = points[index - 1];
        const PointF end = points[index];
        const int steps = std::max(
            1,
            static_cast<int>(std::ceil(Length(Subtract(end, start)) * 2.0F)));
        for (int step = 0; step <= steps; ++step) {
            const float ratio = static_cast<float>(step) / static_cast<float>(steps);
            if (!PointSupportedByInk(
                    Add(start, Multiply(Subtract(end, start), ratio)), ink)) {
                return false;
            }
        }
    }
    return true;
}

std::optional<CubicBezier> FitSingleCubic(
    const Stroke& points,
    const float tolerance,
    std::size_t& worstPoint)
{
    if (points.size() < 4) {
        return std::nullopt;
    }
    const PointF p0 = points.front();
    const PointF p3 = points.back();
    const float chord = Length(Subtract(p3, p0));
    if (chord <= kGeometryEpsilon) {
        return std::nullopt;
    }
    const PointF startTangent = Normalize(Subtract(points[1], p0));
    const PointF endTangent = Normalize(Subtract(points[points.size() - 2], p3));
    if (Length(startTangent) < 0.5F || Length(endTangent) < 0.5F) {
        return std::nullopt;
    }

    std::vector<float> parameters(points.size(), 0.0F);
    float totalLength = 0.0F;
    for (std::size_t index = 1; index < points.size(); ++index) {
        totalLength += Length(Subtract(points[index], points[index - 1]));
        parameters[index] = totalLength;
    }
    if (totalLength <= kGeometryEpsilon) {
        return std::nullopt;
    }
    for (float& parameter : parameters) {
        parameter /= totalLength;
    }

    float c00 = 0.0F;
    float c01 = 0.0F;
    float c11 = 0.0F;
    float x0 = 0.0F;
    float x1 = 0.0F;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const float t = parameters[index];
        const float u = 1.0F - t;
        const float b0 = u * u * u;
        const float b1 = 3.0F * u * u * t;
        const float b2 = 3.0F * u * t * t;
        const float b3 = t * t * t;
        const PointF a0 = Multiply(startTangent, b1);
        const PointF a1 = Multiply(endTangent, b2);
        const PointF base = Add(Multiply(p0, b0 + b1), Multiply(p3, b2 + b3));
        const PointF residual = Subtract(points[index], base);
        c00 += Dot(a0, a0);
        c01 += Dot(a0, a1);
        c11 += Dot(a1, a1);
        x0 += Dot(a0, residual);
        x1 += Dot(a1, residual);
    }
    const float determinant = c00 * c11 - c01 * c01;
    float alpha0 = chord / 3.0F;
    float alpha1 = chord / 3.0F;
    if (std::abs(determinant) > 1.0e-6F) {
        alpha0 = (x0 * c11 - x1 * c01) / determinant;
        alpha1 = (c00 * x1 - c01 * x0) / determinant;
    }
    const float minimumHandle = chord * 0.02F;
    const float maximumHandle = chord * 1.25F;
    if (alpha0 < minimumHandle || alpha1 < minimumHandle ||
        alpha0 > maximumHandle || alpha1 > maximumHandle) {
        alpha0 = chord / 3.0F;
        alpha1 = chord / 3.0F;
    }
    CubicBezier curve{
        .p0 = p0,
        .p1 = Add(p0, Multiply(startTangent, alpha0)),
        .p2 = Add(p3, Multiply(endTangent, alpha1)),
        .p3 = p3,
    };
    if (Dot(Subtract(curve.p1, p0), Subtract(p3, p0)) < 0.0F ||
        Dot(Subtract(curve.p2, p3), Subtract(p0, p3)) < 0.0F) {
        return std::nullopt;
    }

    float maximumError = 0.0F;
    worstPoint = points.size() / 2;
    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const float error = Length(Subtract(Evaluate(curve, parameters[index]), points[index]));
        if (error > maximumError) {
            maximumError = error;
            worstPoint = index;
        }
    }
    return maximumError <= tolerance ? std::optional<CubicBezier>{curve} : std::nullopt;
}

void FitRecursive(
    const Stroke& points,
    const VectorFittingOptions& options,
    std::vector<VectorSegment>& output)
{
    if (points.size() < 2) {
        return;
    }
    float maximumLineDistance = 0.0F;
    std::size_t lineSplit = points.size() / 2;
    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const float distance = PointSegmentDistance(
            points[index], points.front(), points.back());
        if (distance > maximumLineDistance) {
            maximumLineDistance = distance;
            lineSplit = index;
        }
    }
    if (maximumLineDistance <= options.lineTolerance) {
        output.push_back(VectorSegment{
            .type = VectorSegmentType::Line,
            .lineEnd = points.back(),
        });
        return;
    }

    std::size_t worstPoint = lineSplit;
    if (const auto curve = FitSingleCubic(points, options.curveTolerance, worstPoint)) {
        output.push_back(VectorSegment{
            .type = VectorSegmentType::CubicBezier,
            .lineEnd = curve->p3,
            .cubic = *curve,
        });
        return;
    }
    if (points.size() <= 3) {
        for (std::size_t index = 1; index < points.size(); ++index) {
            output.push_back(VectorSegment{
                .type = VectorSegmentType::Line,
                .lineEnd = points[index],
            });
        }
        return;
    }
    worstPoint = std::clamp(worstPoint, std::size_t{1}, points.size() - 2);
    const Stroke left(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(worstPoint + 1));
    const Stroke right(points.begin() + static_cast<std::ptrdiff_t>(worstPoint), points.end());
    FitRecursive(left, options, output);
    FitRecursive(right, options, output);
}

Stroke FlattenSegments(
    const PointF start,
    const std::vector<VectorSegment>& segments,
    const float tolerance)
{
    Stroke result{start};
    PointF current = start;
    for (const VectorSegment& segment : segments) {
        if (segment.type == VectorSegmentType::CubicBezier) {
            CubicBezier curve = segment.cubic;
            curve.p0 = current;
            FlattenCubic(curve, std::max(0.01F, tolerance), 0, result);
            current = curve.p3;
        } else {
            if (result.back() != segment.lineEnd) {
                result.push_back(segment.lineEnd);
            }
            current = segment.lineEnd;
        }
    }
    return result;
}

bool CandidateIsSafe(
    const Stroke& fallback,
    const std::vector<VectorSegment>& segments,
    const BinaryImage& ink,
    const VectorFittingOptions& options)
{
    if (fallback.size() < 2 || segments.empty()) {
        return false;
    }
    const Stroke candidate = FlattenSegments(
        fallback.front(), segments, options.flatteningTolerance);
    if (candidate.size() < 2 || candidate.front() != fallback.front() ||
        candidate.back() != fallback.back() ||
        BidirectionalDistance(candidate, fallback) > options.curveTolerance ||
        !PolylineSupportedByInk(candidate, ink) ||
        SelfIntersectionCount(candidate) > SelfIntersectionCount(fallback) ||
        CurvatureSpikeCount(candidate) > CurvatureSpikeCount(fallback)) {
        return false;
    }
    float minimumX = fallback.front().x;
    float maximumX = fallback.front().x;
    float minimumY = fallback.front().y;
    float maximumY = fallback.front().y;
    for (const PointF point : fallback) {
        minimumX = std::min(minimumX, point.x);
        maximumX = std::max(maximumX, point.x);
        minimumY = std::min(minimumY, point.y);
        maximumY = std::max(maximumY, point.y);
    }
    return std::ranges::all_of(candidate, [&](const PointF point) {
        return point.x >= minimumX - options.curveTolerance &&
               point.x <= maximumX + options.curveTolerance &&
               point.y >= minimumY - options.curveTolerance &&
               point.y <= maximumY + options.curveTolerance;
    });
}

std::vector<std::size_t> ProtectedCorners(
    const Stroke& points,
    const VectorFittingOptions& options)
{
    std::vector<std::pair<std::size_t, float>> candidates;
    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        std::size_t left = index;
        float leftLength = 0.0F;
        while (left > 0 && leftLength < options.cornerArmLength) {
            leftLength += Length(Subtract(points[left], points[left - 1]));
            --left;
        }
        std::size_t right = index;
        float rightLength = 0.0F;
        while (right + 1 < points.size() && rightLength < options.cornerArmLength) {
            rightLength += Length(Subtract(points[right + 1], points[right]));
            ++right;
        }
        if (leftLength < 1.5F || rightLength < 1.5F) {
            continue;
        }
        const PointF leftArm = Normalize(Subtract(points[left], points[index]));
        const PointF rightArm = Normalize(Subtract(points[right], points[index]));
        const float cosine = Dot(leftArm, rightArm);
        if (cosine > options.cornerCosineThreshold) {
            candidates.emplace_back(index, cosine);
        }
    }
    std::ranges::sort(candidates, {}, &std::pair<std::size_t, float>::first);
    std::vector<std::size_t> result;
    for (const auto [index, cosine] : candidates) {
        if (!result.empty() && index - result.back() <= 2) {
            const auto previous = std::ranges::find(candidates, result.back(),
                &std::pair<std::size_t, float>::first);
            if (previous != candidates.end() && cosine > previous->second) {
                result.back() = index;
            }
            continue;
        }
        result.push_back(index);
    }
    return result;
}

FittedRouteSpan ReverseFittedSpan(FittedRouteSpan canonical, Stroke fallback)
{
    std::vector<std::pair<PointF, VectorSegment>> source;
    source.reserve(canonical.segments.size());
    PointF current = canonical.start;
    for (const VectorSegment& segment : canonical.segments) {
        source.emplace_back(current, segment);
        current = segment.type == VectorSegmentType::CubicBezier
            ? segment.cubic.p3
            : segment.lineEnd;
    }
    std::vector<VectorSegment> reversed;
    reversed.reserve(source.size());
    for (auto item = source.rbegin(); item != source.rend(); ++item) {
        const PointF oldStart = item->first;
        const VectorSegment& old = item->second;
        if (old.type == VectorSegmentType::CubicBezier) {
            reversed.push_back(VectorSegment{
                .type = VectorSegmentType::CubicBezier,
                .lineEnd = oldStart,
                .cubic = CubicBezier{
                    old.cubic.p3, old.cubic.p2, old.cubic.p1, old.cubic.p0,
                },
            });
        } else {
            reversed.push_back(VectorSegment{
                .type = VectorSegmentType::Line,
                .lineEnd = oldStart,
            });
        }
    }
    canonical.start = fallback.front();
    canonical.segments = std::move(reversed);
    canonical.fallbackPolyline = std::move(fallback);
    std::swap(canonical.beginAnchorFlags, canonical.endAnchorFlags);
    return canonical;
}

FittedRouteSpan FitSpan(
    Stroke fallback,
    const LineRegionType regionType,
    const std::uint8_t beginFlags,
    const std::uint8_t endFlags,
    const BinaryImage& ink,
    const VectorFittingOptions& options)
{
    FittedRouteSpan result{
        .start = fallback.empty() ? PointF{} : fallback.front(),
        .segments = {},
        .fallbackPolyline = fallback,
        .regionType = regionType,
        .beginAnchorFlags = beginFlags,
        .endAnchorFlags = endFlags,
        .fitted = false,
    };
    if (fallback.size() < 3 || regionType == LineRegionType::Junction ||
        regionType == LineRegionType::Ambiguous) {
        return result;
    }
    const bool reverse = LexicographicallyLess(fallback.back(), fallback.front());
    Stroke canonical = fallback;
    std::uint8_t canonicalBegin = beginFlags;
    std::uint8_t canonicalEnd = endFlags;
    if (reverse) {
        std::ranges::reverse(canonical);
        std::swap(canonicalBegin, canonicalEnd);
    }

    std::vector<VectorSegment> segments;
    FitRecursive(canonical, options, segments);
    if (segments.size() >= canonical.size() - 1 ||
        !CandidateIsSafe(canonical, segments, ink, options)) {
        return result;
    }
    FittedRouteSpan canonicalResult{
        .start = canonical.front(),
        .segments = std::move(segments),
        .fallbackPolyline = canonical,
        .regionType = regionType,
        .beginAnchorFlags = canonicalBegin,
        .endAnchorFlags = canonicalEnd,
        .fitted = true,
    };
    return reverse
        ? ReverseFittedSpan(std::move(canonicalResult), std::move(fallback))
        : canonicalResult;
}

StrokeRouteMetadata FallbackMetadata(const Stroke& stroke)
{
    StrokeRouteMetadata metadata{
        .spans = {},
        .closed = stroke.size() >= 3 && stroke.front() == stroke.back(),
    };
    if (stroke.size() >= 2) {
        std::uint8_t begin = AnchorGraphEndpoint;
        std::uint8_t end = AnchorGraphEndpoint;
        if (metadata.closed) {
            begin |= AnchorClosedSeam;
            end |= AnchorClosedSeam;
        }
        metadata.spans.push_back(RouteSpan{
            .pointBegin = 0,
            .pointEnd = stroke.size() - 1,
            .regionType = LineRegionType::Ambiguous,
            .beginAnchorFlags = begin,
            .endAnchorFlags = end,
        });
    }
    return metadata;
}

} // namespace

Stroke FlattenFittedRouteSpan(const FittedRouteSpan& span, const float tolerance)
{
    if (!span.fitted || span.segments.empty()) {
        return span.fallbackPolyline;
    }
    return FlattenSegments(span.start, span.segments, tolerance);
}

VectorDrawingPath FitVectorDrawingPath(
    const DrawingPath& path,
    const BinaryImage& allowedInk,
    const VectorFittingOptions& options)
{
    VectorDrawingPath result{
        .width = path.width,
        .height = path.height,
        .strokes = {},
        .optimization = path.optimization,
        .fitting = {},
    };
    result.strokes.reserve(path.strokes.size());
    for (std::size_t strokeIndex = 0; strokeIndex < path.strokes.size(); ++strokeIndex) {
        const Stroke& stroke = path.strokes[strokeIndex];
        const StrokeRouteMetadata metadata = strokeIndex < path.routeMetadata.size()
            ? path.routeMetadata[strokeIndex]
            : FallbackMetadata(stroke);
        VectorStroke vectorStroke{
            .spans = {},
            .closed = metadata.closed,
        };
        result.fitting.sourceSpans += metadata.spans.size();
        result.fitting.sourcePolylineSegments += stroke.size() > 1 ? stroke.size() - 1 : 0;
        for (const RouteSpan& span : metadata.spans) {
            if (span.pointEnd >= stroke.size() || span.pointEnd <= span.pointBegin) {
                continue;
            }
            Stroke fallback(
                stroke.begin() + static_cast<std::ptrdiff_t>(span.pointBegin),
                stroke.begin() + static_cast<std::ptrdiff_t>(span.pointEnd + 1));
            std::vector<std::size_t> boundaries{0};
            const auto corners = ProtectedCorners(fallback, options);
            boundaries.insert(boundaries.end(), corners.begin(), corners.end());
            boundaries.push_back(fallback.size() - 1);
            std::ranges::sort(boundaries);
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
            for (std::size_t part = 1; part < boundaries.size(); ++part) {
                const std::size_t begin = boundaries[part - 1];
                const std::size_t end = boundaries[part];
                if (end <= begin) {
                    continue;
                }
                std::uint8_t beginFlags = part == 1
                    ? span.beginAnchorFlags
                    : static_cast<std::uint8_t>(AnchorProtectedCorner);
                std::uint8_t endFlags = part + 1 == boundaries.size()
                    ? span.endAnchorFlags
                    : static_cast<std::uint8_t>(AnchorProtectedCorner);
                Stroke partFallback(
                    fallback.begin() + static_cast<std::ptrdiff_t>(begin),
                    fallback.begin() + static_cast<std::ptrdiff_t>(end + 1));
                FittedRouteSpan fitted = FitSpan(
                    std::move(partFallback),
                    span.regionType,
                    beginFlags,
                    endFlags,
                    allowedInk,
                    options);
                result.fitting.vectorSegments += fitted.segments.size();
                if (fitted.fitted) {
                    ++result.fitting.fittedSpans;
                } else {
                    ++result.fitting.fallbackSpans;
                }
                vectorStroke.spans.push_back(std::move(fitted));
            }
        }
        result.fitting.cornerSplitSpans += vectorStroke.spans.size();
        result.strokes.push_back(std::move(vectorStroke));
    }
    return result;
}

DrawingPath FlattenVectorDrawingPath(const VectorDrawingPath& path, const float tolerance)
{
    DrawingPath result{
        .width = path.width,
        .height = path.height,
        .strokes = {},
        .sourceEdgeEnds = {},
        .optimization = path.optimization,
        .routeMetadata = {},
    };
    result.strokes.reserve(path.strokes.size());
    result.sourceEdgeEnds.reserve(path.strokes.size());
    result.routeMetadata.reserve(path.strokes.size());
    for (const VectorStroke& vectorStroke : path.strokes) {
        Stroke stroke;
        std::vector<std::size_t> spanEnds;
        StrokeRouteMetadata metadata{.spans = {}, .closed = vectorStroke.closed};
        for (const FittedRouteSpan& span : vectorStroke.spans) {
            const Stroke flattened = FlattenFittedRouteSpan(span, tolerance);
            if (flattened.size() < 2) {
                continue;
            }
            const std::size_t offset = stroke.empty() ? 0 : stroke.size() - 1;
            if (stroke.empty()) {
                stroke = flattened;
            } else if (stroke.back() == flattened.front()) {
                stroke.insert(stroke.end(), flattened.begin() + 1, flattened.end());
            } else {
                // A validated vector stroke must remain continuous. Falling back to
                // the current span cannot repair a broken structural boundary.
                stroke.clear();
                metadata.spans.clear();
                spanEnds.clear();
                break;
            }
            metadata.spans.push_back(RouteSpan{
                .pointBegin = offset,
                .pointEnd = stroke.size() - 1,
                .regionType = span.regionType,
                .beginAnchorFlags = span.beginAnchorFlags,
                .endAnchorFlags = span.endAnchorFlags,
            });
            spanEnds.push_back(stroke.size() - 1);
        }
        if (stroke.size() >= 2) {
            result.strokes.push_back(std::move(stroke));
            result.sourceEdgeEnds.push_back(std::move(spanEnds));
            result.routeMetadata.push_back(std::move(metadata));
        }
    }
    return result;
}

} // namespace vrcdraw
