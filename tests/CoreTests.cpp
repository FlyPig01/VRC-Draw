#include "ExecutionPlan.hpp"
#include "ImageProcessor.hpp"
#include "LineArtExporter.hpp"
#include "LineArtRenderer.hpp"
#include "MouseInput.hpp"
#include "PathMath.hpp"
#include "PathOptimizer.hpp"
#include "Settings.hpp"
#include "VectorPath.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <queue>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_set>

namespace {

#pragma pack(push, 1)
struct BitmapFileHeader {
    std::uint16_t type{0x4D42};
    std::uint32_t size{};
    std::uint16_t reserved1{};
    std::uint16_t reserved2{};
    std::uint32_t pixelOffset{54};
};

struct BitmapInfoHeader {
    std::uint32_t size{40};
    std::int32_t width{};
    std::int32_t height{};
    std::uint16_t planes{1};
    std::uint16_t bitCount{24};
    std::uint32_t compression{};
    std::uint32_t imageSize{};
    std::int32_t xPixelsPerMeter{2835};
    std::int32_t yPixelsPerMeter{2835};
    std::uint32_t colorsUsed{};
    std::uint32_t colorsImportant{};
};
#pragma pack(pop)

std::filesystem::path WriteTestBitmap()
{
    constexpr int width = 64;
    constexpr int height = 64;
    constexpr int stride = ((width * 3 + 3) / 4) * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height, 255);

    const auto setBlack = [&](const int x, const int y) {
        const std::size_t offset = static_cast<std::size_t>(height - 1 - y) * stride +
                                   static_cast<std::size_t>(x) * 3;
        pixels[offset] = 0;
        pixels[offset + 1] = 0;
        pixels[offset + 2] = 0;
    };
    for (int x = 10; x <= 53; ++x) {
        for (int thickness = -1; thickness <= 1; ++thickness) {
            setBlack(x, 10 + thickness);
            setBlack(x, 53 + thickness);
        }
    }
    for (int y = 10; y <= 53; ++y) {
        for (int thickness = -1; thickness <= 1; ++thickness) {
            setBlack(10 + thickness, y);
            setBlack(53 + thickness, y);
        }
    }

    BitmapFileHeader fileHeader{};
    BitmapInfoHeader infoHeader{};
    infoHeader.width = width;
    infoHeader.height = height;
    infoHeader.imageSize = static_cast<std::uint32_t>(pixels.size());
    fileHeader.size = fileHeader.pixelOffset + infoHeader.imageSize;

    const auto path = std::filesystem::current_path() / L"vrcdraw-core-test.bmp";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return path;
}

std::filesystem::path WriteSegmentBitmap(
    const std::wstring_view filename,
    const std::vector<std::pair<vrcdraw::PointF, vrcdraw::PointF>>& segments)
{
    constexpr int width = 64;
    constexpr int height = 64;
    constexpr int stride = ((width * 3 + 3) / 4) * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const vrcdraw::PointF sample{
                static_cast<float>(x),
                static_cast<float>(y),
            };
            const bool ink = std::ranges::any_of(segments, [&](const auto& segment) {
                return vrcdraw::PointSegmentDistance(sample, segment.first, segment.second) <=
                       3.0F;
            });
            if (!ink) {
                continue;
            }
            const std::size_t offset =
                static_cast<std::size_t>(height - 1 - y) * stride +
                static_cast<std::size_t>(x) * 3;
            pixels[offset] = 0;
            pixels[offset + 1] = 0;
            pixels[offset + 2] = 0;
        }
    }

    BitmapFileHeader fileHeader{};
    BitmapInfoHeader infoHeader{};
    infoHeader.width = width;
    infoHeader.height = height;
    infoHeader.imageSize = static_cast<std::uint32_t>(pixels.size());
    fileHeader.size = fileHeader.pixelOffset + infoHeader.imageSize;

    const auto path = std::filesystem::current_path() / filename;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    output.write(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
    assert(output.good());
    return path;
}

std::filesystem::path WriteMaskBitmap(
    const std::wstring_view filename,
    const int width,
    const int height,
    const std::function<bool(int, int)>& isInk)
{
    const int stride = ((width * 3 + 3) / 4) * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!isInk(x, y)) {
                continue;
            }
            const std::size_t offset =
                static_cast<std::size_t>(height - 1 - y) * stride +
                static_cast<std::size_t>(x) * 3;
            pixels[offset] = 0;
            pixels[offset + 1] = 0;
            pixels[offset + 2] = 0;
        }
    }

    BitmapFileHeader fileHeader{};
    BitmapInfoHeader infoHeader{};
    infoHeader.width = width;
    infoHeader.height = height;
    infoHeader.imageSize = static_cast<std::uint32_t>(pixels.size());
    fileHeader.size = fileHeader.pixelOffset + infoHeader.imageSize;

    const auto path = std::filesystem::current_path() / filename;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
    output.write(reinterpret_cast<char*>(&infoHeader), sizeof(infoHeader));
    output.write(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
    assert(output.good());
    return path;
}

void WriteQualityBitmap(
    const std::filesystem::path& path,
    const vrcdraw::RasterImage& image)
{
    BitmapFileHeader fileHeader{};
    BitmapInfoHeader infoHeader{};
    infoHeader.width = static_cast<std::int32_t>(image.width);
    infoHeader.height = -static_cast<std::int32_t>(image.height);
    infoHeader.bitCount = 32;
    infoHeader.imageSize = static_cast<std::uint32_t>(image.bgra.size());
    fileHeader.size = fileHeader.pixelOffset + infoHeader.imageSize;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    output.write(
        reinterpret_cast<const char*>(image.bgra.data()),
        static_cast<std::streamsize>(image.bgra.size()));
    assert(output.good());
}

vrcdraw::RasterImage RouteOverLineArt(
    const vrcdraw::GrayImage& lineArt,
    const vrcdraw::DrawingPath& route)
{
    auto combined = vrcdraw::RenderGrayscaleLineArt(lineArt);
    const auto routeRaster = vrcdraw::RenderLineArt(route, lineArt.width, lineArt.height);
    for (std::size_t pixel = 0; pixel < lineArt.pixels.size(); ++pixel) {
        if (routeRaster.bgra[pixel * 4U] >= 128) {
            continue;
        }
        combined.bgra[pixel * 4U] = 32;
        combined.bgra[pixel * 4U + 1U] = 32;
        combined.bgra[pixel * 4U + 2U] = 255;
        combined.bgra[pixel * 4U + 3U] = 255;
    }
    return combined;
}

double RouteOutsideInkRatio(
    const vrcdraw::DrawingPath& path,
    const vrcdraw::BinaryImage& ink)
{
    const auto supported = [&](const vrcdraw::PointF point) {
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
    };

    std::size_t samples = 0;
    std::size_t outside = 0;
    for (const vrcdraw::Stroke& stroke : path.strokes) {
        for (std::size_t index = 1; index < stroke.size(); ++index) {
            const vrcdraw::PointF start = stroke[index - 1];
            const vrcdraw::PointF end = stroke[index];
            const int steps = std::max(
                1,
                static_cast<int>(std::ceil(
                    std::hypot(end.x - start.x, end.y - start.y) * 2.0F)));
            for (int step = 0; step <= steps; ++step) {
                const float ratio = static_cast<float>(step) / static_cast<float>(steps);
                outside += !supported(vrcdraw::PointF{
                    start.x + (end.x - start.x) * ratio,
                    start.y + (end.y - start.y) * ratio,
                });
                ++samples;
            }
        }
    }
    return static_cast<double>(outside) /
           static_cast<double>(std::max<std::size_t>(1, samples));
}

bool PathHasPointNear(
    const vrcdraw::DrawingPath& path,
    const vrcdraw::PointF expected,
    const float radius)
{
    return std::ranges::any_of(path.strokes, [&](const vrcdraw::Stroke& stroke) {
        return std::ranges::any_of(stroke, [&](const vrcdraw::PointF point) {
            return std::hypot(point.x - expected.x, point.y - expected.y) <= radius;
        });
    });
}

using MouseCoverage = std::unordered_set<std::uint64_t>;

std::uint64_t MousePointKey(const vrcdraw::MousePoint point)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(point.x)) << 32U) |
           static_cast<std::uint32_t>(point.y);
}

MouseCoverage PenDownCoverage(const vrcdraw::ExecutionPlan& plan)
{
    MouseCoverage coverage;
    vrcdraw::MousePoint current{};
    bool penDown = false;
    for (const auto& command : plan.commands) {
        if (command.type == vrcdraw::MouseCommandType::LeftDown) {
            penDown = true;
            continue;
        }
        if (command.type == vrcdraw::MouseCommandType::LeftUp) {
            penDown = false;
            continue;
        }
        if (command.type != vrcdraw::MouseCommandType::Move) {
            continue;
        }
        const vrcdraw::MousePoint target{
            current.x + command.dx,
            current.y + command.dy,
        };
        if (penDown) {
            const bool reverse =
                std::pair{target.x, target.y} < std::pair{current.x, current.y};
            const vrcdraw::MousePoint rasterStart = reverse ? target : current;
            const vrcdraw::MousePoint rasterTarget = reverse ? current : target;
            int x = rasterStart.x;
            int y = rasterStart.y;
            const int differenceX = std::abs(rasterTarget.x - rasterStart.x);
            const int differenceY = -std::abs(rasterTarget.y - rasterStart.y);
            const int stepX = rasterStart.x < rasterTarget.x ? 1 : -1;
            const int stepY = rasterStart.y < rasterTarget.y ? 1 : -1;
            int error = differenceX + differenceY;
            for (;;) {
                coverage.insert(MousePointKey({x, y}));
                if (x == rasterTarget.x && y == rasterTarget.y) {
                    break;
                }
                const int doubled = error * 2;
                if (doubled >= differenceY) {
                    error += differenceY;
                    x += stepX;
                }
                if (doubled <= differenceX) {
                    error += differenceX;
                    y += stepY;
                }
            }
        }
        current = target;
    }
    return coverage;
}

std::vector<std::array<int, 4>> PenDownMoveSegments(const vrcdraw::ExecutionPlan& plan)
{
    std::vector<std::array<int, 4>> segments;
    vrcdraw::MousePoint current{};
    bool penDown = false;
    for (const auto& command : plan.commands) {
        if (command.type == vrcdraw::MouseCommandType::LeftDown) {
            penDown = true;
        } else if (command.type == vrcdraw::MouseCommandType::LeftUp) {
            penDown = false;
        } else if (command.type == vrcdraw::MouseCommandType::Move) {
            const vrcdraw::MousePoint next{current.x + command.dx, current.y + command.dy};
            if (penDown) {
                std::array segment{current.x, current.y, next.x, next.y};
                if (std::pair{segment[2], segment[3]} <
                    std::pair{segment[0], segment[1]}) {
                    std::swap(segment[0], segment[2]);
                    std::swap(segment[1], segment[3]);
                }
                segments.push_back(segment);
            }
            current = next;
        }
    }
    std::ranges::sort(segments);
    return segments;
}

