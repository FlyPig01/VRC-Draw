#pragma once

#include <cstdint>
#include <filesystem>

namespace vrcdraw {

struct Settings {
    int futureStrokeLimit{300};
    int drawingHotkeyVirtualKey{0x77};   // VK_F8
    std::uint32_t drawingHotkeyModifiers{};
};

[[nodiscard]] Settings LoadSettings(const std::filesystem::path& path);
[[nodiscard]] bool SaveSettings(const std::filesystem::path& path, const Settings& settings);

} // namespace vrcdraw
