#include "PathOptimizer.hpp"

#include "ExecutionPlan.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vrcdraw {
namespace {

struct PointKey {
    std::uint32_t x{};
    std::uint32_t y{};

    friend bool operator==(const PointKey&, const PointKey&) = default;
    friend auto operator<=>(const PointKey&, const PointKey&) = default;
};

struct PointKeyHash {
    std::size_t operator()(const PointKey key) const noexcept
    {
        return static_cast<std::size_t>(key.x) * 0x9E3779B1U ^ key.y;
    }
};

struct SegmentKey {
    PointKey first;
    PointKey second;

    friend bool operator==(const SegmentKey&, const SegmentKey&) = default;
};

struct SegmentKeyHash {
    std::size_t operator()(const SegmentKey& key) const noexcept
    {
        const PointKeyHash pointHash;
        return pointHash(key.first) ^
               (pointHash(key.second) + 0x9E3779B9U +
                (pointHash(key.first) << 6U) + (pointHash(key.first) >> 2U));
    }
};

struct MouseSegmentKey {
    MousePoint first;
    MousePoint second;

    friend bool operator==(const MouseSegmentKey&, const MouseSegmentKey&) = default;
};

struct MouseSegmentKeyHash {
    std::size_t operator()(const MouseSegmentKey& key) const noexcept
    {
        const auto pointHash = [](const MousePoint point) {
            return static_cast<std::size_t>(static_cast<std::uint32_t>(point.x)) *
                       0x9E3779B1U ^
                   static_cast<std::uint32_t>(point.y);
        };
        const std::size_t left = pointHash(key.first);
        const std::size_t right = pointHash(key.second);
        return left ^ (right + 0x9E3779B9U + (left << 6U) + (left >> 2U));
    }
};

PointKey ToKey(const PointF point)
{
    const float x = point.x == 0.0F ? 0.0F : point.x;
    const float y = point.y == 0.0F ? 0.0F : point.y;
    return PointKey{std::bit_cast<std::uint32_t>(x), std::bit_cast<std::uint32_t>(y)};
}

SegmentKey ToKey(const PointF first, const PointF second)
{
    PointKey left = ToKey(first);
    PointKey right = ToKey(second);
    if (right < left) {
        std::swap(left, right);
    }
    return SegmentKey{left, right};
}

using SegmentCounts = std::unordered_map<SegmentKey, std::size_t, SegmentKeyHash>;

SegmentCounts CountSegments(const DrawingPath& path)
{
    SegmentCounts counts;
    std::size_t segmentCount = 0;
    for (const Stroke& stroke : path.strokes) {
        segmentCount += stroke.size() > 1 ? stroke.size() - 1 : 0;
    }
    counts.reserve(segmentCount);
    for (const Stroke& stroke : path.strokes) {
        for (std::size_t index = 1; index < stroke.size(); ++index) {
            ++counts[ToKey(stroke[index - 1], stroke[index])];
        }
    }
    return counts;
}

using MouseSegmentCounts =
    std::unordered_map<MouseSegmentKey, std::size_t, MouseSegmentKeyHash>;

MouseSegmentCounts CountPenDownMoves(const ExecutionPlan& plan)
{
    MouseSegmentCounts counts;
    counts.reserve(plan.metrics.penDownMoves);
    MousePoint current{};
    bool penDown = false;
    for (const MouseCommand& command : plan.commands) {
        if (command.type == MouseCommandType::LeftDown) {
            penDown = true;
        } else if (command.type == MouseCommandType::LeftUp) {
            penDown = false;
        } else if (command.type == MouseCommandType::Move) {
            const MousePoint next{current.x + command.dx, current.y + command.dy};
            if (penDown) {
                MousePoint first = current;
                MousePoint second = next;
                if (std::pair{second.x, second.y} < std::pair{first.x, first.y}) {
                    std::swap(first, second);
                }
                ++counts[MouseSegmentKey{first, second}];
            }
            current = next;
        }
    }
    return counts;
}

std::size_t SegmentCount(const DrawingPath& path)
{
    return std::accumulate(
        path.strokes.begin(),
        path.strokes.end(),
        std::size_t{},
        [](const std::size_t total, const Stroke& stroke) {
            return total + (stroke.size() > 1 ? stroke.size() - 1 : 0);
        });
}

float PenUpDistance(const DrawingPath& path)
{
    PointF current{
        static_cast<float>(path.width) * 0.5F,
        static_cast<float>(path.height) * 0.5F,
    };
    float distance = 0.0F;
    for (const Stroke& stroke : path.strokes) {
        if (stroke.size() < 2) {
            continue;
        }
        distance += std::hypot(stroke.front().x - current.x, stroke.front().y - current.y);
        current = stroke.back();
    }
    return distance;
}

struct GraphEdge {
    std::size_t stroke{};
    std::size_t left{};
    std::size_t right{};
    bool virtualEdge{};
};

struct EdgeVisit {
    std::size_t edge{};
    std::size_t from{};
    std::size_t to{};
};

struct Trail {
    Stroke points;
    std::vector<std::size_t> sourceEdgeEnds;
    StrokeRouteMetadata metadata;
};

StrokeRouteMetadata DefaultMetadata(const Stroke& stroke)
{
    StrokeRouteMetadata metadata{
        .spans = {},
        .closed = stroke.size() >= 3 && stroke.front() == stroke.back(),
    };
    if (stroke.size() < 2) {
        return metadata;
    }
    std::uint8_t beginFlags = AnchorGraphEndpoint;
    std::uint8_t endFlags = AnchorGraphEndpoint;
    if (metadata.closed) {
        beginFlags |= AnchorClosedSeam;
        endFlags |= AnchorClosedSeam;
    }
    metadata.spans.push_back(RouteSpan{
        .pointBegin = 0,
        .pointEnd = stroke.size() - 1,
        .regionType = LineRegionType::Ambiguous,
        .beginAnchorFlags = beginFlags,
        .endAnchorFlags = endFlags,
    });
    return metadata;
}

StrokeRouteMetadata MetadataForStroke(
    const DrawingPath& path,
    const std::size_t strokeIndex)
{
    if (strokeIndex < path.routeMetadata.size() &&
        !path.routeMetadata[strokeIndex].spans.empty()) {
        return path.routeMetadata[strokeIndex];
    }
    return DefaultMetadata(path.strokes[strokeIndex]);
}

void AppendOrientedMetadata(
    Trail& trail,
    const StrokeRouteMetadata& source,
    const std::size_t sourcePointCount,
    const bool forward,
    const std::size_t destinationOffset)
{
    if (sourcePointCount < 2 || source.spans.empty()) {
        return;
    }
    if (forward) {
        for (const RouteSpan& span : source.spans) {
            trail.metadata.spans.push_back(RouteSpan{
                .pointBegin = destinationOffset + span.pointBegin,
                .pointEnd = destinationOffset + span.pointEnd,
                .regionType = span.regionType,
                .beginAnchorFlags = span.beginAnchorFlags,
                .endAnchorFlags = span.endAnchorFlags,
            });
        }
    } else {
        for (auto span = source.spans.rbegin(); span != source.spans.rend(); ++span) {
            trail.metadata.spans.push_back(RouteSpan{
                .pointBegin = destinationOffset + sourcePointCount - 1 - span->pointEnd,
                .pointEnd = destinationOffset + sourcePointCount - 1 - span->pointBegin,
                .regionType = span->regionType,
                .beginAnchorFlags = span->endAnchorFlags,
                .endAnchorFlags = span->beginAnchorFlags,
            });
        }
    }
}

PointF OutgoingTangent(
    const GraphEdge& edge,
    const std::size_t node,
    const std::vector<Stroke>& strokes)
{
    if (edge.virtualEdge) {
        return {};
    }
    const Stroke& stroke = strokes[edge.stroke];
    if (edge.left == node) {
        for (std::size_t index = 1; index < stroke.size(); ++index) {
            const float x = stroke[index].x - stroke.front().x;
            const float y = stroke[index].y - stroke.front().y;
            const float length = std::hypot(x, y);
            if (length > 1.0e-5F) {
                return PointF{x / length, y / length};
            }
        }
    } else {
        for (std::size_t index = stroke.size() - 1; index-- > 0;) {
            const float x = stroke[index].x - stroke.back().x;
            const float y = stroke[index].y - stroke.back().y;
            const float length = std::hypot(x, y);
            if (length > 1.0e-5F) {
                return PointF{x / length, y / length};
            }
        }
    }
    return {};
}

std::size_t OtherNode(const GraphEdge& edge, const std::size_t node)
{
    return edge.left == node ? edge.right : edge.left;
}

std::size_t SelectNextEdge(
    const std::size_t node,
    const std::size_t incomingEdge,
    const std::vector<GraphEdge>& edges,
    const std::vector<std::vector<std::size_t>>& adjacency,
    const std::vector<std::uint8_t>& used,
    const std::vector<Stroke>& strokes)
{
    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    std::size_t best = missing;
    float bestScore = std::numeric_limits<float>::infinity();
    PointF incoming{};
    bool hasIncomingTangent = false;
    if (incomingEdge != missing && !edges[incomingEdge].virtualEdge) {
        const PointF outward = OutgoingTangent(edges[incomingEdge], node, strokes);
        incoming = PointF{-outward.x, -outward.y};
        hasIncomingTangent = std::hypot(incoming.x, incoming.y) > 0.5F;
    }

    for (const std::size_t edgeIndex : adjacency[node]) {
        if (used[edgeIndex] != 0) {
            continue;
        }
        const GraphEdge& edge = edges[edgeIndex];
        float score = edge.virtualEdge ? 100.0F : 0.0F;
        if (!edge.virtualEdge && hasIncomingTangent) {
            const PointF outgoing = OutgoingTangent(edge, node, strokes);
            score = 1.0F - (incoming.x * outgoing.x + incoming.y * outgoing.y);
        }
        if (score < bestScore || (score == bestScore && edgeIndex < best)) {
            best = edgeIndex;
            bestScore = score;
        }
    }
    return best;
}

std::vector<EdgeVisit> EulerCircuit(
    const std::size_t start,
    const std::vector<GraphEdge>& edges,
    const std::vector<std::vector<std::size_t>>& adjacency,
    std::vector<std::uint8_t>& used,
    const std::vector<Stroke>& strokes)
{
    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    struct StackItem {
        std::size_t node{};
        std::size_t incomingEdge{};
        std::size_t from{};
    };
    std::vector<StackItem> stack{
        StackItem{.node = start, .incomingEdge = missing, .from = start},
    };
    std::vector<EdgeVisit> reversed;

    while (!stack.empty()) {
        const StackItem current = stack.back();
        const std::size_t nextEdge = SelectNextEdge(
            current.node,
            current.incomingEdge,
            edges,
            adjacency,
            used,
            strokes);
        if (nextEdge != missing) {
            used[nextEdge] = 1;
            const std::size_t nextNode = OtherNode(edges[nextEdge], current.node);
            stack.push_back(StackItem{
                .node = nextNode,
                .incomingEdge = nextEdge,
                .from = current.node,
            });
            continue;
        }
        stack.pop_back();
        if (current.incomingEdge != missing) {
            reversed.push_back(EdgeVisit{
                .edge = current.incomingEdge,
                .from = current.from,
                .to = current.node,
            });
        }
    }
    std::ranges::reverse(reversed);
    return reversed;
}

bool AppendVisit(
    Trail& trail,
    const EdgeVisit visit,
    const std::vector<GraphEdge>& edges,
    const DrawingPath& baseline)
{
    const GraphEdge& edge = edges[visit.edge];
    if (edge.virtualEdge) {
        return false;
    }
    const Stroke& source = baseline.strokes[edge.stroke];
    const bool forward = visit.from == edge.left;
    const PointF first = forward ? source.front() : source.back();
    if (!trail.points.empty() && trail.points.back() != first) {
        return false;
    }
    const std::size_t destinationOffset = trail.points.empty()
        ? 0
        : trail.points.size() - 1;
    const StrokeRouteMetadata sourceMetadata = MetadataForStroke(baseline, edge.stroke);
    if (forward) {
        trail.points.insert(
            trail.points.end(),
            source.begin() + static_cast<std::ptrdiff_t>(!trail.points.empty()),
            source.end());
    } else {
        const auto firstToCopy =
            source.rbegin() + static_cast<std::ptrdiff_t>(!trail.points.empty());
        trail.points.insert(trail.points.end(), firstToCopy, source.rend());
    }
    AppendOrientedMetadata(
        trail, sourceMetadata, source.size(), forward, destinationOffset);
    trail.sourceEdgeEnds.push_back(trail.points.size() - 1);
    trail.metadata.closed = trail.points.size() >= 3 &&
                            trail.points.front() == trail.points.back();
    return true;
}

bool AppendCircuitTrails(
    const std::vector<EdgeVisit>& circuit,
    const std::vector<GraphEdge>& edges,
    const DrawingPath& baseline,
    std::vector<Trail>& output)
{
    const auto virtualPosition = std::ranges::find_if(
        circuit,
        [&](const EdgeVisit visit) { return edges[visit.edge].virtualEdge; });
    const std::size_t first = virtualPosition == circuit.end()
        ? 0
        : (static_cast<std::size_t>(virtualPosition - circuit.begin()) + 1) % circuit.size();
    Trail trail;
    for (std::size_t offset = 0; offset < circuit.size(); ++offset) {
        const EdgeVisit visit = circuit[(first + offset) % circuit.size()];
        if (edges[visit.edge].virtualEdge) {
            if (trail.points.size() >= 2) {
                output.push_back(std::move(trail));
                trail = Trail{};
            }
            continue;
        }
        if (!AppendVisit(trail, visit, edges, baseline)) {
            return false;
        }
    }
    if (trail.points.size() >= 2) {
        output.push_back(std::move(trail));
    }
    return true;
}

void ReverseTrail(Trail& trail)
{
    std::vector<std::size_t> edgeLengths;
    edgeLengths.reserve(trail.sourceEdgeEnds.size());
    std::size_t previousEnd = 0;
    for (const std::size_t end : trail.sourceEdgeEnds) {
        edgeLengths.push_back(end - previousEnd);
        previousEnd = end;
    }
    const std::size_t pointCount = trail.points.size();
    std::vector<RouteSpan> reversedSpans;
    reversedSpans.reserve(trail.metadata.spans.size());
    for (auto span = trail.metadata.spans.rbegin();
         span != trail.metadata.spans.rend();
         ++span) {
        reversedSpans.push_back(RouteSpan{
            .pointBegin = pointCount - 1 - span->pointEnd,
            .pointEnd = pointCount - 1 - span->pointBegin,
            .regionType = span->regionType,
            .beginAnchorFlags = span->endAnchorFlags,
            .endAnchorFlags = span->beginAnchorFlags,
        });
    }
    std::ranges::reverse(trail.points);
    trail.metadata.spans = std::move(reversedSpans);
    std::ranges::reverse(edgeLengths);
    trail.sourceEdgeEnds.clear();
    std::size_t cumulative = 0;
    for (const std::size_t length : edgeLengths) {
        cumulative += length;
        trail.sourceEdgeEnds.push_back(cumulative);
    }
}

void RotateClosedTrail(Trail& trail, const std::size_t firstEdge)
{
    if (firstEdge == 0 || trail.points.size() < 3 ||
        trail.points.front() != trail.points.back() ||
        firstEdge >= trail.sourceEdgeEnds.size()) {
        return;
    }
    std::vector<std::size_t> edgeLengths;
    edgeLengths.reserve(trail.sourceEdgeEnds.size());
    std::size_t previousEnd = 0;
    for (const std::size_t end : trail.sourceEdgeEnds) {
        edgeLengths.push_back(end - previousEnd);
        previousEnd = end;
    }
    const std::size_t firstPoint = trail.sourceEdgeEnds[firstEdge - 1];
    const std::size_t uniquePoints = trail.points.size() - 1;
    Stroke rotated;
    rotated.reserve(trail.points.size());
    for (std::size_t offset = 0; offset < uniquePoints; ++offset) {
        rotated.push_back(trail.points[(firstPoint + offset) % uniquePoints]);
    }
    rotated.push_back(rotated.front());
    trail.points = std::move(rotated);

    if (!trail.metadata.spans.empty()) {
        const auto firstSpan = std::ranges::find(
            trail.metadata.spans, firstPoint, &RouteSpan::pointBegin);
        if (firstSpan != trail.metadata.spans.end()) {
            std::rotate(
                trail.metadata.spans.begin(),
                firstSpan,
                trail.metadata.spans.end());
            std::size_t cumulative = 0;
            for (RouteSpan& span : trail.metadata.spans) {
                const std::size_t length = span.pointEnd - span.pointBegin;
                span.pointBegin = cumulative;
                span.pointEnd = cumulative + length;
                span.beginAnchorFlags &= static_cast<std::uint8_t>(~AnchorClosedSeam);
                span.endAnchorFlags &= static_cast<std::uint8_t>(~AnchorClosedSeam);
                cumulative += length;
            }
            trail.metadata.spans.front().beginAnchorFlags |= AnchorClosedSeam;
            trail.metadata.spans.back().endAnchorFlags |= AnchorClosedSeam;
        }
    }
    std::rotate(edgeLengths.begin(), edgeLengths.begin() +
        static_cast<std::ptrdiff_t>(firstEdge), edgeLengths.end());
    trail.sourceEdgeEnds.clear();
    std::size_t cumulative = 0;
    for (const std::size_t length : edgeLengths) {
        cumulative += length;
        trail.sourceEdgeEnds.push_back(cumulative);
    }
}

int PenUpMoveCost(
    const PointF from,
    const PointF to,
    const std::uint32_t width,
    const std::uint32_t height)
{
    const int maximumDimension = static_cast<int>(std::max(width, height));
    const float scale = maximumDimension > 0
        ? std::min(1.0F, 768.0F / static_cast<float>(maximumDimension))
        : 1.0F;
    const float centerX = static_cast<float>(width) * 0.5F;
    const float centerY = static_cast<float>(height) * 0.5F;
    const auto convert = [&](const PointF point) {
        return std::pair{
            static_cast<int>(std::lround((point.x - centerX) * scale)),
            static_cast<int>(std::lround((point.y - centerY) * scale)),
        };
    };
    const auto left = convert(from);
    const auto right = convert(to);
    const int distance = std::max(
        std::abs(right.first - left.first),
        std::abs(right.second - left.second));
    return (distance + 5) / 6;
}

bool HasMouseMovement(
    const Stroke& stroke,
    const std::uint32_t width,
    const std::uint32_t height)
{
    if (stroke.size() < 2) {
        return false;
    }
    const int maximumDimension = static_cast<int>(std::max(width, height));
    const float scale = maximumDimension > 0
        ? std::min(1.0F, 768.0F / static_cast<float>(maximumDimension))
        : 1.0F;
    const float centerX = static_cast<float>(width) * 0.5F;
    const float centerY = static_cast<float>(height) * 0.5F;
    const auto convert = [&](const PointF point) {
        return std::pair{
            static_cast<int>(std::lround((point.x - centerX) * scale)),
            static_cast<int>(std::lround((point.y - centerY) * scale)),
        };
    };
    auto previous = convert(stroke.front());
    for (std::size_t index = 1; index < stroke.size(); ++index) {
        const auto current = convert(stroke[index]);
        if (current != previous) {
            return true;
        }
        previous = current;
    }
    return false;
}

void ImproveTrailOrder(
    std::vector<Trail>& trails,
    const std::uint32_t width,
    const std::uint32_t height)
{
    constexpr std::size_t searchWindow = 48;
    const PointF center{
        static_cast<float>(width) * 0.5F,
        static_cast<float>(height) * 0.5F,
    };
    for (int pass = 0; pass < 2; ++pass) {
        bool improved = false;
        for (std::size_t first = 0; first < trails.size(); ++first) {
            const PointF previous = first == 0 ? center : trails[first - 1].points.back();
            const std::size_t limit = std::min(trails.size(), first + searchWindow + 1);
            for (std::size_t last = first + 1; last < limit; ++last) {
                const int oldCost = PenUpMoveCost(
                    previous, trails[first].points.front(), width, height) +
                    (last + 1 < trails.size()
                        ? PenUpMoveCost(
                              trails[last].points.back(),
                              trails[last + 1].points.front(),
                              width,
                              height)
                        : 0);
                const int newCost = PenUpMoveCost(
                    previous, trails[last].points.back(), width, height) +
                    (last + 1 < trails.size()
                        ? PenUpMoveCost(
                              trails[first].points.front(),
                              trails[last + 1].points.front(),
                              width,
                              height)
                        : 0);
                if (newCost >= oldCost) {
                    continue;
                }
                std::reverse(
                    trails.begin() + static_cast<std::ptrdiff_t>(first),
                    trails.begin() + static_cast<std::ptrdiff_t>(last + 1));
                for (std::size_t index = first; index <= last; ++index) {
                    ReverseTrail(trails[index]);
                }
                improved = true;
                break;
            }
        }
        if (!improved) {
            break;
        }
    }
}

std::vector<Trail> OrderTrails(
    std::vector<Trail> trails,
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<Trail> ordered;
    ordered.reserve(trails.size());
    PointF current{static_cast<float>(width) * 0.5F, static_cast<float>(height) * 0.5F};
    while (!trails.empty()) {
        std::size_t bestStroke = 0;
        std::size_t bestLoopEdge = 0;
        bool reverseBest = false;
        float bestDistance = std::numeric_limits<float>::infinity();
        for (std::size_t index = 0; index < trails.size(); ++index) {
            const Stroke& stroke = trails[index].points;
            if (stroke.size() < 2) {
                continue;
            }
            if (stroke.front() == stroke.back() &&
                !trails[index].sourceEdgeEnds.empty()) {
                for (std::size_t edge = 0;
                     edge < trails[index].sourceEdgeEnds.size();
                     ++edge) {
                    const std::size_t point = edge == 0
                        ? 0
                        : trails[index].sourceEdgeEnds[edge - 1];
                    const float distance = std::hypot(
                        stroke[point].x - current.x, stroke[point].y - current.y);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        bestStroke = index;
                        bestLoopEdge = edge;
                        reverseBest = false;
                    }
                }
                continue;
            }
            const float frontDistance = std::hypot(
                stroke.front().x - current.x, stroke.front().y - current.y);
            const float backDistance = std::hypot(
                stroke.back().x - current.x, stroke.back().y - current.y);
            if (std::min(frontDistance, backDistance) < bestDistance) {
                bestDistance = std::min(frontDistance, backDistance);
                bestStroke = index;
                bestLoopEdge = 0;
                reverseBest = backDistance < frontDistance;
            }
        }

        Trail selected = std::move(trails[bestStroke]);
        if (selected.points.front() == selected.points.back()) {
            RotateClosedTrail(selected, bestLoopEdge);
        } else if (reverseBest) {
            ReverseTrail(selected);
        }
        current = selected.points.back();
        ordered.push_back(std::move(selected));
        trails.erase(trails.begin() + static_cast<std::ptrdiff_t>(bestStroke));
    }
    ImproveTrailOrder(ordered, width, height);
    return ordered;
}