vrcdraw::DrawingPath PathFromExecutionPlan(const vrcdraw::ExecutionPlan& plan)
{
    vrcdraw::DrawingPath path{
        .width = static_cast<std::uint32_t>(std::max(1, plan.canvasWidth)),
        .height = static_cast<std::uint32_t>(std::max(1, plan.canvasHeight)),
        .strokes = {},
    };
    const auto toLogical = [&](const vrcdraw::MousePoint point) {
        return vrcdraw::PointF{
            static_cast<float>(point.x) / plan.mouseScale +
                static_cast<float>(plan.canvasWidth) * 0.5F,
            static_cast<float>(point.y) / plan.mouseScale +
                static_cast<float>(plan.canvasHeight) * 0.5F,
        };
    };
    vrcdraw::MousePoint current{};
    vrcdraw::Stroke stroke;
    bool penDown = false;
    for (const auto& command : plan.commands) {
        if (command.type == vrcdraw::MouseCommandType::LeftDown) {
            penDown = true;
            stroke = {toLogical(current)};
        } else if (command.type == vrcdraw::MouseCommandType::LeftUp) {
            if (stroke.size() >= 2) {
                path.strokes.push_back(std::move(stroke));
            }
            stroke.clear();
            penDown = false;
        } else if (command.type == vrcdraw::MouseCommandType::Move) {
            current.x += command.dx;
            current.y += command.dy;
            if (penDown) {
                stroke.push_back(toLogical(current));
            }
        }
    }
    return path;
}

bool HaveIdenticalCommands(
    const vrcdraw::ExecutionPlan& left,
    const vrcdraw::ExecutionPlan& right)
{
    if (left.commands.size() != right.commands.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.commands.size(); ++index) {
        const auto& a = left.commands[index];
        const auto& b = right.commands[index];
        if (a.type != b.type || a.dx != b.dx || a.dy != b.dy ||
            a.duration != b.duration) {
            return false;
        }
    }
    return true;
}

vrcdraw::BinaryImage InkAroundStrokes(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::vector<vrcdraw::Stroke>& strokes,
    const int radius = 2)
{
    vrcdraw::BinaryImage ink{
        .width = width,
        .height = height,
        .pixels = std::vector<std::uint8_t>(
            static_cast<std::size_t>(width) * height, 0),
    };
    const auto mark = [&](const vrcdraw::PointF point) {
        const int centerX = static_cast<int>(std::lround(point.x));
        const int centerY = static_cast<int>(std::lround(point.y));
        for (int offsetY = -radius; offsetY <= radius; ++offsetY) {
            for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
                const int x = centerX + offsetX;
                const int y = centerY + offsetY;
                if (x >= 0 && y >= 0 && x < static_cast<int>(width) &&
                    y < static_cast<int>(height)) {
                    ink.pixels[static_cast<std::size_t>(y) * width +
                               static_cast<std::size_t>(x)] = 1;
                }
            }
        }
    };
    for (const auto& stroke : strokes) {
        for (std::size_t index = 1; index < stroke.size(); ++index) {
            const auto start = stroke[index - 1];
            const auto end = stroke[index];
            const int steps = std::max(
                1,
                static_cast<int>(std::ceil(
                    std::hypot(end.x - start.x, end.y - start.y) * 2.0F)));
            for (int step = 0; step <= steps; ++step) {
                const float ratio = static_cast<float>(step) / static_cast<float>(steps);
                mark(vrcdraw::PointF{
                    start.x + (end.x - start.x) * ratio,
                    start.y + (end.y - start.y) * ratio,
                });
            }
        }
    }
    return ink;
}

std::pair<std::size_t, std::size_t> InkComponentCoverage(
    const vrcdraw::DrawingPath& path,
    const vrcdraw::BinaryImage& ink)
{
    std::vector<std::uint8_t> routeSupport(ink.pixels.size(), 0);
    const auto markNearestInk = [&](const vrcdraw::PointF point) {
        const int centerX = static_cast<int>(std::lround(point.x));
        const int centerY = static_cast<int>(std::lround(point.y));
        float bestDistance = std::numeric_limits<float>::infinity();
        std::size_t best = ink.pixels.size();
        for (int offsetY = -1; offsetY <= 1; ++offsetY) {
            for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                const int x = centerX + offsetX;
                const int y = centerY + offsetY;
                if (x < 0 || y < 0 || x >= static_cast<int>(ink.width) ||
                    y >= static_cast<int>(ink.height)) {
                    continue;
                }
                const std::size_t index = static_cast<std::size_t>(y) * ink.width +
                                          static_cast<std::size_t>(x);
                const float distance = std::hypot(
                    static_cast<float>(x) - point.x,
                    static_cast<float>(y) - point.y);
                if (ink.pixels[index] != 0 && distance < bestDistance) {
                    bestDistance = distance;
                    best = index;
                }
            }
        }
        if (best != ink.pixels.size()) {
            routeSupport[best] = 1;
        }
    };
    for (const vrcdraw::Stroke& stroke : path.strokes) {
        for (std::size_t index = 1; index < stroke.size(); ++index) {
            const vrcdraw::PointF start = stroke[index - 1];
            const vrcdraw::PointF end = stroke[index];
            const int steps = std::max(
                1,
                static_cast<int>(std::ceil(
                    std::hypot(end.x - start.x, end.y - start.y) * 2.0F)));
            for (int step = 0; step <= steps; ++step) {
                const float ratio = static_cast<float>(step) / static_cast<float>(steps);
                markNearestInk(vrcdraw::PointF{
                    start.x + (end.x - start.x) * ratio,
                    start.y + (end.y - start.y) * ratio,
                });
            }
        }
    }

    std::vector<std::uint8_t> visited(ink.pixels.size(), 0);
    std::queue<std::size_t> pending;
    std::size_t components = 0;
    std::size_t represented = 0;
    for (std::size_t start = 0; start < ink.pixels.size(); ++start) {
        if (ink.pixels[start] == 0 || visited[start] != 0) {
            continue;
        }
        ++components;
        bool hasRoute = false;
        visited[start] = 1;
        pending.push(start);
        while (!pending.empty()) {
            const std::size_t current = pending.front();
            pending.pop();
            hasRoute = hasRoute || routeSupport[current] != 0;
            const int x = static_cast<int>(current % ink.width);
            const int y = static_cast<int>(current / ink.width);
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int neighborX = x + offsetX;
                    const int neighborY = y + offsetY;
                    if ((offsetX == 0 && offsetY == 0) || neighborX < 0 || neighborY < 0 ||
                        neighborX >= static_cast<int>(ink.width) ||
                        neighborY >= static_cast<int>(ink.height)) {
                        continue;
                    }
                    const std::size_t neighbor =
                        static_cast<std::size_t>(neighborY) * ink.width +
                        static_cast<std::size_t>(neighborX);
                    if (ink.pixels[neighbor] != 0 && visited[neighbor] == 0) {
                        visited[neighbor] = 1;
                        pending.push(neighbor);
                    }
                }
            }
        }
        represented += hasRoute;
    }
    return {represented, components};
}

void TestPathSimplification()
{
    const vrcdraw::Stroke straight{{0, 0}, {1, 0}, {2, 0}, {3, 0}};
    const auto simplifiedStraight = vrcdraw::SimplifyRdp(straight, 0.1F);
    assert(simplifiedStraight.size() == 2);

    const vrcdraw::Stroke corner{{0, 0}, {1, 0}, {1, 1}, {2, 1}};
    const auto simplifiedCorner = vrcdraw::SimplifyRdp(corner, 0.1F);
    assert(simplifiedCorner.size() >= 3);
}

void TestMouseInterpolation()
{
    const vrcdraw::MousePoint start{0, 0};
    const vrcdraw::MousePoint target{12, 3};
    const auto deltas = vrcdraw::InterpolateMouseLine(start, target, 3);
    assert(!deltas.empty());

    vrcdraw::MousePoint current = start;
    for (const auto delta : deltas) {
        assert(std::abs(delta.dx) <= 3);
        assert(std::abs(delta.dy) <= 3);
        current.x += delta.dx;
        current.y += delta.dy;
        const float distance = vrcdraw::PointSegmentDistance(
            vrcdraw::PointF{static_cast<float>(current.x), static_cast<float>(current.y)},
            vrcdraw::PointF{0, 0},
            vrcdraw::PointF{12, 3});
        assert(distance <= 1.0F);
    }
    assert(current == target);

    const auto negative = vrcdraw::InterpolateMouseLine({4, 8}, {-3, -17}, 2);
    current = {4, 8};
    for (const auto delta : negative) {
        assert(std::abs(delta.dx) <= 2);
        assert(std::abs(delta.dy) <= 2);
        current.x += delta.dx;
        current.y += delta.dy;
    }
    assert(current == vrcdraw::MousePoint(-3, -17));
}

void TestMouseInputModeHelpers()
{
    assert(vrcdraw::RecommendedProbeDistance(1) == 96);
    assert(vrcdraw::RecommendedProbeDistance(10) == 10);
    assert(vrcdraw::RecommendedProbeDistance(20) == 8);
    assert(vrcdraw::RecommendedProbeDistance(0) == 96);

    const vrcdraw::MousePoint origin{500, 400};
    const vrcdraw::MousePoint injected{12, 0};
    const std::array desktopSamples{
        vrcdraw::MousePoint{512, 400},
        vrcdraw::MousePoint{512, 400},
    };
    assert(vrcdraw::ClassifyCursorBehavior(origin, injected, desktopSamples) ==
           vrcdraw::MouseInputMode::DesktopAbsolute);

    const std::array lockedSamples{
        vrcdraw::MousePoint{512, 400},
        vrcdraw::MousePoint{500, 400},
    };
    assert(vrcdraw::ClassifyCursorBehavior(origin, injected, lockedSamples) ==
           vrcdraw::MouseInputMode::Relative);

    const std::array unstableSamples{
        vrcdraw::MousePoint{505, 400},
        vrcdraw::MousePoint{510, 400},
    };
    assert(vrcdraw::ClassifyCursorBehavior(origin, injected, unstableSamples) ==
           vrcdraw::MouseInputMode::Undetermined);

    const vrcdraw::DesktopRect desktop{-1920, 0, 3840, 1080};
    assert(vrcdraw::FitsDesktopRect(
        {-100, 500}, {-500, -300}, {500, 300}, desktop));
    assert(!vrcdraw::FitsDesktopRect(
        {1800, 500}, {-100, -100}, {200, 100}, desktop));
    assert(vrcdraw::NormalizeDesktopPoint({-1920, 0}, desktop) ==
           vrcdraw::MousePoint(0, 0));
    assert(vrcdraw::NormalizeDesktopPoint({1919, 1079}, desktop) ==
           vrcdraw::MousePoint(65535, 65535));
}

