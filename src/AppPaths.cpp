#include "AppPaths.hpp"

#include <windows.h>

#include <fstream>
#include <system_error>
#include <vector>

namespace vrcdraw {
namespace {

std::expected<std::filesystem::path, std::wstring> ExecutableDirectory()
{
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::unexpected(L"无法取得程序所在目录。");
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace

std::expected<AppPaths, std::wstring> AppPaths::CreatePortable()
{
    const auto executableDirectory = ExecutableDirectory();
    if (!executableDirectory) {
        return std::unexpected(executableDirectory.error());
    }

    AppPaths paths{};
    paths.root = *executableDirectory;
    paths.data = paths.root / L"data";
    paths.settings = paths.data / L"settings.ini";
    paths.imguiIni = paths.data / L"imgui.ini";
    paths.logs = paths.data / L"logs";
    paths.imguiLog = paths.logs / L"imgui.log";
    paths.temp = paths.data / L"temp";
    paths.exports = paths.root / L"exports";

    std::error_code error;
    std::filesystem::create_directories(paths.logs, error);
    if (error) {
        return std::unexpected(L"无法在软件目录中创建 data\\logs。请将软件放到具有写权限的目录。");
    }
    std::filesystem::create_directories(paths.temp, error);
    if (error) {
        return std::unexpected(L"无法在软件目录中创建 data\\temp。请将软件放到具有写权限的目录。");
    }
    std::filesystem::create_directories(paths.exports, error);
    if (error) {
        return std::unexpected(L"无法在软件目录中创建 exports。请将软件放到具有写权限的目录。");
    }

    const auto writeProbe = paths.temp / L".write-test";
    {
        std::ofstream output(writeProbe, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(L"软件目录不可写。程序不会回退到 AppData 或注册表。");
        }
        output << "ok";
    }
    std::filesystem::remove(writeProbe, error);

    return paths;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), required, nullptr, nullptr);
    return utf8;
}

} // namespace vrcdraw
