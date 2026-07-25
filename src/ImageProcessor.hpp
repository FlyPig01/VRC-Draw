#pragma once

#include "PathTypes.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace vrcdraw {

[[nodiscard]] std::expected<ProcessedImage, std::wstring> ProcessImage(
    const std::filesystem::path& sourcePath);

} // namespace vrcdraw