void TestExecutionPlan()
{
    vrcdraw::DrawingPath path{
        .width = 32,
        .height = 32,
        .strokes = {{{4, 8}, {16, 11}, {27, 25}}},
    };
    const vrcdraw::ExecutionOptions options{};
    const auto plan = vrcdraw::BuildExecutionPlan(path, options);
    assert(plan.strokeCount == 1);
    assert(plan.metrics.leftDownCount == plan.strokeCount);
    assert(plan.metrics.strokesAfterOptimization == plan.strokeCount);
    assert(plan.metrics.penDownMoveTime + plan.metrics.penUpMoveTime +
               plan.metrics.buttonGuardWait + plan.metrics.fixedButtonWait ==
           plan.estimatedDuration);
    assert(vrcdraw::RemainingPlannedDuration(plan, 0) == plan.estimatedDuration);
    assert(vrcdraw::RemainingPlannedDuration(plan, plan.commands.size()).count() == 0);
    assert(vrcdraw::PlannedProgress(plan, 0) == 0.0F);
    assert(vrcdraw::PlannedProgress(plan, plan.commands.size()) == 1.0F);
    assert(vrcdraw::RemainingPlannedDuration(
               plan, std::chrono::milliseconds{}) == plan.estimatedDuration);
    assert(vrcdraw::RemainingPlannedDuration(
               plan, plan.estimatedDuration).count() == 0);
    assert(vrcdraw::PlannedProgress(
               plan, std::chrono::milliseconds{}) == 0.0F);
    assert(vrcdraw::PlannedProgress(
               plan, plan.estimatedDuration) == 1.0F);
    assert(!plan.commands.empty());
    assert(plan.estimatedDuration.count() > 0);

    bool penDown = false;
    bool sawPenDownMove = false;
    bool sawTravelMove = false;
    std::size_t downCount = 0;
    std::size_t upCount = 0;
    vrcdraw::MousePoint current{};
    for (const auto& command : plan.commands) {
        switch (command.type) {
        case vrcdraw::MouseCommandType::LeftDown:
            penDown = true;
            ++downCount;
            break;
        case vrcdraw::MouseCommandType::LeftUp:
            penDown = false;
            ++upCount;
            break;
        case vrcdraw::MouseCommandType::Move:
            assert(std::abs(command.dx) <=
                   (penDown ? options.maximumPenDownStep : options.maximumPenUpStep));
            assert(std::abs(command.dy) <=
                   (penDown ? options.maximumPenDownStep : options.maximumPenUpStep));
            sawTravelMove = sawTravelMove || !penDown;
            current.x += command.dx;
            current.y += command.dy;
            sawPenDownMove = sawPenDownMove || penDown;
            break;
        case vrcdraw::MouseCommandType::Wait:
            break;
        }
    }
    assert(!penDown);
    assert(sawPenDownMove);
    assert(sawTravelMove);
    assert(downCount == 1);
    assert(upCount == 1);
    assert(current == vrcdraw::MousePoint(11, 9));
    assert(plan.minimumMousePosition.x <= current.x);
    assert(plan.minimumMousePosition.y <= current.y);
    assert(plan.maximumMousePosition.x >= current.x);
    assert(plan.maximumMousePosition.y >= current.y);
    assert(plan.hasDrawingPosition);

    for (std::size_t index = 0; index < plan.commands.size(); ++index) {
        if (plan.commands[index].type == vrcdraw::MouseCommandType::Move) {
            assert(index + 1 < plan.commands.size());
            assert(plan.commands[index + 1].type == vrcdraw::MouseCommandType::Wait);
            assert(
                plan.commands[index + 1].duration == options.penDownMoveInterval ||
                plan.commands[index + 1].duration == options.penUpMoveInterval);
        }
        if (plan.commands[index].type == vrcdraw::MouseCommandType::LeftUp) {
            assert(index + 1 < plan.commands.size());
            assert(plan.commands[index + 1].type == vrcdraw::MouseCommandType::Wait);
            assert(plan.commands[index + 1].duration == std::chrono::milliseconds(32));
        }
    }

    const vrcdraw::DrawingPath shortStrokes{
        .width = 32,
        .height = 32,
        .strokes = {
            {{15, 15}, {16, 15}},
            {{16, 16}, {17, 16}},
            {{17, 17}, {18, 17}},
        },
    };
    const auto guardedPlan = vrcdraw::BuildExecutionPlan(shortStrokes, options);
    bool hasPreviousDown = false;
    std::chrono::milliseconds elapsed{};
    std::chrono::milliseconds previousDownAt{};
    for (const auto& command : guardedPlan.commands) {
        if (command.type == vrcdraw::MouseCommandType::Wait) {
            elapsed += command.duration;
        } else if (command.type == vrcdraw::MouseCommandType::LeftDown) {
            if (hasPreviousDown) {
                assert(elapsed - previousDownAt >= options.minimumButtonDownInterval);
            }
            previousDownAt = elapsed;
            hasPreviousDown = true;
        }
    }
    assert(guardedPlan.strokeCount == 3);

    const vrcdraw::DrawingPath highResolutionPath{
        .width = 1536,
        .height = 1024,
        .strokes = {{{0, 0}, {1535, 1023}}},
    };
    const auto scaledPlan = vrcdraw::BuildExecutionPlan(highResolutionPath, options);
    assert(std::abs(scaledPlan.mouseScale - 0.5F) < 1.0e-5F);
    vrcdraw::MousePoint scaledCurrent{};
    vrcdraw::MousePoint minimum{};
    vrcdraw::MousePoint maximum{};
    for (const auto& command : scaledPlan.commands) {
        if (command.type != vrcdraw::MouseCommandType::Move) {
            continue;
        }
        scaledCurrent.x += command.dx;
        scaledCurrent.y += command.dy;
        minimum.x = std::min(minimum.x, scaledCurrent.x);
        minimum.y = std::min(minimum.y, scaledCurrent.y);
        maximum.x = std::max(maximum.x, scaledCurrent.x);
        maximum.y = std::max(maximum.y, scaledCurrent.y);
    }
    assert(maximum.x - minimum.x <= options.maximumDrawingDimension);
    assert(maximum.y - minimum.y <= options.maximumDrawingDimension);
    const float restoredX = static_cast<float>(scaledCurrent.x) / scaledPlan.mouseScale +
                            static_cast<float>(highResolutionPath.width) * 0.5F;
    const float restoredY = static_cast<float>(scaledCurrent.y) / scaledPlan.mouseScale +
                            static_cast<float>(highResolutionPath.height) * 0.5F;
    assert(std::abs(restoredX - 1535.0F) <= 1.0F);
    assert(std::abs(restoredY - 1023.0F) <= 1.0F);

    vrcdraw::ExecutionOptions smallOptions{};
    smallOptions.mouseScale = 0.3F;
    const auto smallPlan = vrcdraw::BuildExecutionPlan(highResolutionPath, smallOptions);
    vrcdraw::ExecutionOptions largeOptions{};
    largeOptions.mouseScale = 3.0F;
    const auto largePlan = vrcdraw::BuildExecutionPlan(highResolutionPath, largeOptions);
    assert(std::abs(smallPlan.requestedMouseScale - 0.3F) < 1.0e-5F);
    assert(std::abs(largePlan.requestedMouseScale - 3.0F) < 1.0e-5F);
    const int defaultWidth = scaledPlan.maximumDrawingPosition.x -
        scaledPlan.minimumDrawingPosition.x;
    const int smallWidth = smallPlan.maximumDrawingPosition.x -
        smallPlan.minimumDrawingPosition.x;
    const int largeWidth = largePlan.maximumDrawingPosition.x -
        largePlan.minimumDrawingPosition.x;
    assert(std::abs(smallWidth - defaultWidth * 0.3F) <= 2.0F);
    assert(std::abs(largeWidth - defaultWidth * 3.0F) <= 2.0F);
    for (const auto& command : largePlan.commands) {
        if (command.type == vrcdraw::MouseCommandType::Move) {
            assert(std::abs(command.dx) <= largeOptions.maximumPenUpStep);
            assert(std::abs(command.dy) <= largeOptions.maximumPenUpStep);
        }
    }
}

void TestLosslessPathOptimizer()
{
    struct OptimizerCase {
        std::string_view name;
        vrcdraw::DrawingPath path;
        std::size_t expectedStrokes{};
    };
    const vrcdraw::PointF center{32, 32};
    const std::array cases{
        OptimizerCase{
            "chain",
            vrcdraw::DrawingPath{
                .width = 64,
                .height = 64,
                .strokes = {
                    {{4, 32}, {16, 32}},
                    {{16, 32}, {32, 32}},
                    {{32, 32}, {60, 32}},
                },
            },
            1,
        },
        OptimizerCase{
            "x",
            vrcdraw::DrawingPath{
                .width = 64,
                .height = 64,
                .strokes = {
                    {{4, 4}, center},
                    {center, {60, 60}},
                    {{60, 4}, center},
                    {center, {4, 60}},
                },
            },
            2,
        },
        OptimizerCase{
            "t",
            vrcdraw::DrawingPath{
                .width = 64,
                .height = 64,
                .strokes = {
                    {{4, 16}, center},
                    {center, {60, 16}},
                    {center, {32, 60}},
                },
            },
            2,
        },
        OptimizerCase{
            "disconnected",
            vrcdraw::DrawingPath{
                .width = 64,
                .height = 64,
                .strokes = {
                    {{4, 8}, {20, 8}},
                    {{40, 54}, {60, 54}},
                },
            },
            2,
        },
        OptimizerCase{
            "near-but-separate",
            vrcdraw::DrawingPath{
                .width = 64,
                .height = 64,
                .strokes = {
                    {{4, 20}, {30, 20}},
                    {{30.01F, 20}, {60, 20}},
                },
            },
            2,
        },
        OptimizerCase{
            "tap-only",
            vrcdraw::DrawingPath{
                .width = 1536,
                .height = 1536,
                .strokes = {
                    {{100, 100}, {101, 100}},
                    {{101, 100}, {100, 100}},
                },
            },
            2,
        },
    };

    for (const auto& testCase : cases) {
        const auto baselinePlan = vrcdraw::BuildExecutionPlan(testCase.path);
        const auto optimized = vrcdraw::OptimizeDrawingPathLossless(testCase.path);
        const auto optimizedPlan = vrcdraw::BuildExecutionPlan(optimized);
        std::cout << "Optimizer " << testCase.name
                  << ": strokes=" << testCase.path.strokes.size()
                  << " -> " << optimized.strokes.size()
                  << ", duration_ms=" << baselinePlan.estimatedDuration.count()
                  << " -> " << optimizedPlan.estimatedDuration.count() << std::endl;
        assert(optimized.optimization.exactSegmentMatch);
        assert(optimized.optimization.exactExecutionMatch);
        assert(optimized.optimization.applied);
        assert(optimized.strokes.size() == testCase.expectedStrokes);
        assert(optimized.sourceEdgeEnds.size() == optimized.strokes.size());
        assert(vrcdraw::HaveConsistentRouteMetadata(optimized));
        for (std::size_t stroke = 0; stroke < optimized.strokes.size(); ++stroke) {
            if (!optimized.sourceEdgeEnds[stroke].empty()) {
                assert(optimized.sourceEdgeEnds[stroke].back() ==
                       optimized.strokes[stroke].size() - 1);
            }
        }
        assert(vrcdraw::HaveIdenticalPathSegments(testCase.path, optimized));
        assert(PenDownCoverage(baselinePlan) == PenDownCoverage(optimizedPlan));
        assert(optimizedPlan.estimatedDuration <= baselinePlan.estimatedDuration);
        assert(optimizedPlan.metrics.leftDownCount == optimized.strokes.size());
    }

    auto changed = cases[1].path;
    changed.strokes.front().back().x += 1.0F;
    assert(!vrcdraw::HaveIdenticalPathSegments(cases[1].path, changed));
}