bool BuildTrails(const DrawingPath& baseline, std::vector<Trail>& trails)
{
    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    std::unordered_map<PointKey, std::size_t, PointKeyHash> nodeByPoint;
    std::vector<GraphEdge> edges;
    std::vector<std::vector<std::size_t>> adjacency;
    std::vector<std::size_t> nonDrawable;

    const auto nodeFor = [&](const PointF point) {
        const PointKey key = ToKey(point);
        const auto existing = nodeByPoint.find(key);
        if (existing != nodeByPoint.end()) {
            return existing->second;
        }
        const std::size_t node = adjacency.size();
        nodeByPoint.emplace(key, node);
        adjacency.emplace_back();
        return node;
    };

    edges.reserve(baseline.strokes.size());
    for (std::size_t strokeIndex = 0; strokeIndex < baseline.strokes.size(); ++strokeIndex) {
        const Stroke& stroke = baseline.strokes[strokeIndex];
        if (stroke.size() < 2 ||
            !HasMouseMovement(stroke, baseline.width, baseline.height)) {
            nonDrawable.push_back(strokeIndex);
            continue;
        }
        const std::size_t left = nodeFor(stroke.front());
        const std::size_t right = nodeFor(stroke.back());
        const std::size_t edgeIndex = edges.size();
        edges.push_back(GraphEdge{
            .stroke = strokeIndex,
            .left = left,
            .right = right,
            .virtualEdge = false,
        });
        adjacency[left].push_back(edgeIndex);
        adjacency[right].push_back(edgeIndex);
    }

    const std::size_t realEdgeCount = edges.size();
    std::vector<std::uint8_t> visitedNode(adjacency.size(), 0);
    std::vector<std::vector<std::size_t>> components;
    for (std::size_t start = 0; start < adjacency.size(); ++start) {
        if (visitedNode[start] != 0 || adjacency[start].empty()) {
            continue;
        }
        std::vector<std::size_t> component;
        std::queue<std::size_t> pending;
        visitedNode[start] = 1;
        pending.push(start);
        while (!pending.empty()) {
            const std::size_t node = pending.front();
            pending.pop();
            component.push_back(node);
            for (const std::size_t edgeIndex : adjacency[node]) {
                const std::size_t neighbor = OtherNode(edges[edgeIndex], node);
                if (visitedNode[neighbor] == 0) {
                    visitedNode[neighbor] = 1;
                    pending.push(neighbor);
                }
            }
        }
        components.push_back(std::move(component));
    }

    for (const auto& component : components) {
        std::vector<std::size_t> oddNodes;
        for (const std::size_t node : component) {
            if (adjacency[node].size() % 2 != 0) {
                oddNodes.push_back(node);
            }
        }
        for (std::size_t index = 0; index + 1 < oddNodes.size(); index += 2) {
            const std::size_t edgeIndex = edges.size();
            edges.push_back(GraphEdge{
                .stroke = missing,
                .left = oddNodes[index],
                .right = oddNodes[index + 1],
                .virtualEdge = true,
            });
            adjacency[oddNodes[index]].push_back(edgeIndex);
            adjacency[oddNodes[index + 1]].push_back(edgeIndex);
        }
    }

    std::vector<std::uint8_t> used(edges.size(), 0);
    for (const auto& component : components) {
        const auto circuit = EulerCircuit(
            component.front(), edges, adjacency, used, baseline.strokes);
        if (circuit.empty() ||
            !AppendCircuitTrails(circuit, edges, baseline, trails)) {
            return false;
        }
    }
    for (const std::size_t strokeIndex : nonDrawable) {
        trails.push_back(Trail{
            .points = baseline.strokes[strokeIndex],
            .sourceEdgeEnds = {},
            .metadata = MetadataForStroke(baseline, strokeIndex),
        });
    }
    return std::ranges::all_of(
        std::views::iota(std::size_t{}, realEdgeCount),
        [&](const std::size_t edge) { return used[edge] != 0; });
}

} // namespace

