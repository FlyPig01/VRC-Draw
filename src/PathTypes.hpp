#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace vrcdraw {

struct PointF {
    float x{};
    float y{};

    friend bool operator==(const PointF&, const PointF&) = default;
};

using Stroke = std::vector<PointF>;

struct DrawingPath {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Stroke> strokes;
};

// 0 is background, 1 is a drawable black line pixel.
struct BinaryImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels;
};

struct LineArtDocument {
    BinaryImage cleanLineArt;
    DrawingPath strokes;
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