void TestStageThreeVectorPath()
{
    const vrcdraw::Stroke smooth{
        {10, 72}, {18, 63}, {28, 55}, {40, 49}, {54, 46},
        {68, 47}, {82, 52}, {94, 60}, {104, 71}, {112, 84},
    };
    vrcdraw::DrawingPath smoothPath{
        .width = 128,
        .height = 128,
        .strokes = {smooth},
        .sourceEdgeEnds = {},
        .optimization = {},
        .routeMetadata = {
            vrcdraw::StrokeRouteMetadata{
                .spans = {
                    vrcdraw::RouteSpan{
                        .pointBegin = 0,
                        .pointEnd = smooth.size() - 1,
                        .regionType = vrcdraw::LineRegionType::ThinLine,
                        .beginAnchorFlags = vrcdraw::AnchorGraphEndpoint,
                        .endAnchorFlags = vrcdraw::AnchorGraphEndpoint,
                    },
                },
                .closed = false,
            },
        },
    };
    const auto smoothInk = InkAroundStrokes(128, 128, smoothPath.strokes, 3);
    const auto fitted = vrcdraw::FitVectorDrawingPath(smoothPath, smoothInk);
    assert(fitted.strokes.size() == 1);
    assert(fitted.fitting.fittedSpans > 0);
    assert(fitted.fitting.vectorSegments < fitted.fitting.sourcePolylineSegments);
    const auto flattened = vrcdraw::FlattenVectorDrawingPath(fitted, 0.15F);
    assert(flattened.strokes.size() == 1);
    assert(flattened.strokes.front().front() == smooth.front());
    assert(flattened.strokes.front().back() == smooth.back());

    auto reversedPath = smoothPath;
    std::ranges::reverse(reversedPath.strokes.front());
    std::swap(
        reversedPath.routeMetadata.front().spans.front().beginAnchorFlags,
        reversedPath.routeMetadata.front().spans.front().endAnchorFlags);
    const auto reverseFitted = vrcdraw::FitVectorDrawingPath(reversedPath, smoothInk);
    const auto reverseFlattened = vrcdraw::FlattenVectorDrawingPath(reverseFitted, 0.15F);
    auto expectedReverse = flattened.strokes.front();
    std::ranges::reverse(expectedReverse);
    assert(reverseFlattened.strokes.front() == expectedReverse);
    const auto penDownPoints = [](const vrcdraw::ExecutionPlan& plan) {
        std::vector<vrcdraw::MousePoint> points;
        vrcdraw::MousePoint current{};
        bool penDown = false;
        for (const auto& command : plan.commands) {
            if (command.type == vrcdraw::MouseCommandType::LeftDown) {
                penDown = true;
                points.push_back(current);
            } else if (command.type == vrcdraw::MouseCommandType::LeftUp) {
                penDown = false;
            } else if (command.type == vrcdraw::MouseCommandType::Move) {
                current.x += command.dx;
                current.y += command.dy;
                if (penDown) {
                    points.push_back(current);
                }
            }
        }
        return points;
    };
    vrcdraw::ExecutionOptions directionOptions{};
    directionOptions.mouseScale = 3.0F;
    const auto forwardDirectionPlan =
        vrcdraw::BuildExecutionPlan(fitted, smoothInk, directionOptions);
    const auto reverseDirectionPlan =
        vrcdraw::BuildExecutionPlan(reverseFitted, smoothInk, directionOptions);
    auto forwardPoints = penDownPoints(forwardDirectionPlan);
    const auto reversePoints = penDownPoints(reverseDirectionPlan);
    std::ranges::reverse(forwardPoints);
    assert(forwardPoints == reversePoints);

    const vrcdraw::Stroke sharp{
        {12, 96}, {28, 96}, {44, 96}, {60, 96},
        {60, 80}, {60, 64}, {60, 48}, {60, 32},
    };
    vrcdraw::DrawingPath sharpPath{
        .width = 128,
        .height = 128,
        .strokes = {sharp},
        .routeMetadata = {
            vrcdraw::StrokeRouteMetadata{
                .spans = {
                    vrcdraw::RouteSpan{
                        .pointBegin = 0,
                        .pointEnd = sharp.size() - 1,
                        .regionType = vrcdraw::LineRegionType::ThinLine,
                        .beginAnchorFlags = vrcdraw::AnchorGraphEndpoint,
                        .endAnchorFlags = vrcdraw::AnchorGraphEndpoint,
                    },
                },
            },
        },
    };
    const auto sharpInk = InkAroundStrokes(128, 128, sharpPath.strokes, 2);
    const auto sharpFitted = vrcdraw::FitVectorDrawingPath(sharpPath, sharpInk);
    assert(sharpFitted.strokes.front().spans.size() == 2);
    assert((sharpFitted.strokes.front().spans[0].endAnchorFlags &
            vrcdraw::AnchorProtectedCorner) != 0);
    assert((sharpFitted.strokes.front().spans[1].beginAnchorFlags &
            vrcdraw::AnchorProtectedCorner) != 0);
    assert(sharpFitted.strokes.front().spans[0].fallbackPolyline.back() ==
           vrcdraw::PointF(60, 96));

    for (const float scale : std::array{0.3F, 1.0F, 3.0F}) {
        vrcdraw::ExecutionOptions options{};
        options.mouseScale = scale;
        const auto plan = vrcdraw::BuildExecutionPlan(fitted, smoothInk, options);
        assert(plan.strokeCount == 1);
        assert(plan.metrics.fittedSpanCount == fitted.fitting.fittedSpans);
        assert(plan.metrics.acceptedFittedSpanCount +
                   plan.metrics.scaleFallbackSpanCount ==
               plan.metrics.fittedSpanCount);
        assert(plan.metrics.finalPenDownSamplePoints >= 2);
    }

    const vrcdraw::Stroke fallback{{16, 64}, {48, 64}, {80, 64}, {112, 64}};
    const auto fallbackInk = InkAroundStrokes(128, 128, {fallback}, 1);
    const vrcdraw::VectorDrawingPath unsafeVector{
        .width = 128,
        .height = 128,
        .strokes = {
            vrcdraw::VectorStroke{
                .spans = {
                    vrcdraw::FittedRouteSpan{
                        .start = fallback.front(),
                        .segments = {
                            vrcdraw::VectorSegment{
                                .type = vrcdraw::VectorSegmentType::CubicBezier,
                                .lineEnd = fallback.back(),
                                .cubic = vrcdraw::CubicBezier{
                                    fallback.front(), {24, 8}, {104, 8}, fallback.back(),
                                },
                            },
                        },
                        .fallbackPolyline = fallback,
                        .regionType = vrcdraw::LineRegionType::ThinLine,
                        .beginAnchorFlags = vrcdraw::AnchorGraphEndpoint,
                        .endAnchorFlags = vrcdraw::AnchorGraphEndpoint,
                        .fitted = true,
                    },
                },
            },
        },
    };
    const auto unsafePlan = vrcdraw::BuildExecutionPlan(unsafeVector, fallbackInk);
    const vrcdraw::DrawingPath fallbackPath{
        .width = 128,
        .height = 128,
        .strokes = {fallback},
    };
    const auto fallbackPlan = vrcdraw::BuildExecutionPlan(fallbackPath);
    assert(unsafePlan.metrics.fittedSpanCount == 1);
    assert(unsafePlan.metrics.acceptedFittedSpanCount == 0);
    assert(unsafePlan.metrics.scaleFallbackSpanCount == 1);
    assert(PenDownCoverage(unsafePlan) == PenDownCoverage(fallbackPlan));
}

