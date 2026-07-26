#pragma once

#include "PathTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace vrcdraw {

struct ImageProcessingOptions {
    std::uint32_t maximumDimension{1536};
    std::uint64_t maximumPixels{2'400'000};
    bool optimizeDrawingTime{true};
    bool generateVectorPath{true};
};

// Optional, test-only stage-two diagnostics. The application passes nullptr, so
// these temporary evidence maps are never retained in a user's ProcessedImage.
struct ImageProcessingDebug {
    GrayImage psResponse;
    GrayImage confidence;
    GrayImage tangent;
    GrayImage scaleSupport;
    GrayImage provenance;
    GrayImage regionTypes;
    GrayImage junctionRegions;
    GrayImage removedSpurs;
    DrawingPath stage2UnoptimizedRoute;
    std::size_t compactRegionCount{};
    std::size_t elongatedRegionCount{};
    std::size_t junctionCount{};
    std::size_t junctionPortCount{};
    std::size_t removedSpurCount{};
    std::size_t acceptedRayIntersectionCount{};
    std::size_t reverseRayRejectionCount{};
    std::size_t geodesicConnectorCount{};
};

[[nodiscard]] std::expected<ProcessedImage, std::wstring> ProcessImage(
    const std::filesystem::path& sourcePath,
    const ImageProcessingOptions& options = {},
    ImageProcessingDebug* debug = nullptr);

} // namespace vrcdraw