bool HaveIdenticalPathSegments(
    const DrawingPath& baseline,
    const DrawingPath& candidate)
{
    return baseline.width == candidate.width && baseline.height == candidate.height &&
           SegmentCount(baseline) == SegmentCount(candidate) &&
           CountSegments(baseline) == CountSegments(candidate);
}

bool HaveConsistentRouteMetadata(const DrawingPath& path)
{
    if (path.routeMetadata.size() != path.strokes.size()) {
        return path.strokes.empty() && path.routeMetadata.empty();
    }
    for (std::size_t strokeIndex = 0; strokeIndex < path.strokes.size(); ++strokeIndex) {
        const Stroke& stroke = path.strokes[strokeIndex];
        const StrokeRouteMetadata& metadata = path.routeMetadata[strokeIndex];
        if (stroke.size() < 2) {
            if (!metadata.spans.empty()) {
                return false;
            }
            continue;
        }
        if (metadata.spans.empty() || metadata.spans.front().pointBegin != 0 ||
            metadata.spans.back().pointEnd != stroke.size() - 1 ||
            metadata.closed != (stroke.size() >= 3 && stroke.front() == stroke.back())) {
            return false;
        }
        std::size_t previousEnd = 0;
        for (const RouteSpan& span : metadata.spans) {
            if (span.pointBegin != previousEnd || span.pointEnd <= span.pointBegin ||
                span.pointEnd >= stroke.size()) {
                return false;
            }
            previousEnd = span.pointEnd;
        }
    }
    return true;
}