void TestSettings(const std::filesystem::path& directory)
{
    const auto path = directory / L"settings-test.ini";
    const vrcdraw::Settings expected{
        .futureStrokeLimit = 777,
        .drawingScale = 2.35F,
        .controlPanelWidth = 386.5F,
        .drawingHotkeyVirtualKey = 0x79,
        .drawingHotkeyModifiers = 0x02,
        .vectorPathEnabled = false,
    };
    assert(vrcdraw::SaveSettings(path, expected));
    const auto actual = vrcdraw::LoadSettings(path);
    assert(actual.futureStrokeLimit == expected.futureStrokeLimit);
    assert(std::abs(actual.drawingScale - expected.drawingScale) < 1.0e-5F);
    assert(std::abs(actual.controlPanelWidth - expected.controlPanelWidth) < 1.0e-5F);
    assert(actual.drawingHotkeyVirtualKey == expected.drawingHotkeyVirtualKey);
    assert(actual.drawingHotkeyModifiers == expected.drawingHotkeyModifiers);
    assert(actual.vectorPathEnabled == expected.vectorPathEnabled);

    {
        std::ofstream invalid(path, std::ios::trunc);
        invalid << "[drawing]\n"
                << "future_stroke_limit=5000\n"
                << "drawing_scale=9.50\n"
                << "control_panel_width=900.00\n"
                << "drawing_hotkey_vk=1\n";
    }
    const auto clamped = vrcdraw::LoadSettings(path);
    assert(clamped.futureStrokeLimit == 1000);
    assert(std::abs(clamped.drawingScale - 3.0F) < 1.0e-5F);
    assert(std::abs(clamped.controlPanelWidth - 480.0F) < 1.0e-5F);
    assert(clamped.drawingHotkeyVirtualKey == 0x77);
    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestProgramIcon()
{
    const auto path =
        std::filesystem::path(VRC_DRAW_SOURCE_DIR) / L"resources" / L"VRC-Draw.ico";
    std::ifstream icon(path, std::ios::binary);
    assert(icon.good());

    std::array<unsigned char, 6> header{};
    icon.read(reinterpret_cast<char*>(header.data()), header.size());
    assert(icon.gcount() == static_cast<std::streamsize>(header.size()));
    assert(header[0] == 0 && header[1] == 0);
    assert(header[2] == 1 && header[3] == 0);
    const unsigned int imageCount = header[4] | (static_cast<unsigned int>(header[5]) << 8U);
    assert(imageCount == 9);

    std::array<unsigned int, 9> actualSizes{};
    for (unsigned int index = 0; index < imageCount; ++index) {
        std::array<unsigned char, 16> entry{};
        icon.read(reinterpret_cast<char*>(entry.data()), entry.size());
        assert(icon.gcount() == static_cast<std::streamsize>(entry.size()));
        actualSizes[index] = entry[0] == 0 ? 256U : entry[0];
        const unsigned int height = entry[1] == 0 ? 256U : entry[1];
        assert(actualSizes[index] == height);
    }
    constexpr std::array<unsigned int, 9> expectedSizes{
        16, 20, 24, 32, 40, 48, 64, 128, 256};
    assert(actualSizes == expectedSizes);

    for (const unsigned int size : expectedSizes) {
        const auto individualPath = std::filesystem::path(VRC_DRAW_SOURCE_DIR) /
                                    L"resources" / L"icons" /
                                    (L"VRC-Draw-" + std::to_wstring(size) + L".ico");
        std::ifstream individual(individualPath, std::ios::binary);
        assert(individual.good());
        std::array<unsigned char, 22> data{};
        individual.read(reinterpret_cast<char*>(data.data()), data.size());
        assert(individual.gcount() == static_cast<std::streamsize>(data.size()));
        assert(data[4] == 1 && data[5] == 0);
        const unsigned int width = data[6] == 0 ? 256U : data[6];
        const unsigned int height = data[7] == 0 ? 256U : data[7];
        assert(width == size && height == size);
    }
}

void TestImageProcessing()
{
    const auto path = WriteTestBitmap();
    const auto result = vrcdraw::ProcessImage(path);
    if (!result) {
        std::wcerr << L"Image processing failed: " << result.error() << L'\n';
    }
    assert(result.has_value());
    assert(result->width == 64);
    assert(result->height == 64);
    assert(!result->lineArt.strokes.strokes.empty());
    assert(result->lineArt.cleanLineArt.width == 64);
    assert(result->lineArt.cleanLineArt.height == 64);
    assert(result->lineArt.cleanLineArt.pixels.size() == 64U * 64U);
    assert(result->lineArt.coverageLineArt.width == 64);
    assert(result->lineArt.coverageLineArt.height == 64);
    assert(result->lineArt.coverageLineArt.pixels.size() == 64U * 64U);
    assert(std::ranges::any_of(
        result->lineArt.coverageLineArt.pixels,
        [](const std::uint8_t value) { return value > 0 && value < 255; }));

    // The line-art document preserves source ink width; skeletonization belongs only to routes.
    std::size_t horizontalLinePixels = 0;
    for (std::size_t y = 5; y <= 15; ++y) {
        horizontalLinePixels +=
            result->lineArt.cleanLineArt.pixels[y * 64U + 32U] != 0;
    }
    std::size_t verticalLinePixels = 0;
    for (std::size_t x = 5; x <= 15; ++x) {
        verticalLinePixels +=
            result->lineArt.cleanLineArt.pixels[32U * 64U + x] != 0;
    }
    assert(horizontalLinePixels >= 3);
    assert(verticalLinePixels >= 3);

    const auto rendered = vrcdraw::RenderGrayscaleLineArt(
        result->lineArt.coverageLineArt);
    assert(rendered.bgra.size() == 64U * 64U * 4U);
    const bool hasDarkPixel = std::ranges::any_of(rendered.bgra, [](const std::uint8_t value) {
        return value < 128;
    });
    assert(hasDarkPixel);

    const auto routeRendered = vrcdraw::RenderLineArt(result->lineArt.strokes, 64, 64);
    std::size_t lineArtInk = 0;
    std::size_t routeInk = 0;
    for (std::size_t pixel = 0; pixel < 64U * 64U; ++pixel) {
        lineArtInk += rendered.bgra[pixel * 4U] < 128;
        routeInk += routeRendered.bgra[pixel * 4U] < 128;
    }
    assert(lineArtInk > routeInk);

    const auto exportDirectory = std::filesystem::current_path() / L"vrcdraw-export-test";
    std::error_code error;
    std::filesystem::remove_all(exportDirectory, error);
    assert(!error);
    const auto exported = vrcdraw::ExportLineArt(*result, exportDirectory);
    if (!exported) {
        std::wcerr << L"Line-art export failed: " << exported.error() << L'\n';
    }
    assert(exported.has_value());
    assert(std::filesystem::file_size(exported->pngPath) > 8);
    assert(std::filesystem::file_size(exported->svgPath) > 32);
    {
        std::ifstream svg(exported->svgPath, std::ios::binary);
        const std::string contents{
            std::istreambuf_iterator<char>{svg}, std::istreambuf_iterator<char>{}};
        assert(contents.contains("<path"));
        assert(!contents.contains("<rect x=\""));
        assert(contents.contains("fill-rule=\"evenodd\""));
        assert(std::ranges::count(contents, 'M') >= 2);
    }
    std::string svgText;
    {
        std::ifstream svgInput(exported->svgPath, std::ios::binary);
        svgText.assign(
            std::istreambuf_iterator<char>(svgInput), std::istreambuf_iterator<char>{});
    }
    assert(svgText.contains("<path"));
    assert(!svgText.contains("<rect x=\""));
    vrcdraw::ProcessedImage scaledResult = *result;
    scaledResult.originalWidth = 128;
    scaledResult.originalHeight = 128;
    const auto exportedAgain = vrcdraw::ExportLineArt(scaledResult, exportDirectory);
    assert(exportedAgain.has_value());
    assert(exportedAgain->pngPath.stem() == exportedAgain->svgPath.stem());
    assert(exportedAgain->pngPath != exported->pngPath);
    {
        std::ifstream scaledSvg(exportedAgain->svgPath, std::ios::binary);
        const std::string contents{
            std::istreambuf_iterator<char>{scaledSvg},
            std::istreambuf_iterator<char>{}};
        assert(contents.contains("viewBox=\"0 0 64 64\""));
        assert(contents.contains("width=\"128\" height=\"128\""));
    }
    error.clear();
    std::filesystem::remove_all(exportDirectory, error);
    assert(!error);
    assert(!std::filesystem::exists(exportDirectory));
    std::filesystem::remove(path, error);
}

void TestLineArtSeparation()
{
    const auto path = WriteMaskBitmap(
        L"vrcdraw-near-parallel-lines.bmp",
        96,
        96,
        [](const int x, const int y) {
            return y >= 12 && y <= 83 &&
                   ((x >= 29 && x <= 31) || (x >= 35 && x <= 37));
        });
    const auto result = vrcdraw::ProcessImage(path);
    assert(result.has_value());
    const auto& topology = result->lineArt.cleanLineArt;
    std::size_t separatedRows = 0;
    for (int y = 18; y <= 77; ++y) {
        bool gapIsClear = true;
        for (int x = 32; x <= 34; ++x) {
            gapIsClear = gapIsClear &&
                topology.pixels[static_cast<std::size_t>(y) * topology.width +
                                static_cast<std::size_t>(x)] == 0;
        }
        separatedRows += gapIsClear;
    }
    assert(separatedRows >= 57);
    std::error_code error;
    std::filesystem::remove(path, error);
    assert(!error);
}

void TestJunctionPathQuality()
{
    struct JunctionCase {
        std::wstring_view name;
        std::vector<std::pair<vrcdraw::PointF, vrcdraw::PointF>> segments;
        std::size_t expectedStrokes{};
    };
    const std::array cases{
        JunctionCase{
            L"vrcdraw-x-junction.bmp",
            {{{8, 8}, {55, 55}}, {{55, 8}, {8, 55}}},
            2,
        },
        JunctionCase{
            L"vrcdraw-t-junction.bmp",
            {{{8, 14}, {55, 14}}, {{32, 14}, {32, 55}}},
            2,
        },
        JunctionCase{
            L"vrcdraw-y-junction.bmp",
            {{{32, 32}, {8, 8}}, {{32, 32}, {55, 8}}, {{32, 32}, {32, 56}}},
            2,
        },
        JunctionCase{
            L"vrcdraw-sharp-v.bmp",
            {{{8, 8}, {32, 55}}, {{32, 55}, {55, 8}}},
            1,
        },
        JunctionCase{
            L"vrcdraw-acute-x.bmp",
            {{{5, 23}, {58, 41}}, {{5, 41}, {58, 23}}},
            2,
        },
    };

    std::error_code error;
    for (const JunctionCase& testCase : cases) {
        const auto path = WriteSegmentBitmap(testCase.name, testCase.segments);
        vrcdraw::ImageProcessingDebug debug;
        const auto result = vrcdraw::ProcessImage(path, {}, &debug);
        if (!result) {
            std::wcerr << L"Junction processing failed for " << path << L": "
                       << result.error() << L'\n';
        }
        assert(result.has_value());
        std::cout << "Junction " << path.filename().string()
                  << ": strokes=" << result->lineArt.strokes.strokes.size()
                  << ", junctions=" << debug.junctionCount
                  << ", removed_spurs=" << debug.removedSpurCount << std::endl;
        if (result->lineArt.strokes.strokes.size() != testCase.expectedStrokes) {
            for (const vrcdraw::Stroke& stroke : result->lineArt.strokes.strokes) {
                float length = 0.0F;
                for (std::size_t index = 1; index < stroke.size(); ++index) {
                    length += std::hypot(
                        stroke[index].x - stroke[index - 1].x,
                        stroke[index].y - stroke[index - 1].y);
                }
                std::cout << "  length=" << length << " from=(" << stroke.front().x
                          << ',' << stroke.front().y << ") to=(" << stroke.back().x
                          << ',' << stroke.back().y << ")" << std::endl;
            }
        }
        assert(result->lineArt.strokes.strokes.size() == testCase.expectedStrokes);
        assert(RouteOutsideInkRatio(
                   result->lineArt.strokes,
                   result->lineArt.cleanLineArt) <= 0.01);
        for (const vrcdraw::Stroke& stroke : result->lineArt.strokes.strokes) {
            assert(stroke.size() >= 2);
        }
        std::filesystem::remove(path, error);
        assert(!error);
    }
}

void TestFilledRegionPathQuality()
{
    struct FilledCase {
        std::wstring_view name;
        std::function<bool(int, int)> isInk;
        vrcdraw::PointF protectedCenter;
        float protectedRadius{};
    };
    const std::array cases{
        FilledCase{
            L"vrcdraw-filled-circle.bmp",
            [](const int x, const int y) {
                return std::hypot(static_cast<float>(x - 48),
                                  static_cast<float>(y - 48)) <= 18.0F;
            },
            {48, 48},
            8.0F,
        },
        FilledCase{
            L"vrcdraw-filled-eye.bmp",
            [](const int x, const int y) {
                const float normalizedX = static_cast<float>(x - 48) / 23.0F;
                const float normalizedY = static_cast<float>(y - 48) / 12.0F;
                return normalizedX * normalizedX + normalizedY * normalizedY <= 1.0F;
            },
            {48, 48},
            6.0F,
        },
        FilledCase{
            L"vrcdraw-thick-to-thin.bmp",
            [](const int x, const int y) {
                const vrcdraw::PointF point{
                    static_cast<float>(x), static_cast<float>(y)};
                return vrcdraw::PointSegmentDistance(point, {16, 48}, {55, 48}) <= 8.0F ||
                       vrcdraw::PointSegmentDistance(point, {55, 48}, {84, 48}) <= 2.0F;
            },
            {34, 48},
            4.0F,
        },
    };

    std::error_code error;
    for (const FilledCase& testCase : cases) {
        const auto path = WriteMaskBitmap(testCase.name, 96, 96, testCase.isInk);
        const auto result = vrcdraw::ProcessImage(path);
        if (!result) {
            std::wcerr << L"Filled-region processing failed for " << path << L": "
                       << result.error() << L'\n';
        }
        assert(result.has_value());

        std::size_t centerPoints = 0;
        std::size_t centerEndpoints = 0;
        for (const auto& stroke : result->lineArt.strokes.strokes) {
            for (const auto point : stroke) {
                centerPoints += std::hypot(
                    point.x - testCase.protectedCenter.x,
                    point.y - testCase.protectedCenter.y) < testCase.protectedRadius;
            }
            centerEndpoints += std::hypot(
                stroke.front().x - testCase.protectedCenter.x,
                stroke.front().y - testCase.protectedCenter.y) < testCase.protectedRadius;
            centerEndpoints += std::hypot(
                stroke.back().x - testCase.protectedCenter.x,
                stroke.back().y - testCase.protectedCenter.y) < testCase.protectedRadius;
        }
        std::cout << "Filled " << path.filename().string()
                  << ": strokes=" << result->lineArt.strokes.strokes.size()
                  << ", center_points=" << centerPoints
                  << ", center_endpoints=" << centerEndpoints << std::endl;
        assert(centerPoints == 0);
        assert(centerEndpoints < 3);
        assert(RouteOutsideInkRatio(
                   result->lineArt.strokes,
                   result->lineArt.cleanLineArt) <= 0.01);

        std::filesystem::remove(path, error);
        assert(!error);
    }
}

void TestStageTwoTopologyRegressions()
{
    std::error_code error;

    // Two crossing components separated by a narrow white corridor must remain two
    // junctions. This guards against the removed image-scaled junction merge.
    {
        const std::array segments{
            std::pair{vrcdraw::PointF{22, 24}, vrcdraw::PointF{38, 40}},
            std::pair{vrcdraw::PointF{38, 24}, vrcdraw::PointF{22, 40}},
            std::pair{vrcdraw::PointF{44, 24}, vrcdraw::PointF{60, 40}},
            std::pair{vrcdraw::PointF{60, 24}, vrcdraw::PointF{44, 40}},
        };
        const auto path = WriteMaskBitmap(
            L"vrcdraw-nearby-independent-junctions.bmp",
            96,
            64,
            [&](const int x, const int y) {
                const vrcdraw::PointF point{
                    static_cast<float>(x), static_cast<float>(y)};
                return std::ranges::any_of(segments, [&](const auto& segment) {
                    return vrcdraw::PointSegmentDistance(
                               point, segment.first, segment.second) <= 1.25F;
                });
            });
        vrcdraw::ImageProcessingDebug debug;
        const auto result = vrcdraw::ProcessImage(path, {}, &debug);
        assert(result.has_value());
        const auto [represented, components] = InkComponentCoverage(
            result->lineArt.strokes, result->lineArt.cleanLineArt);
        std::cout << "Stage2 nearby junctions: junctions=" << debug.junctionCount
                  << ", ports=" << debug.junctionPortCount
                  << ", strokes=" << result->lineArt.strokes.strokes.size()
                  << ", components=" << represented << '/' << components << std::endl;
        assert(debug.junctionCount == 2);
        assert(debug.junctionPortCount == 8);
        assert(result->lineArt.strokes.strokes.size() == 4);
        assert(represented == 2 && components == 2);
        assert(RouteOutsideInkRatio(
                   result->lineArt.strokes, result->lineArt.cleanLineArt) <= 0.01);
        std::filesystem::remove(path, error);
        assert(!error);
    }

    // A long filled rectangle represents one broad pen stroke, not a compact fill;
    // its route must stay on the medial axis instead of tracing a rectangular frame.
    {
        const auto path = WriteMaskBitmap(
            L"vrcdraw-elongated-thick-stroke.bmp",
            128,
            64,
            [](const int x, const int y) {
                return x >= 12 && x <= 115 && y >= 25 && y <= 38;
            });
        vrcdraw::ImageProcessingDebug debug;
        const auto result = vrcdraw::ProcessImage(path, {}, &debug);
        assert(result.has_value());
        std::cout << "Stage2 elongated stroke: elongated=" << debug.elongatedRegionCount
                  << ", compact=" << debug.compactRegionCount
                  << ", strokes=" << result->lineArt.strokes.strokes.size() << std::endl;
        assert(debug.elongatedRegionCount >= 1);
        assert(result->lineArt.strokes.strokes.size() == 1);
        assert(std::ranges::all_of(
            result->lineArt.strokes.strokes.front(),
            [](const vrcdraw::PointF point) { return std::abs(point.y - 31.5F) <= 2.0F; }));
        assert(PathHasPointNear(result->lineArt.strokes, {16, 31.5F}, 4.0F));
        assert(PathHasPointNear(result->lineArt.strokes, {111, 31.5F}, 4.0F));
        std::filesystem::remove(path, error);
        assert(!error);
    }

    // Strong, short branches attached to a broad stroke are intentional details.
    // Multi-scale/original evidence must protect them from the spur filter.
    {
        const auto path = WriteMaskBitmap(
            L"vrcdraw-protected-short-eyelash.bmp",
            112,
            72,
            [](const int x, const int y) {
                const vrcdraw::PointF point{
                    static_cast<float>(x), static_cast<float>(y)};
                return vrcdraw::PointSegmentDistance(point, {14, 44}, {92, 44}) <= 5.0F ||
                       vrcdraw::PointSegmentDistance(point, {54, 43}, {61, 27}) <= 1.5F;
            });
        vrcdraw::ImageProcessingDebug debug;
        const auto result = vrcdraw::ProcessImage(path, {}, &debug);
        assert(result.has_value());
        std::cout << "Stage2 protected eyelash: elongated=" << debug.elongatedRegionCount
                  << ", removed_spurs=" << debug.removedSpurCount
                  << ", strokes=" << result->lineArt.strokes.strokes.size() << std::endl;
        assert(debug.elongatedRegionCount >= 1);
        assert(debug.removedSpurCount == 0);
        assert(PathHasPointNear(result->lineArt.strokes, {61, 27}, 5.0F));
        assert(RouteOutsideInkRatio(
                   result->lineArt.strokes, result->lineArt.cleanLineArt) <= 0.01);
        std::filesystem::remove(path, error);
        assert(!error);
    }

    // Five valid ports must be decomposed into local pass-through/termination
    // relations; no least-squares star center or dropped endpoint is allowed.
    {
        constexpr vrcdraw::PointF center{48, 48};
        const std::array endpoints{
            vrcdraw::PointF{48, 10},
            vrcdraw::PointF{84, 31},
            vrcdraw::PointF{71, 82},
            vrcdraw::PointF{25, 82},
            vrcdraw::PointF{12, 31},
        };
        const auto path = WriteMaskBitmap(
            L"vrcdraw-five-port-junction.bmp",
            96,
            96,
            [&](const int x, const int y) {
                const vrcdraw::PointF point{
                    static_cast<float>(x), static_cast<float>(y)};
                return std::ranges::any_of(endpoints, [&](const vrcdraw::PointF endpoint) {
                    return vrcdraw::PointSegmentDistance(point, center, endpoint) <= 2.5F;
                });
            });
        vrcdraw::ImageProcessingDebug debug;
        const auto result = vrcdraw::ProcessImage(path, {}, &debug);
        assert(result.has_value());
        std::cout << "Stage2 five-port: junctions=" << debug.junctionCount
                  << ", ports=" << debug.junctionPortCount
                  << ", strokes=" << result->lineArt.strokes.strokes.size() << std::endl;
        assert(debug.junctionCount == 1);
        assert(debug.junctionPortCount == 5);
        assert(result->lineArt.strokes.strokes.size() == 3);
        for (const vrcdraw::PointF endpoint : endpoints) {
            assert(PathHasPointNear(result->lineArt.strokes, endpoint, 5.0F));
        }
        assert(RouteOutsideInkRatio(
                   result->lineArt.strokes, result->lineArt.cleanLineArt) <= 0.01);
        std::filesystem::remove(path, error);
        assert(!error);
    }

    // The most continuous tangent pair below intersects only by extending one ray
    // backwards. The candidate must be rejected and replaced by an ink-supported
    // geodesic connection, leaving no protruding tail.
    {
        const std::array segments{
            std::pair{vrcdraw::PointF{28, 28}, vrcdraw::PointF{12, 44}},
            std::pair{vrcdraw::PointF{28, 36}, vrcdraw::PointF{12, 20}},
            std::pair{vrcdraw::PointF{36, 28}, vrcdraw::PointF{52, 44}},
            std::pair{vrcdraw::PointF{36, 36}, vrcdraw::PointF{58, 25}},
        };
        const auto path = WriteMaskBitmap(
            L"vrcdraw-reverse-ray-trap.bmp",
            72,
            64,
            [&](const int x, const int y) {
                const vrcdraw::PointF point{
                    static_cast<float>(x), static_cast<float>(y)};
                return std::hypot(point.x - 32.0F, point.y - 32.0F) <= 5.0F ||
                       std::ranges::any_of(segments, [&](const auto& segment) {
                           return vrcdraw::PointSegmentDistance(
                                      point, segment.first, segment.second) <= 1.5F;
                       });
            });
        vrcdraw::ImageProcessingDebug debug;
        const auto result = vrcdraw::ProcessImage(path, {}, &debug);
        assert(result.has_value());
        std::cout << "Stage2 reverse ray: junctions=" << debug.junctionCount
                  << ", ports=" << debug.junctionPortCount
                  << ", reverse_rejections=" << debug.reverseRayRejectionCount
                  << ", geodesics=" << debug.geodesicConnectorCount << std::endl;
        assert(debug.junctionCount == 1);
        assert(debug.junctionPortCount == 4);
        assert(debug.reverseRayRejectionCount >= 1);
        assert(debug.geodesicConnectorCount >= 1);
        assert(RouteOutsideInkRatio(
                   result->lineArt.strokes, result->lineArt.cleanLineArt) <= 0.01);
        std::filesystem::remove(path, error);
        assert(!error);
    }
}

void TestResolutionScaling()
{
    const auto sample = std::filesystem::path(VRC_DRAW_SOURCE_DIR) /
                        L"sample" / L"image(3).png";
    if (!std::filesystem::is_regular_file(sample)) {
        return;
    }

    const auto lowResolution = vrcdraw::ProcessImage(
        sample,
        vrcdraw::ImageProcessingOptions{
            .maximumDimension = 768,
            .maximumPixels = 2'400'000,
            .optimizeDrawingTime = true,
        });
    const auto highResolution = vrcdraw::ProcessImage(sample);
    assert(lowResolution.has_value());
    assert(highResolution.has_value());
    assert(lowResolution->width == 351);
    assert(lowResolution->height == 768);
    assert(highResolution->width == 688);
    assert(highResolution->height == 1504);

    const auto pointCount = [](const vrcdraw::ProcessedImage& image) {
        return std::accumulate(
            image.lineArt.strokes.strokes.begin(),
            image.lineArt.strokes.strokes.end(),
            std::size_t{},
            [](const std::size_t count, const vrcdraw::Stroke& stroke) {
                return count + stroke.size();
            });
    };
    const std::size_t lowPoints = pointCount(*lowResolution);
    const std::size_t highPoints = pointCount(*highResolution);
    const std::size_t lowInk = std::ranges::count(
        lowResolution->lineArt.cleanLineArt.pixels, std::uint8_t{1});
    const std::size_t highInk = std::ranges::count(
        highResolution->lineArt.cleanLineArt.pixels, std::uint8_t{1});
    std::cout << "Resolution image(3): low_points=" << lowPoints
              << ", high_points=" << highPoints
              << ", low_ink=" << lowInk
              << ", high_ink=" << highInk << std::endl;
    assert(highPoints > lowPoints);
    assert(highInk > lowInk * 2U);
}

void TestRepositorySamples()
{
    const auto samples = std::filesystem::path(VRC_DRAW_SOURCE_DIR) / L"sample";
    if (!std::filesystem::is_directory(samples)) {
        return;
    }

    std::vector<std::filesystem::path> samplePaths;
    for (const auto& entry : std::filesystem::directory_iterator(samples)) {
        if (entry.is_regular_file() && entry.path().extension() == L".png") {
            samplePaths.push_back(entry.path());
        }
    }
    std::ranges::sort(samplePaths);
    const char* qualityOutputEnvironment = std::getenv("VRC_DRAW_QUALITY_OUTPUT");
    const std::filesystem::path qualityOutput = qualityOutputEnvironment != nullptr
        ? std::filesystem::path(qualityOutputEnvironment)
        : std::filesystem::path{};
    if (!qualityOutput.empty()) {
        std::filesystem::create_directories(qualityOutput);
    }

    std::size_t tested = 0;
    for (const auto& samplePath : samplePaths) {
        vrcdraw::ImageProcessingDebug debug;
        const auto baselineProcessingStarted = std::chrono::steady_clock::now();
        const auto baseline = vrcdraw::ProcessImage(
            samplePath,
            vrcdraw::ImageProcessingOptions{
                .maximumDimension = 1536,
                .maximumPixels = 2'400'000,
                .optimizeDrawingTime = false,
                .generateVectorPath = false,
            },
            qualityOutput.empty() ? nullptr : &debug);
        const auto baselineProcessingDuration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - baselineProcessingStarted);
        const auto processingStarted = std::chrono::steady_clock::now();
        const auto result = vrcdraw::ProcessImage(
            samplePath,
            vrcdraw::ImageProcessingOptions{
                .generateVectorPath = true,
            });
        const auto processingDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - processingStarted);
        if (!baseline) {
            std::wcerr << L"Baseline processing failed for " << samplePath << L": "
                       << baseline.error() << L'\n';
        }
        if (!result) {
            std::wcerr << L"Sample processing failed for " << samplePath << L": "
                       << result.error() << L'\n';
        }
        assert(baseline.has_value());
        assert(result.has_value());
        assert(!baseline->lineArt.vectorPath.has_value());
        assert(result->lineArt.vectorPath.has_value());
        assert(baseline->lineArt.cleanLineArt.pixels ==
               result->lineArt.cleanLineArt.pixels);
        assert(vrcdraw::HaveIdenticalPathSegments(
            baseline->lineArt.strokes, result->lineArt.strokes));
        const auto baselinePlan = vrcdraw::BuildExecutionPlan(baseline->lineArt.strokes);
        const auto optimizedPlan = vrcdraw::BuildExecutionPlan(result->lineArt.strokes);
        assert(vrcdraw::HaveConsistentRouteMetadata(baseline->lineArt.strokes));
        assert(vrcdraw::HaveConsistentRouteMetadata(result->lineArt.strokes));
        auto withoutMetadata = result->lineArt.strokes;
        withoutMetadata.routeMetadata.clear();
        const auto withoutMetadataPlan = vrcdraw::BuildExecutionPlan(withoutMetadata);
        assert(HaveIdenticalCommands(optimizedPlan, withoutMetadataPlan));
        assert(result->lineArt.vectorPath.has_value());
        const auto vectorRoute = vrcdraw::FlattenVectorDrawingPath(
            *result->lineArt.vectorPath, 0.20F);
        assert(vectorRoute.strokes.size() == result->lineArt.strokes.strokes.size());
        assert(RouteOutsideInkRatio(vectorRoute, result->lineArt.cleanLineArt) <= 0.01);
        std::array<vrcdraw::ExecutionPlan, 3> vectorPlans;
        const std::array vectorScales{0.3F, 1.0F, 3.0F};
        for (std::size_t scaleIndex = 0; scaleIndex < vectorScales.size(); ++scaleIndex) {
            vrcdraw::ExecutionOptions vectorOptions{};
            vectorOptions.mouseScale = vectorScales[scaleIndex];
            vectorPlans[scaleIndex] = vrcdraw::BuildExecutionPlan(
                *result->lineArt.vectorPath,
                result->lineArt.cleanLineArt,
                vectorOptions);
            const auto& metrics = vectorPlans[scaleIndex].metrics;
            assert(metrics.acceptedFittedSpanCount + metrics.scaleFallbackSpanCount ==
                   metrics.fittedSpanCount);
            assert(vectorPlans[scaleIndex].strokeCount ==
                   result->lineArt.strokes.strokes.size());
        }
        const auto baselineRouteRaster = vrcdraw::RenderLineArt(
            baseline->lineArt.strokes, baseline->width, baseline->height);
        const auto optimizedRouteRaster = vrcdraw::RenderLineArt(
            result->lineArt.strokes, result->width, result->height);
        const auto baselineExecutionRaster = vrcdraw::RenderLineArt(
            PathFromExecutionPlan(baselinePlan), baseline->width, baseline->height);
        const auto optimizedExecutionRaster = vrcdraw::RenderLineArt(
            PathFromExecutionPlan(optimizedPlan), result->width, result->height);
        assert(baselineRouteRaster.bgra == optimizedRouteRaster.bgra);
        assert(baselineExecutionRaster.bgra == optimizedExecutionRaster.bgra);
        const auto baselineCoverage = PenDownCoverage(baselinePlan);
        const auto optimizedCoverage = PenDownCoverage(optimizedPlan);
        if (baselineCoverage != optimizedCoverage) {
            const std::size_t missing = std::ranges::count_if(
                baselineCoverage,
                [&](const std::uint64_t pixel) {
                    return !optimizedCoverage.contains(pixel);
                });
            const std::size_t added = std::ranges::count_if(
                optimizedCoverage,
                [&](const std::uint64_t pixel) {
                    return !baselineCoverage.contains(pixel);
                });
            std::cout << "Execution coverage mismatch "
                      << samplePath.filename().string()
                      << ": baseline=" << baselineCoverage.size()
                      << ", optimized=" << optimizedCoverage.size()
                      << ", missing=" << missing
                      << ", added=" << added
                      << ", moves=" << baselinePlan.metrics.penDownMoves
                      << "->" << optimizedPlan.metrics.penDownMoves
                      << ", command_segments_equal="
                      << (PenDownMoveSegments(baselinePlan) ==
                          PenDownMoveSegments(optimizedPlan)) << std::endl;
        }
        assert(baselineCoverage == optimizedCoverage);
        const auto& lineArt = result->lineArt.cleanLineArt;
        const auto& optimization = result->lineArt.strokes.optimization;
        const std::size_t inkPixels = std::ranges::count(lineArt.pixels, std::uint8_t{1});
        const double inkRatio = static_cast<double>(inkPixels) /
                                static_cast<double>(lineArt.pixels.size());
        const double outsideInkRatio = RouteOutsideInkRatio(
            result->lineArt.strokes,
            result->lineArt.cleanLineArt);
        const auto [representedComponents, totalComponents] = InkComponentCoverage(
            result->lineArt.strokes,
            result->lineArt.cleanLineArt);
        std::cout << "Sample " << samplePath.filename().string()
                  << ": lineart_ink=" << inkPixels
                  << ", ink_ratio=" << inkRatio
                  << ", strokes=" << optimization.strokesBefore << "->"
                  << result->lineArt.strokes.strokes.size()
                  << ", route_points="
                  << std::accumulate(
                         result->lineArt.strokes.strokes.begin(),
                         result->lineArt.strokes.strokes.end(),
                         std::size_t{},
                         [](const std::size_t count, const vrcdraw::Stroke& stroke) {
                             return count + stroke.size();
                         })
                  << ", outside_ink_ratio=" << outsideInkRatio
                  << ", components=" << representedComponents << '/' << totalComponents
                  << ", estimated_ms=" << optimization.estimatedMillisecondsBefore
                  << "->" << optimization.estimatedMillisecondsAfter
                  << ", vector_spans="
                  << result->lineArt.vectorPath->fitting.fittedSpans << '/'
                  << result->lineArt.vectorPath->fitting.cornerSplitSpans
                  << ", accepted_1x="
                  << vectorPlans[1].metrics.acceptedFittedSpanCount
                  << ", fallback_1x="
                  << vectorPlans[1].metrics.scaleFallbackSpanCount
                  << std::endl;
        assert(optimization.exactSegmentMatch);
        assert(optimization.exactExecutionMatch);
        assert(optimization.applied);
        assert(result->lineArt.strokes.sourceEdgeEnds.size() ==
               result->lineArt.strokes.strokes.size());
        for (std::size_t stroke = 0;
             stroke < result->lineArt.strokes.strokes.size();
             ++stroke) {
            if (!result->lineArt.strokes.sourceEdgeEnds[stroke].empty()) {
                assert(result->lineArt.strokes.sourceEdgeEnds[stroke].back() ==
                       result->lineArt.strokes.strokes[stroke].size() - 1);
            }
        }
        assert(optimization.strokesAfter == result->lineArt.strokes.strokes.size());
        assert(optimization.strokesAfter <= optimization.strokesBefore);
        assert(optimization.estimatedMillisecondsAfter <=
               optimization.estimatedMillisecondsBefore);
        if (outsideInkRatio > 0.01) {
            std::size_t reported = 0;
            for (std::size_t strokeIndex = 0;
                 strokeIndex < result->lineArt.strokes.strokes.size() && reported < 20;
                 ++strokeIndex) {
                const auto& stroke = result->lineArt.strokes.strokes[strokeIndex];
                vrcdraw::DrawingPath single{
                    .width = result->width,
                    .height = result->height,
                    .strokes = {stroke},
                };
                const double ratio = RouteOutsideInkRatio(single, lineArt);
                if (ratio > 0.01) {
                    std::cout << "  outside stroke=" << strokeIndex
                              << ", points=" << stroke.size()
                              << ", ratio=" << ratio
                              << ", from=" << stroke.front().x << ',' << stroke.front().y
                              << ", to=" << stroke.back().x << ',' << stroke.back().y
                              << std::endl;
                    ++reported;
                }
            }
        }
        assert(inkPixels > 0);
        assert(inkRatio < 0.35);
        assert(outsideInkRatio <= 0.01);
        assert(representedComponents == totalComponents);
        if (!qualityOutput.empty()) {
            const std::wstring prefix =
                std::to_wstring(tested + 1) + L"-" + samplePath.stem().wstring();
            const auto sampleOutput = qualityOutput / prefix;
            std::filesystem::create_directories(sampleOutput);
            WriteQualityBitmap(
                sampleOutput / L"ps-response.bmp",
                vrcdraw::RenderGrayscaleLineArt(debug.psResponse));
            WriteQualityBitmap(
                sampleOutput / L"line-confidence.bmp",
                vrcdraw::RenderGrayscaleLineArt(debug.confidence));
            WriteQualityBitmap(
                sampleOutput / L"line-tangent.bmp",
                vrcdraw::RenderGrayscaleLineArt(debug.tangent));
            WriteQualityBitmap(
                sampleOutput / L"line-scale-mask.bmp",
                vrcdraw::RenderGrayscaleLineArt(debug.scaleSupport));
            WriteQualityBitmap(
                sampleOutput / L"line-provenance.bmp",
                vrcdraw::RenderGrayscaleLineArt(debug.provenance));
            WriteQualityBitmap(
                sampleOutput / L"lineart.bmp",
                vrcdraw::RenderGrayscaleLineArt(result->lineArt.coverageLineArt));
            WriteQualityBitmap(
                sampleOutput / L"lineart-coverage.bmp",
                vrcdraw::RenderGrayscaleLineArt(result->lineArt.coverageLineArt));
            WriteQualityBitmap(
                sampleOutput / L"lineart-topology.bmp",
                vrcdraw::RenderBinaryLineArt(result->lineArt.cleanLineArt));
            WriteQualityBitmap(
                sampleOutput / L"region-types.bmp",
                vrcdraw::RenderGrayscaleLineArt(debug.regionTypes));
            WriteQualityBitmap(
                sampleOutput / L"junction-debug.bmp",
                vrcdraw::RenderGrayscaleLineArt(debug.junctionRegions));
            WriteQualityBitmap(
                sampleOutput / L"removed-spurs.bmp",
                vrcdraw::RenderGrayscaleLineArt(debug.removedSpurs));
            WriteQualityBitmap(
                sampleOutput / L"stage2-validated-route.bmp",
                vrcdraw::RenderLineArt(
                    debug.stage2UnoptimizedRoute, result->width, result->height));
            WriteQualityBitmap(
                sampleOutput / L"route-over-lineart.bmp",
                RouteOverLineArt(
                    result->lineArt.coverageLineArt, debug.stage2UnoptimizedRoute));
            WriteQualityBitmap(
                sampleOutput / L"baseline-route.bmp",
                baselineRouteRaster);
            WriteQualityBitmap(
                sampleOutput / L"optimized-route.bmp",
                optimizedRouteRaster);
            WriteQualityBitmap(
                sampleOutput / L"baseline-execution.bmp",
                baselineExecutionRaster);
            WriteQualityBitmap(
                sampleOutput / L"optimized-execution.bmp",
                optimizedExecutionRaster);
            WriteQualityBitmap(
                sampleOutput / L"vector-route.bmp",
                vrcdraw::RenderLineArt(vectorRoute, result->width, result->height));
            WriteQualityBitmap(
                sampleOutput / L"execution-route-0.30x.bmp",
                vrcdraw::RenderLineArt(
                    PathFromExecutionPlan(vectorPlans[0]), result->width, result->height));
            WriteQualityBitmap(
                sampleOutput / L"execution-route-1.00x.bmp",
                vrcdraw::RenderLineArt(
                    PathFromExecutionPlan(vectorPlans[1]), result->width, result->height));
            WriteQualityBitmap(
                sampleOutput / L"execution-route-3.00x.bmp",
                vrcdraw::RenderLineArt(
                    PathFromExecutionPlan(vectorPlans[2]), result->width, result->height));
            const auto exportStarted = std::chrono::steady_clock::now();
            const auto exportedLineArt = vrcdraw::ExportLineArt(*result, sampleOutput);
            const auto exportDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - exportStarted);
            if (!exportedLineArt) {
                std::wcerr << L"Quality export failed for " << samplePath << L": "
                           << exportedLineArt.error() << L'\n';
            }
            assert(exportedLineArt.has_value());
            std::ofstream metrics(sampleOutput / L"metrics.txt", std::ios::trunc);
            const std::size_t intermediateCoverage = std::ranges::count_if(
                result->lineArt.coverageLineArt.pixels,
                [](const std::uint8_t value) { return value > 0 && value < 255; });
            metrics << "topology_ink_pixels=" << inkPixels << '\n'
                    << "intermediate_coverage_pixels=" << intermediateCoverage << '\n'
                    << "baseline_processing_ms="
                    << baselineProcessingDuration.count() << '\n'
                    << "processing_ms=" << processingDuration.count() << '\n'
                    << "strokes_before=" << baselinePlan.strokeCount << '\n'
                    << "strokes_after=" << optimizedPlan.strokeCount << '\n'
                    << "duration_before_ms=" << baselinePlan.estimatedDuration.count() << '\n'
                    << "duration_after_ms=" << optimizedPlan.estimatedDuration.count() << '\n'
                    << "pen_down_moves_before=" << baselinePlan.metrics.penDownMoves << '\n'
                    << "pen_down_moves_after=" << optimizedPlan.metrics.penDownMoves << '\n'
                    << "pen_up_moves_before=" << baselinePlan.metrics.penUpMoves << '\n'
                    << "pen_up_moves_after=" << optimizedPlan.metrics.penUpMoves << '\n'
                    << "vector_source_spans="
                    << result->lineArt.vectorPath->fitting.sourceSpans << '\n'
                    << "vector_corner_split_spans="
                    << result->lineArt.vectorPath->fitting.cornerSplitSpans << '\n'
                    << "vector_fitted_spans="
                    << result->lineArt.vectorPath->fitting.fittedSpans << '\n'
                    << "vector_geometry_fallback_spans="
                    << result->lineArt.vectorPath->fitting.fallbackSpans << '\n'
                    << "vector_segments="
                    << result->lineArt.vectorPath->fitting.vectorSegments << '\n'
                    << "vector_source_polyline_segments="
                    << result->lineArt.vectorPath->fitting.sourcePolylineSegments << '\n'
                    << "vector_accepted_0_30x="
                    << vectorPlans[0].metrics.acceptedFittedSpanCount << '\n'
                    << "vector_scale_fallback_0_30x="
                    << vectorPlans[0].metrics.scaleFallbackSpanCount << '\n'
                    << "vector_final_samples_0_30x="
                    << vectorPlans[0].metrics.finalPenDownSamplePoints << '\n'
                    << "vector_accepted_1_00x="
                    << vectorPlans[1].metrics.acceptedFittedSpanCount << '\n'
                    << "vector_scale_fallback_1_00x="
                    << vectorPlans[1].metrics.scaleFallbackSpanCount << '\n'
                    << "vector_final_samples_1_00x="
                    << vectorPlans[1].metrics.finalPenDownSamplePoints << '\n'
                    << "vector_accepted_3_00x="
                    << vectorPlans[2].metrics.acceptedFittedSpanCount << '\n'
                    << "vector_scale_fallback_3_00x="
                    << vectorPlans[2].metrics.scaleFallbackSpanCount << '\n'
                    << "vector_final_samples_3_00x="
                    << vectorPlans[2].metrics.finalPenDownSamplePoints << '\n'
                    << "compact_regions=" << debug.compactRegionCount << '\n'
                    << "elongated_regions=" << debug.elongatedRegionCount << '\n'
                    << "junctions=" << debug.junctionCount << '\n'
                    << "junction_ports=" << debug.junctionPortCount << '\n'
                    << "removed_spurs=" << debug.removedSpurCount << '\n'
                    << "accepted_ray_intersections="
                    << debug.acceptedRayIntersectionCount << '\n'
                    << "reverse_ray_rejections=" << debug.reverseRayRejectionCount << '\n'
                    << "geodesic_connectors=" << debug.geodesicConnectorCount << '\n'
                    << "button_guard_before_ms="
                    << baselinePlan.metrics.buttonGuardWait.count() << '\n'
                    << "button_guard_after_ms="
                    << optimizedPlan.metrics.buttonGuardWait.count() << '\n'
                    << "lineart_export_ms=" << exportDuration.count() << '\n'
                    << "lineart_png_bytes="
                    << std::filesystem::file_size(exportedLineArt->pngPath) << '\n'
                    << "lineart_svg_bytes="
                    << std::filesystem::file_size(exportedLineArt->svgPath) << '\n'
                    << "logical_segment_difference=0\n"
                    << "execution_coverage_difference=0\n";
            assert(metrics.good());
        }
        ++tested;
    }
    assert(tested > 0);
}

} // namespace

int main()
{
    TestPathSimplification();
    TestMouseInterpolation();
    TestMouseInputModeHelpers();
    TestExecutionPlan();
    TestLosslessPathOptimizer();
    TestStageThreeVectorPath();
    TestSettings(std::filesystem::current_path());
    TestProgramIcon();
    TestImageProcessing();
    TestLineArtSeparation();
    TestJunctionPathQuality();
    TestFilledRegionPathQuality();
    TestStageTwoTopologyRegressions();
    TestResolutionScaling();
    TestRepositorySamples();
    std::cout << "All VRC-Draw core tests passed.\n";
    return 0;
}
