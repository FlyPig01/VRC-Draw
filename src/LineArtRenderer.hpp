#pragma once

#include "PathTypes.hpp"

#include <cstdint>
#include <vector>

namespace vrcdraw {

struct RasterImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> bgra;
};

[[nodiscard]] RasterImage RenderLineArt(
    const DrawingPath& path,
    std::uint32_t outputWidth,
    std::uint32_t outputHeight);

[[nodiscard]] RasterImage RenderBinaryLineArt(const BinaryImage& lineArt);

[[nodiscard]] RasterImage RenderBinaryLineArt(
    const BinaryImage& lineArt,
    std::uint32_t outputWidth,
    std::uint32_t outputHeight);

} // namespace vrcdraw
