#pragma once

#include "PathTypes.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace vrcdraw {

struct LineArtExportResult {
    std::filesystem::path pngPath;
    std::filesystem::path svgPath;
};

[[nodiscard]] std::expected<LineArtExportResult, std::wstring> ExportLineArt(
    const ProcessedImage& image,
    const std::filesystem::path& exportDirectory);

} // namespace vrcdraw

