#pragma once

#include <cstdint>
#include <filesystem>

namespace vrcdraw {

struct Settings {
    int futureStrokeLimit{300};
    float drawingScale{1.0F};
    float controlPanelWidth{320.0F};
    int drawingHotkeyVirtualKey{0x77};   // VK_F8
    std::uint32_t drawingHotkeyModifiers{};
    bool vectorPathEnabled{true};
};

[[nodiscard]] Settings LoadSettings(const std::filesystem::path& path);
[[nodiscard]] bool SaveSettings(const std::filesystem::path& path, const Settings& settings);

} // namespace vrcdraw
