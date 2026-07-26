#include "LineArtRenderer.hpp"

#include "PathMath.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace vrcdraw {
namespace {

void BlendBlackPixel(
    RasterImage& image,
    const int x,
    const int y,
    const float coverage)
{
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) ||
        y >= static_cast<int>(image.height) || coverage <= 0.0F) {
        return;
    }
    const auto value = static_cast<std::uint8_t>(
        std::lround(255.0F * (1.0F - std::clamp(coverage, 0.0F, 1.0F))));
    const std::size_t offset =
        (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4;
    image.bgra[offset] = std::min(image.bgra[offset], value);
    image.bgra[offset + 1] = std::min(image.bgra[offset + 1], value);
    image.bgra[offset + 2] = std::min(image.bgra[offset + 2], value);
}

void DrawAntialiasedSegment(
    RasterImage& image,
    const PointF originalStart,
    const PointF originalEnd,
    const float lineWidth)
{
    const bool reverse = std::pair{originalEnd.x, originalEnd.y} <
                         std::pair{originalStart.x, originalStart.y};
    const PointF start = reverse ? originalEnd : originalStart;
    const PointF end = reverse ? originalStart : originalEnd;
    const float radius = std::max(0.5F, lineWidth * 0.5F);
    const int minimumX = static_cast<int>(std::floor(std::min(start.x, end.x) - radius - 1.0F));
    const int maximumX = static_cast<int>(std::ceil(std::max(start.x, end.x) + radius + 1.0F));
    const int minimumY = static_cast<int>(std::floor(std::min(start.y, end.y) - radius - 1.0F));
    const int maximumY = static_cast<int>(std::ceil(std::max(start.y, end.y) + radius + 1.0F));

    for (int y = minimumY; y <= maximumY; ++y) {
        for (int x = minimumX; x <= maximumX; ++x) {
            const float distance = PointSegmentDistance(
                PointF{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F},
                start,
                end);
            const float coverage = std::clamp(radius + 0.5F - distance, 0.0F, 1.0F);
            BlendBlackPixel(image, x, y, coverage);
        }
    }
}

} // namespace

RasterImage RenderLineArt(
    const DrawingPath& path,
    const std::uint32_t outputWidth,
    const std::uint32_t outputHeight)
{
    RasterImage image{
        .width = std::max(1U, outputWidth),
        .height = std::max(1U, outputHeight),
        .bgra = {},
    };
    image.bgra.assign(
        static_cast<std::size_t>(image.width) * image.height * 4,
        static_cast<std::uint8_t>(255));

    const float scaleX = static_cast<float>(image.width) /
                         static_cast<float>(std::max(1U, path.width));
    const float scaleY = static_cast<float>(image.height) /
                         static_cast<float>(std::max(1U, path.height));
    const float lineWidth = std::max(1.0F, std::min(scaleX, scaleY) * 1.15F);

    for (const Stroke& stroke : path.strokes) {
        for (std::size_t index = 1; index < stroke.size(); ++index) {
            const PointF start{
                stroke[index - 1].x * scaleX,
                stroke[index - 1].y * scaleY,
            };
            const PointF end{
                stroke[index].x * scaleX,
                stroke[index].y * scaleY,
            };
            DrawAntialiasedSegment(image, start, end, lineWidth);
        }
    }
    return image;
}

RasterImage RenderBinaryLineArt(const BinaryImage& lineArt)
{
    return RenderBinaryLineArt(lineArt, lineArt.width, lineArt.height);
}

