#include "Settings.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string>
#include <string_view>

namespace vrcdraw {
namespace {

constexpr std::uint32_t kSupportedModifierMask = 0x0FU;

void ParseIntegerSetting(
    const std::string& line,
    const std::string_view name,
    int& destination)
{
    const std::string prefix = std::string(name) + '=';
    if (!line.starts_with(prefix)) {
        return;
    }
    int value = destination;
    const auto first = line.data() + prefix.size();
    const auto last = line.data() + line.size();
    if (const auto result = std::from_chars(first, last, value); result.ec == std::errc{}) {
        destination = value;
    }
}

void ParseModifierSetting(
    const std::string& line,
    const std::string_view name,
    std::uint32_t& destination)
{
    int value = static_cast<int>(destination);
    ParseIntegerSetting(line, name, value);
    destination = static_cast<std::uint32_t>(std::max(0, value)) & kSupportedModifierMask;
}

bool IsValidVirtualKey(const int virtualKey)
{
    return virtualKey >= 0x08 && virtualKey <= 0xFE;
}

} // namespace

Settings LoadSettings(const std::filesystem::path& path)
{
    Settings settings{};
    std::ifstream input(path);
    if (!input) {
        return settings;
    }

    std::string line;
    while (std::getline(input, line)) {
        ParseIntegerSetting(line, "future_stroke_limit", settings.futureStrokeLimit);
        ParseIntegerSetting(line, "drawing_hotkey_vk", settings.drawingHotkeyVirtualKey);
        ParseModifierSetting(
            line, "drawing_hotkey_modifiers", settings.drawingHotkeyModifiers);
    }
    settings.futureStrokeLimit = std::clamp(settings.futureStrokeLimit, 20, 1000);
    if (!IsValidVirtualKey(settings.drawingHotkeyVirtualKey)) {
        settings.drawingHotkeyVirtualKey = 0x77;
        settings.drawingHotkeyModifiers = 0;
    }
    return settings;
}

bool SaveSettings(const std::filesystem::path& path, const Settings& settings)
{
    const auto temporary = path.parent_path() / L"settings.ini.tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            return false;
        }
        output << "[drawing]\n";
        output << "future_stroke_limit=" << std::clamp(settings.futureStrokeLimit, 20, 1000) << '\n';
        output << "drawing_hotkey_vk=" << settings.drawingHotkeyVirtualKey << '\n';
        output << "drawing_hotkey_modifiers="
               << (settings.drawingHotkeyModifiers & kSupportedModifierMask) << '\n';
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace vrcdraw