DrawingPath OptimizeDrawingPathLossless(DrawingPath baseline)
{
    if (!HaveConsistentRouteMetadata(baseline)) {
        baseline.routeMetadata.clear();
        baseline.routeMetadata.reserve(baseline.strokes.size());
        for (const Stroke& stroke : baseline.strokes) {
            baseline.routeMetadata.push_back(DefaultMetadata(stroke));
        }
    }
    PathOptimizationStats stats{
        .strokesBefore = baseline.strokes.size(),
        .strokesAfter = baseline.strokes.size(),
        .sourceSegments = SegmentCount(baseline),
        .penUpDistanceBefore = PenUpDistance(baseline),
    };
    const auto baselinePlan = BuildExecutionPlan(baseline);
    stats.estimatedMillisecondsBefore = baselinePlan.estimatedDuration.count();

    DrawingPath candidate{
        .width = baseline.width,
        .height = baseline.height,
        .strokes = {},
        .sourceEdgeEnds = {},
        .optimization = {},
        .routeMetadata = {},
    };
    std::vector<Trail> trails;
    if (!BuildTrails(baseline, trails)) {
        baseline.optimization = stats;
        return baseline;
    }
    trails = OrderTrails(std::move(trails), candidate.width, candidate.height);
    candidate.strokes.reserve(trails.size());
    candidate.sourceEdgeEnds.reserve(trails.size());
    candidate.routeMetadata.reserve(trails.size());
    for (Trail& trail : trails) {
        candidate.strokes.push_back(std::move(trail.points));
        candidate.sourceEdgeEnds.push_back(std::move(trail.sourceEdgeEnds));
        candidate.routeMetadata.push_back(std::move(trail.metadata));
    }
    stats.strokesAfter = candidate.strokes.size();
    stats.penUpDistanceAfter = PenUpDistance(candidate);
    stats.exactSegmentMatch = HaveIdenticalPathSegments(baseline, candidate);
    const auto candidatePlan = BuildExecutionPlan(candidate);
    stats.estimatedMillisecondsAfter = candidatePlan.estimatedDuration.count();
    stats.exactExecutionMatch =
        baselinePlan.metrics.penDownMoves == candidatePlan.metrics.penDownMoves &&
        CountPenDownMoves(baselinePlan) == CountPenDownMoves(candidatePlan);
    const bool metadataConsistent = HaveConsistentRouteMetadata(candidate);
    stats.applied = stats.exactSegmentMatch && stats.exactExecutionMatch &&
                    metadataConsistent &&
                    stats.strokesAfter <= stats.strokesBefore &&
                    stats.estimatedMillisecondsAfter <= stats.estimatedMillisecondsBefore;
    if (!stats.applied) {
        stats.strokesAfter = stats.strokesBefore;
        stats.penUpDistanceAfter = stats.penUpDistanceBefore;
        stats.estimatedMillisecondsAfter = stats.estimatedMillisecondsBefore;
        baseline.optimization = stats;
        return baseline;
    }
    candidate.optimization = stats;
    return candidate;
}

} // namespace vrcdraw