RasterImage RenderBinaryLineArt(
    const BinaryImage& lineArt,
    const std::uint32_t outputWidth,
    const std::uint32_t outputHeight)
{
    RasterImage image{
        .width = std::max(1U, outputWidth),
        .height = std::max(1U, outputHeight),
        .bgra = {},
    };
    image.bgra.assign(
        static_cast<std::size_t>(image.width) * image.height * 4,
        static_cast<std::uint8_t>(255));
    if (lineArt.width == 0 || lineArt.height == 0 || lineArt.pixels.size() !=
        static_cast<std::size_t>(lineArt.width) * lineArt.height) {
        return image;
    }

    const auto sample = [&](const int x, const int y) {
        const int safeX = std::clamp(x, 0, static_cast<int>(lineArt.width) - 1);
        const int safeY = std::clamp(y, 0, static_cast<int>(lineArt.height) - 1);
        return static_cast<float>(lineArt.pixels[
            static_cast<std::size_t>(safeY) * lineArt.width +
            static_cast<std::size_t>(safeX)] != 0);
    };

    for (std::uint32_t y = 0; y < image.height; ++y) {
        const float sourceY =
            (static_cast<float>(y) + 0.5F) * static_cast<float>(lineArt.height) /
                static_cast<float>(image.height) -
            0.5F;
        const int y0 = static_cast<int>(std::floor(sourceY));
        const float fractionY = sourceY - static_cast<float>(y0);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float sourceX =
                (static_cast<float>(x) + 0.5F) * static_cast<float>(lineArt.width) /
                    static_cast<float>(image.width) -
                0.5F;
            const int x0 = static_cast<int>(std::floor(sourceX));
            const float fractionX = sourceX - static_cast<float>(x0);
            const float top = sample(x0, y0) * (1.0F - fractionX) +
                              sample(x0 + 1, y0) * fractionX;
            const float bottom = sample(x0, y0 + 1) * (1.0F - fractionX) +
                                 sample(x0 + 1, y0 + 1) * fractionX;
            const float coverage = top * (1.0F - fractionY) + bottom * fractionY;
            if (coverage <= 0.0F) {
                continue;
            }
            const auto value = static_cast<std::uint8_t>(std::lround(
                255.0F * (1.0F - std::clamp(coverage, 0.0F, 1.0F))));
            const std::size_t offset =
                (static_cast<std::size_t>(y) * image.width + x) * 4;
            image.bgra[offset] = value;
            image.bgra[offset + 1] = value;
            image.bgra[offset + 2] = value;
        }
    }
    return image;
}

RasterImage RenderGrayscaleLineArt(const GrayImage& lineArt)
{
    return RenderGrayscaleLineArt(lineArt, lineArt.width, lineArt.height);
}

RasterImage RenderGrayscaleLineArt(
    const GrayImage& lineArt,
    const std::uint32_t outputWidth,
    const std::uint32_t outputHeight)
{
    RasterImage image{
        .width = std::max(1U, outputWidth),
        .height = std::max(1U, outputHeight),
        .bgra = {},
    };
    image.bgra.assign(
        static_cast<std::size_t>(image.width) * image.height * 4,
        static_cast<std::uint8_t>(255));
    if (lineArt.width == 0 || lineArt.height == 0 || lineArt.pixels.size() !=
        static_cast<std::size_t>(lineArt.width) * lineArt.height) {
        return image;
    }

    const auto sample = [&](const int x, const int y) {
        const int safeX = std::clamp(x, 0, static_cast<int>(lineArt.width) - 1);
        const int safeY = std::clamp(y, 0, static_cast<int>(lineArt.height) - 1);
        return static_cast<float>(lineArt.pixels[
            static_cast<std::size_t>(safeY) * lineArt.width +
            static_cast<std::size_t>(safeX)]);
    };

    for (std::uint32_t y = 0; y < image.height; ++y) {
        const float sourceY =
            (static_cast<float>(y) + 0.5F) * static_cast<float>(lineArt.height) /
                static_cast<float>(image.height) -
            0.5F;
        const int y0 = static_cast<int>(std::floor(sourceY));
        const float fractionY = sourceY - static_cast<float>(y0);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float sourceX =
                (static_cast<float>(x) + 0.5F) * static_cast<float>(lineArt.width) /
                    static_cast<float>(image.width) -
                0.5F;
            const int x0 = static_cast<int>(std::floor(sourceX));
            const float fractionX = sourceX - static_cast<float>(x0);
            const float top = sample(x0, y0) * (1.0F - fractionX) +
                              sample(x0 + 1, y0) * fractionX;
            const float bottom = sample(x0, y0 + 1) * (1.0F - fractionX) +
                                 sample(x0 + 1, y0 + 1) * fractionX;
            const float coverage = top * (1.0F - fractionY) + bottom * fractionY;
            const auto value = static_cast<std::uint8_t>(std::lround(
                255.0F - std::clamp(coverage, 0.0F, 255.0F)));
            const std::size_t offset =
                (static_cast<std::size_t>(y) * image.width + x) * 4;
            image.bgra[offset] = value;
            image.bgra[offset + 1] = value;
            image.bgra[offset + 2] = value;
        }
    }
    return image;
}

} // namespace vrcdraw
