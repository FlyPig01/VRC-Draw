#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace vrcdraw {

struct AppPaths {
    std::filesystem::path root;
    std::filesystem::path data;
    std::filesystem::path settings;
    std::filesystem::path imguiIni;
    std::filesystem::path imguiLog;
    std::filesystem::path logs;
    std::filesystem::path temp;
    std::filesystem::path exports;

    [[nodiscard]] static std::expected<AppPaths, std::wstring> CreatePortable();
};

[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);

} // namespace vrcdraw
