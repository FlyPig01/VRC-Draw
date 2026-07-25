#include "AppPaths.hpp"
#include "DrawingEngine.hpp"
#include "ExecutionPlan.hpp"
#include "ImageProcessor.hpp"
#include "LineArtExporter.hpp"
#include "LineArtRenderer.hpp"
#include "OverlayWindow.hpp"
#include "Settings.hpp"
#include "resource.h"

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <shellapi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace {

constexpr int kDrawingToggleHotkeyId = 0x5102;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_deviceContext = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
ID3D11ShaderResourceView* g_originalTexture = nullptr;
ID3D11ShaderResourceView* g_lineArtTexture = nullptr;
std::optional<std::filesystem::path> g_droppedFile;
std::atomic<bool> g_drawingToggleRequested{false};
std::atomic<bool> g_hotkeyCaptureActive{false};
std::atomic<int> g_capturedHotkeyVirtualKey{0};
std::atomic<std::uint32_t> g_capturedHotkeyModifiers{0};

enum class HotkeyCaptureTarget {
    None,
    DrawingToggle,
};

bool CreateDeviceD3D(HWND window);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    return result;
}

bool IsModifierVirtualKey(const int virtualKey)
{
    return virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT ||
           virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL ||
           virtualKey == VK_RCONTROL || virtualKey == VK_MENU || virtualKey == VK_LMENU ||
           virtualKey == VK_RMENU || virtualKey == VK_LWIN || virtualKey == VK_RWIN;
}

std::string VirtualKeyName(const int virtualKey)
{
    switch (virtualKey) {
    case VK_PAUSE:
        return "Pause";
    case VK_ESCAPE:
        return "Esc";
    case VK_SPACE:
        return "Space";
    case VK_RETURN:
        return "Enter";
    case VK_TAB:
        return "Tab";
    case VK_BACK:
        return "Backspace";
    case VK_DELETE:
        return "Delete";
    case VK_INSERT:
        return "Insert";
    default:
        break;
    }

    const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
    LONG keyNameParameter = static_cast<LONG>(scanCode << 16U);
    if (virtualKey == VK_LEFT || virtualKey == VK_RIGHT || virtualKey == VK_UP ||
        virtualKey == VK_DOWN || virtualKey == VK_HOME || virtualKey == VK_END ||
        virtualKey == VK_PRIOR || virtualKey == VK_NEXT || virtualKey == VK_INSERT ||
        virtualKey == VK_DELETE) {
        keyNameParameter |= 1L << 24;
    }
    wchar_t name[64]{};
    if (GetKeyNameTextW(keyNameParameter, name, static_cast<int>(std::size(name))) > 0) {
        return WideToUtf8(name);
    }
    char fallback[16]{};
    sprintf_s(fallback, "VK 0x%02X", virtualKey);
    return fallback;
}

std::string HotkeyName(const int virtualKey, const std::uint32_t modifiers)
{
    std::string result;
    const auto append = [&](const char* name) {
        if (!result.empty()) {
            result += " + ";
        }
        result += name;
    };
    if ((modifiers & MOD_CONTROL) != 0) {
        append("Ctrl");
    }
    if ((modifiers & MOD_ALT) != 0) {
        append("Alt");
    }
    if ((modifiers & MOD_SHIFT) != 0) {
        append("Shift");
    }
    if ((modifiers & MOD_WIN) != 0) {
        append("Win");
    }
    const std::string key = VirtualKeyName(virtualKey);
    append(key.c_str());
    return result;
}

bool RegisterConfiguredHotkey(
    const HWND window,
    const int identifier,
    const int virtualKey,
    const std::uint32_t modifiers)
{
    return RegisterHotKey(
               window,
               identifier,
               (modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) | MOD_NOREPEAT,
               static_cast<UINT>(virtualKey)) != FALSE;
}

std::chrono::milliseconds RemainingDuration(
    const vrcdraw::ExecutionPlan& plan,
    const std::size_t completedCommands)
{
    std::chrono::milliseconds remaining{};
    for (std::size_t index = std::min(completedCommands, plan.commands.size());
         index < plan.commands.size();
         ++index) {
        if (plan.commands[index].type == vrcdraw::MouseCommandType::Wait) {
            remaining += plan.commands[index].duration;
        }
    }
    return remaining;
}

std::string FormatDuration(const std::chrono::milliseconds duration)
{
    const long long totalSeconds = std::max(0LL, (duration.count() + 999LL) / 1000LL);
    const long long hours = totalSeconds / 3600LL;
    const long long minutes = (totalSeconds / 60LL) % 60LL;
    const long long seconds = totalSeconds % 60LL;
    char buffer[32]{};
    if (hours > 0) {
        sprintf_s(buffer, "%lld:%02lld:%02lld", hours, minutes, seconds);
    } else {
        sprintf_s(buffer, "%02lld:%02lld", minutes, seconds);
    }
    return buffer;
}

std::optional<std::filesystem::path> ShowOpenImageDialog(HWND owner)
{
    std::wstring fileBuffer(32768, L'\0');
    constexpr wchar_t filter[] =
        L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0"
        L"所有文件 (*.*)\0*.*\0\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = fileBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER |
                   OFN_DONTADDTORECENT;
    dialog.lpstrDefExt = L"png";

    if (GetOpenFileNameW(&dialog) == FALSE) {
        return std::nullopt;
    }
    fileBuffer.resize(std::wcslen(fileBuffer.c_str()));
    return std::filesystem::path(fileBuffer);
}

void ApplyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 12.0F;
    style.ChildRounding = 10.0F;
    style.FrameRounding = 8.0F;
    style.PopupRounding = 10.0F;
    style.GrabRounding = 8.0F;
    style.ScrollbarRounding = 8.0F;
    style.WindowPadding = ImVec2(18.0F, 16.0F);
    style.FramePadding = ImVec2(12.0F, 8.0F);
    style.ItemSpacing = ImVec2(10.0F, 10.0F);

    auto& colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.961F, 0.969F, 0.984F, 1.0F);
    colors[ImGuiCol_ChildBg] = ImVec4(1.0F, 1.0F, 1.0F, 1.0F);
    colors[ImGuiCol_PopupBg] = ImVec4(1.0F, 1.0F, 1.0F, 1.0F);
    colors[ImGuiCol_Border] = ImVec4(0.863F, 0.890F, 0.933F, 1.0F);
    colors[ImGuiCol_FrameBg] = ImVec4(0.933F, 0.949F, 0.976F, 1.0F);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.894F, 0.925F, 1.0F, 1.0F);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.835F, 0.875F, 0.996F, 1.0F);
    colors[ImGuiCol_Button] = ImVec4(0.310F, 0.431F, 0.969F, 1.0F);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.388F, 0.494F, 0.980F, 1.0F);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.235F, 0.345F, 0.875F, 1.0F);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.310F, 0.431F, 0.969F, 1.0F);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.235F, 0.345F, 0.875F, 1.0F);
    colors[ImGuiCol_Header] = ImVec4(0.918F, 0.941F, 1.0F, 1.0F);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.855F, 0.898F, 1.0F, 1.0F);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.796F, 0.851F, 0.996F, 1.0F);
    colors[ImGuiCol_Text] = ImVec4(0.122F, 0.161F, 0.216F, 1.0F);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.392F, 0.455F, 0.545F, 1.0F);
    colors[ImGuiCol_Separator] = ImVec4(0.863F, 0.890F, 0.933F, 1.0F);
    colors[ImGuiCol_CheckMark] = ImVec4(0.310F, 0.431F, 0.969F, 1.0F);
}

void LoadChineseFont()
{
    ImGuiIO& io = ImGui::GetIO();
    wchar_t windowsDirectory[MAX_PATH]{};
    GetWindowsDirectoryW(windowsDirectory, static_cast<UINT>(std::size(windowsDirectory)));
    const auto fontPath = std::filesystem::path(windowsDirectory) / L"Fonts" / L"msyh.ttc";
    const std::string fontUtf8 = vrcdraw::PathToUtf8(fontPath);
    if (std::filesystem::exists(fontPath) &&
        io.Fonts->AddFontFromFileTTF(
            fontUtf8.c_str(), 19.0F, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon()) !=
            nullptr) {
        return;
    }
    io.Fonts->AddFontDefault();
}

bool CreateTexture(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::vector<std::uint8_t>& bgra,
    ID3D11ShaderResourceView*& output)
{
    if (output != nullptr) {
        output->Release();
        output = nullptr;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = bgra.data();
    initialData.SysMemPitch = width * 4U;

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(g_device->CreateTexture2D(&description, &initialData, &texture))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = description.Format;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MipLevels = 1;
    const HRESULT result =
        g_device->CreateShaderResourceView(texture, &viewDescription, &output);
    texture->Release();
    return SUCCEEDED(result);
}

const char* StateLabel(const vrcdraw::DrawingState state)
{
    switch (state) {
    case vrcdraw::DrawingState::Idle:
        return "等待图片";
    case vrcdraw::DrawingState::Ready:
        return "可以绘制";
    case vrcdraw::DrawingState::Drawing:
        return "正在绘制";
    case vrcdraw::DrawingState::Paused:
        return "已暂停";
    case vrcdraw::DrawingState::Stopped:
        return "已停止";
    case vrcdraw::DrawingState::Completed:
        return "绘制完成";
    case vrcdraw::DrawingState::Error:
        return "输入发送失败";
    }
    return "未知";
}

enum class PreviewMode {
    Original,
    LineArt,
    Route,
};

ImVec2 FitSize(
    const float width,
    const float height,
    const ImVec2 available)
{
    const float aspect = width / std::max(1.0F, height);
    ImVec2 result{available.x, available.x / aspect};
    if (result.y > available.y) {
        result.y = available.y;
        result.x = available.y * aspect;
    }
    return result;
}

void DrawEmptyPreview(const ImVec2 availableSize)
{
    const ImVec2 childSize{
        std::max(availableSize.x, 120.0F), std::max(availableSize.y, 180.0F)};
    ImGui::BeginChild("Preview", childSize, ImGuiChildFlags_Borders);
    const char* prompt = "将 PNG、JPEG 或 BMP 图片拖入此处\n\n或在左侧点击“打开图片”";
    const ImVec2 textSize = ImGui::CalcTextSize(prompt);
    const ImVec2 content = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(
        std::max(12.0F, (content.x - textSize.x) * 0.5F),
        std::max(12.0F, (content.y - textSize.y) * 0.5F)));
    ImGui::TextDisabled("%s", prompt);
    ImGui::EndChild();
}

void DrawTexturePreview(
    ID3D11ShaderResourceView* texture,
    const std::uint32_t width,
    const std::uint32_t height,
    const ImVec2 availableSize)
{
    const ImVec2 childSize{
        std::max(availableSize.x, 120.0F), std::max(availableSize.y, 180.0F)};
    ImGui::BeginChild("Preview", childSize, ImGuiChildFlags_Borders);
    const ImVec2 content = ImGui::GetContentRegionAvail();
    const ImVec2 drawSize = FitSize(
        static_cast<float>(width), static_cast<float>(height), content);
    const ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(
        cursor.x + std::max(0.0F, (content.x - drawSize.x) * 0.5F),
        cursor.y + std::max(0.0F, (content.y - drawSize.y) * 0.5F)));
    ImGui::Image(
        reinterpret_cast<ImTextureID>(texture),
        drawSize,
        ImVec2(0, 0),
        ImVec2(1, 1),
        ImVec4(1, 1, 1, 1),
        ImVec4(0, 0, 0, 0));
    ImGui::EndChild();
}

void DrawRoutePreview(
    const vrcdraw::ExecutionPlan& plan,
    const ImVec2 availableSize)
{
    const ImVec2 childSize{
        std::max(availableSize.x, 120.0F), std::max(availableSize.y, 180.0F)};
    ImGui::BeginChild("Preview", childSize, ImGuiChildFlags_Borders);
    const ImVec2 content = ImGui::GetContentRegionAvail();
    const ImVec2 drawSize = FitSize(
        static_cast<float>(plan.canvasWidth),
        static_cast<float>(plan.canvasHeight),
        content);
    const ImVec2 cursor = ImGui::GetCursorPos();
    const ImVec2 screenStart{
        ImGui::GetCursorScreenPos().x + std::max(0.0F, (content.x - drawSize.x) * 0.5F),
        ImGui::GetCursorScreenPos().y + std::max(0.0F, (content.y - drawSize.y) * 0.5F),
    };
    ImGui::SetCursorPos(ImVec2(
        cursor.x + std::max(0.0F, (content.x - drawSize.x) * 0.5F),
        cursor.y + std::max(0.0F, (content.y - drawSize.y) * 0.5F)));
    ImGui::InvisibleButton("RouteCanvas", drawSize);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        screenStart,
        ImVec2(screenStart.x + drawSize.x, screenStart.y + drawSize.y),
        IM_COL32(255, 255, 255, 255));
    drawList->AddRect(
        screenStart,
        ImVec2(screenStart.x + drawSize.x, screenStart.y + drawSize.y),
        IM_COL32(220, 227, 238, 255));
    drawList->PushClipRect(
        screenStart,
        ImVec2(screenStart.x + drawSize.x, screenStart.y + drawSize.y),
        true);

    const auto toScreen = [&](const vrcdraw::MousePoint point) {
        const float logicalX = static_cast<float>(point.x) / plan.mouseScale +
                               static_cast<float>(plan.canvasWidth) * 0.5F;
        const float logicalY = static_cast<float>(point.y) / plan.mouseScale +
                               static_cast<float>(plan.canvasHeight) * 0.5F;
        return ImVec2{
            screenStart.x + logicalX / static_cast<float>(plan.canvasWidth) * drawSize.x,
            screenStart.y + logicalY / static_cast<float>(plan.canvasHeight) * drawSize.y,
        };
    };

    vrcdraw::MousePoint position{};
    bool penDown = false;
    std::vector<ImVec2> penPolyline;
    for (const auto& command : plan.commands) {
        if (command.type == vrcdraw::MouseCommandType::LeftDown) {
            penDown = true;
            penPolyline.clear();
            penPolyline.push_back(toScreen(position));
        } else if (command.type == vrcdraw::MouseCommandType::LeftUp) {
            if (penPolyline.size() >= 2) {
                drawList->AddPolyline(
                    penPolyline.data(),
                    static_cast<int>(penPolyline.size()),
                    IM_COL32(37, 99, 235, 255),
                    0,
                    1.8F);
            }
            penPolyline.clear();
            penDown = false;
        } else if (command.type == vrcdraw::MouseCommandType::Move) {
            const vrcdraw::MousePoint next{position.x + command.dx, position.y + command.dy};
            if (penDown) {
                penPolyline.push_back(toScreen(next));
            }
            position = next;
        }
    }
    if (penPolyline.size() >= 2) {
        drawList->AddPolyline(
            penPolyline.data(),
            static_cast<int>(penPolyline.size()),
            IM_COL32(37, 99, 235, 255),
            0,
            1.8F);
    }

    drawList->PopClipRect();
    ImGui::EndChild();
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    const auto portablePaths = vrcdraw::AppPaths::CreatePortable();
    if (!portablePaths) {
        MessageBoxW(nullptr, portablePaths.error().c_str(), L"VRC-Draw 无法启动", MB_OK | MB_ICONERROR);
        return 1;
    }
    vrcdraw::Settings settings = vrcdraw::LoadSettings(portablePaths->settings);

    ImGui_ImplWin32_EnableDpiAwareness();
    const HICON appIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_VRC_DRAW_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    const HICON appSmallIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_VRC_DRAW_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    if (appIcon == nullptr || appSmallIcon == nullptr) {
        if (appIcon != nullptr) {
            DestroyIcon(appIcon);
        }
        if (appSmallIcon != nullptr) {
            DestroyIcon(appSmallIcon);
        }
        MessageBoxW(nullptr, L"无法加载程序图标资源。", L"VRC-Draw", MB_OK | MB_ICONERROR);
        return 1;
    }
    const WNDCLASSEXW windowClass{
        sizeof(WNDCLASSEXW),
        CS_CLASSDC,
        WindowProcedure,
        0,
        0,
        instance,
        appIcon,
        LoadCursorW(nullptr, IDC_ARROW),
        nullptr,
        nullptr,
        L"VRC-Draw.Window",
        appSmallIcon,
    };
    RegisterClassExW(&windowClass);

    const HWND window = CreateWindowW(
        windowClass.lpszClassName,
        L"VRC-Draw",
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        1280,
        800,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr || !CreateDeviceD3D(window)) {
        CleanupDeviceD3D();
        if (window != nullptr) {
            DestroyWindow(window);
        }
        UnregisterClassW(windowClass.lpszClassName, instance);
        DestroyIcon(appSmallIcon);
        DestroyIcon(appIcon);
        MessageBoxW(nullptr, L"无法创建 Direct3D 11 窗口。", L"VRC-Draw", MB_OK | MB_ICONERROR);
        return 1;
    }

    vrcdraw::OverlayWindow overlayWindow;
    if (!overlayWindow.Create(instance)) {
        CleanupDeviceD3D();
        DestroyWindow(window);
        UnregisterClassW(windowClass.lpszClassName, instance);
        DestroyIcon(appSmallIcon);
        DestroyIcon(appIcon);
        MessageBoxW(nullptr, L"无法创建悬浮窗。", L"VRC-Draw", MB_OK | MB_ICONERROR);
        return 1;
    }

    DragAcceptFiles(window, TRUE);
    bool drawingHotkeyRegistered = RegisterConfiguredHotkey(
        window,
        kDrawingToggleHotkeyId,
        settings.drawingHotkeyVirtualKey,
        settings.drawingHotkeyModifiers);
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const std::string imguiIniPath = vrcdraw::PathToUtf8(portablePaths->imguiIni);
    const std::string imguiLogPath = vrcdraw::PathToUtf8(portablePaths->imguiLog);
    io.IniFilename = imguiIniPath.c_str();
    io.LogFilename = imguiLogPath.c_str();
    LoadChineseFont();
    ApplyTheme();

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_deviceContext);

    vrcdraw::DrawingEngine drawingEngine;
    std::unique_ptr<vrcdraw::ProcessedImage> processedImage;
    std::unique_ptr<vrcdraw::ExecutionPlan> executionPlan;
    std::optional<std::future<std::expected<vrcdraw::ProcessedImage, std::wstring>>> processingTask;
    std::filesystem::path currentImagePath;
    std::string errorMessage;
    std::string successMessage;
    PreviewMode previewMode = PreviewMode::LineArt;
    std::uint32_t lineArtPreviewWidth = 0;
    std::uint32_t lineArtPreviewHeight = 0;
    bool done = false;
    bool drawingEnabled = false;
    HotkeyCaptureTarget hotkeyCaptureTarget = HotkeyCaptureTarget::None;

    const auto unregisterConfiguredHotkeys = [&] {
        if (drawingHotkeyRegistered) {
            UnregisterHotKey(window, kDrawingToggleHotkeyId);
            drawingHotkeyRegistered = false;
        }
    };
    const auto registerConfiguredHotkeys = [&] {
        unregisterConfiguredHotkeys();
        drawingHotkeyRegistered = RegisterConfiguredHotkey(
            window,
            kDrawingToggleHotkeyId,
            settings.drawingHotkeyVirtualKey,
            settings.drawingHotkeyModifiers);
        return drawingHotkeyRegistered;
    };
    const auto beginHotkeyCapture = [&](const HotkeyCaptureTarget target) {
        if (hotkeyCaptureTarget != HotkeyCaptureTarget::None) {
            return;
        }
        unregisterConfiguredHotkeys();
        hotkeyCaptureTarget = target;
        g_capturedHotkeyVirtualKey.store(0);
        g_capturedHotkeyModifiers.store(0);
        g_hotkeyCaptureActive.store(true);
        errorMessage.clear();
        successMessage.clear();
    };

    const auto beginProcessing = [&](const std::filesystem::path& path) {
        if (processingTask.has_value()) {
            return;
        }
        drawingEnabled = false;
        drawingEngine.SetRunRequested(false);
        drawingEngine.ClearPlan();
        errorMessage.clear();
        successMessage.clear();
        processedImage.reset();
        executionPlan.reset();
        if (g_originalTexture != nullptr) {
            g_originalTexture->Release();
            g_originalTexture = nullptr;
        }
        if (g_lineArtTexture != nullptr) {
            g_lineArtTexture->Release();
            g_lineArtTexture = nullptr;
        }
        currentImagePath = path;
        processingTask.emplace(std::async(std::launch::async, [path] {
            return vrcdraw::ProcessImage(path);
        }));
    };

    while (!done) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        const int capturedVirtualKey = g_capturedHotkeyVirtualKey.exchange(0);
        if (hotkeyCaptureTarget != HotkeyCaptureTarget::None && capturedVirtualKey != 0) {
            g_hotkeyCaptureActive.store(false);
            hotkeyCaptureTarget = HotkeyCaptureTarget::None;
            const vrcdraw::Settings previousSettings = settings;

            if (capturedVirtualKey != VK_ESCAPE) {
                const std::uint32_t capturedModifiers =
                    g_capturedHotkeyModifiers.exchange(0);
                settings.drawingHotkeyVirtualKey = capturedVirtualKey;
                settings.drawingHotkeyModifiers = capturedModifiers;
            }

            if (capturedVirtualKey == VK_ESCAPE) {
                registerConfiguredHotkeys();
                successMessage = "已取消快捷键修改。";
            } else if (!registerConfiguredHotkeys()) {
                settings = previousSettings;
                registerConfiguredHotkeys();
                errorMessage = "快捷键注册失败：该组合可能已被系统或其他程序占用。";
            } else if (!vrcdraw::SaveSettings(portablePaths->settings, settings)) {
                errorMessage = "快捷键已生效，但无法写入软件目录中的设置文件。";
            } else {
                successMessage = "快捷键已修改并保存。";
            }
        }

        if (g_droppedFile.has_value()) {
            beginProcessing(*g_droppedFile);
            g_droppedFile.reset();
        }
        if (processingTask.has_value() &&
            processingTask->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = processingTask->get();
            processingTask.reset();
            if (result) {
                processedImage = std::make_unique<vrcdraw::ProcessedImage>(std::move(*result));
                const auto lineArtPreview = vrcdraw::RenderBinaryLineArt(
                    processedImage->lineArt.cleanLineArt);
                lineArtPreviewWidth = lineArtPreview.width;
                lineArtPreviewHeight = lineArtPreview.height;
                const bool originalTextureCreated = CreateTexture(
                    processedImage->width,
                    processedImage->height,
                    processedImage->bgra,
                    g_originalTexture);
                const bool lineArtTextureCreated = CreateTexture(
                    lineArtPreview.width,
                    lineArtPreview.height,
                    lineArtPreview.bgra,
                    g_lineArtTexture);
                if (!originalTextureCreated || !lineArtTextureCreated) {
                    errorMessage = "无法创建图片预览纹理。";
                    processedImage.reset();
                    executionPlan.reset();
                } else {
                    executionPlan = std::make_unique<vrcdraw::ExecutionPlan>(
                        vrcdraw::BuildExecutionPlan(processedImage->lineArt.strokes));
                    drawingEngine.LoadPlan(*executionPlan);
                    previewMode = PreviewMode::LineArt;
                }
            } else {
                errorMessage = WideToUtf8(result.error());
                processedImage.reset();
            }
        }

        const bool canDraw = executionPlan != nullptr && !processingTask.has_value();
        if (g_drawingToggleRequested.exchange(false)) {
            if (canDraw) {
                drawingEnabled = !drawingEnabled;
                drawingEngine.SetRunRequested(drawingEnabled);
            } else {
                drawingEnabled = false;
                errorMessage = "请先导入图片并等待路径生成完成。";
            }
        }

        const auto drawingState = drawingEngine.State();
        if (drawingState == vrcdraw::DrawingState::Completed ||
            drawingState == vrcdraw::DrawingState::Stopped ||
            drawingState == vrcdraw::DrawingState::Error) {
            drawingEnabled = false;
        }

        vrcdraw::OverlayState overlayState{};
        if (executionPlan != nullptr) {
            const std::size_t completedCommands = drawingEngine.CompletedCommands();
            const std::size_t totalCommands = executionPlan->commands.size();
            overlayState.completedStrokes = drawingEngine.CompletedStrokes();
            overlayState.totalStrokes = drawingEngine.TotalStrokes();
            overlayState.progress = totalCommands == 0
                ? 0.0F
                : static_cast<float>(completedCommands) /
                      static_cast<float>(totalCommands);
            const std::string remaining =
                FormatDuration(RemainingDuration(*executionPlan, completedCommands));
            overlayState.remaining.assign(remaining.begin(), remaining.end());
        }
        overlayWindow.Update(overlayState);
        if (drawingEnabled) {
            overlayWindow.Show();
        } else {
            overlayWindow.Hide();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin(
            "VRC-Draw",
            nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings);

        ImGui::TextUnformatted("VRC-Draw");
        ImGui::SameLine();
        ImGui::TextDisabled("MVP 0.1");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0F);
        if (drawingEnabled) {
            ImGui::TextColored(ImVec4(0.086F, 0.639F, 0.290F, 1.0F), "绘制已开启");
        } else {
            ImGui::TextDisabled("绘制未开启");
        }
        ImGui::Separator();

        const ImVec2 workspaceSize = ImGui::GetContentRegionAvail();
        const float controlPanelWidth = std::clamp(workspaceSize.x * 0.23F, 220.0F, 280.0F);

        ImGui::BeginChild(
            "ControlPanel",
            ImVec2(controlPanelWidth, workspaceSize.y),
            ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("文件");
        if (ImGui::Button("打开图片", ImVec2(-1.0F, 0))) {
            if (const auto path = ShowOpenImageDialog(window)) {
                beginProcessing(*path);
            }
        }
        const float pairedButtonWidth =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
        ImGui::BeginDisabled(currentImagePath.empty() || processingTask.has_value());
        if (ImGui::Button("重新生成", ImVec2(pairedButtonWidth, 0))) {
            beginProcessing(currentImagePath);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(processedImage == nullptr);
        if (ImGui::Button("导出线稿", ImVec2(-1.0F, 0))) {
            errorMessage.clear();
            successMessage.clear();
            const auto exported = vrcdraw::ExportLineArt(*processedImage, portablePaths->exports);
            if (exported) {
                successMessage = "已导出 PNG 和 SVG：" +
                    vrcdraw::PathToUtf8(exported->pngPath.parent_path());
                previewMode = PreviewMode::LineArt;
            } else {
                errorMessage = WideToUtf8(exported.error());
            }
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextUnformatted("绘制控制");
        ImGui::TextWrapped("使用开始/暂停快捷键控制绘制；绘制期间会自动显示进度悬浮窗。");

        ImGui::Spacing();
        ImGui::TextUnformatted("笔画上限（预留功能）");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::SliderInt("##futureStrokeLimit", &settings.futureStrokeLimit, 20, 1000, "%d");
        ImGui::TextDisabled("当前数值暂不影响生成结果");

        ImGui::Separator();
        ImGui::TextUnformatted("快捷键");
        ImGui::TextDisabled("开始 / 暂停");
        const bool capturingDrawing =
            hotkeyCaptureTarget == HotkeyCaptureTarget::DrawingToggle;
        const std::string drawingHotkeyLabel = capturingDrawing
            ? "请按下新快捷键（Esc 取消）##DrawingHotkey"
            : HotkeyName(
                  settings.drawingHotkeyVirtualKey,
                  settings.drawingHotkeyModifiers) +
                  "##DrawingHotkey";
        ImGui::BeginDisabled(
            drawingEnabled || hotkeyCaptureTarget != HotkeyCaptureTarget::None);
        if (ImGui::Button(drawingHotkeyLabel.c_str(), ImVec2(-1.0F, 0)) &&
            !capturingDrawing) {
            beginHotkeyCapture(HotkeyCaptureTarget::DrawingToggle);
        }
        ImGui::EndDisabled();

        ImGui::TextWrapped(
            "切换到目标窗口后，按“%s”开始或暂停。",
            HotkeyName(
                settings.drawingHotkeyVirtualKey,
                settings.drawingHotkeyModifiers).c_str());
        if (drawingEnabled) {
            ImGui::TextDisabled("请先暂停绘制，再修改快捷键。");
        }

        if (hotkeyCaptureTarget == HotkeyCaptureTarget::None &&
            !drawingHotkeyRegistered) {
            ImGui::TextColored(
                ImVec4(0.863F, 0.149F, 0.149F, 1.0F),
                "快捷键注册失败，请重新绑定未被占用的组合。");
        }
        if (!errorMessage.empty()) {
            ImGui::TextColored(
                ImVec4(0.863F, 0.149F, 0.149F, 1.0F), "%s", errorMessage.c_str());
        }
        if (!successMessage.empty()) {
            ImGui::TextColored(
                ImVec4(0.086F, 0.639F, 0.290F, 1.0F), "%s", successMessage.c_str());
        }
        ImGui::Separator();
        ImGui::TextDisabled("开发模式：不检查当前前台程序");
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild(
            "PreviewPanel", ImVec2(0.0F, workspaceSize.y), ImGuiChildFlags_None);

        ImGui::BeginDisabled(processedImage == nullptr);
        if (ImGui::Selectable(
                "原图", previewMode == PreviewMode::Original, 0, ImVec2(76, 0))) {
            previewMode = PreviewMode::Original;
        }
        ImGui::SameLine();
        if (ImGui::Selectable(
                "线稿", previewMode == PreviewMode::LineArt, 0, ImVec2(76, 0))) {
            previewMode = PreviewMode::LineArt;
        }
        ImGui::SameLine();
        if (ImGui::Selectable(
                "绘画路线", previewMode == PreviewMode::Route, 0, ImVec2(106, 0))) {
            previewMode = PreviewMode::Route;
        }
        ImGui::EndDisabled();
        if (processedImage != nullptr) {
            ImGui::SameLine();
            ImGui::TextDisabled(
                "%s", vrcdraw::PathToUtf8(processedImage->sourcePath.filename()).c_str());
        }

        ImGui::BeginChild("DrawingInformation", ImVec2(0.0F, 88.0F), ImGuiChildFlags_Borders);
        if (executionPlan != nullptr) {
            const std::size_t completedCommands = drawingEngine.CompletedCommands();
            const std::size_t totalCommands = executionPlan->commands.size();
            const float progress = totalCommands == 0
                ? 0.0F
                : std::clamp(
                      static_cast<float>(completedCommands) /
                          static_cast<float>(totalCommands),
                      0.0F,
                      1.0F);
            const std::string remaining =
                FormatDuration(RemainingDuration(*executionPlan, completedCommands));
            ImGui::Text("状态：%s", StateLabel(drawingEngine.State()));
            ImGui::SameLine();
            ImGui::TextDisabled("总笔画：%zu", drawingEngine.TotalStrokes());
            ImGui::SameLine();
            ImGui::TextDisabled("预计剩余：%s", remaining.c_str());
            char progressLabel[96]{};
            sprintf_s(
                progressLabel,
                "绘制进度：%zu / %zu 笔",
                drawingEngine.CompletedStrokes(),
                drawingEngine.TotalStrokes());
            ImGui::ProgressBar(progress, ImVec2(-1.0F, 0.0F), progressLabel);
        } else if (processingTask.has_value()) {
            ImGui::TextUnformatted("正在处理图片……");
            ImGui::ProgressBar(0.0F, ImVec2(-1.0F, 0.0F), "正在生成线稿和路径");
        } else {
            ImGui::TextUnformatted("尚未导入图片");
            ImGui::TextDisabled("导入图片后，这里会显示绘制进度和预计剩余时间。");
        }
        ImGui::EndChild();

        const float routeNoteHeight =
            previewMode == PreviewMode::Route && executionPlan != nullptr
            ? ImGui::GetTextLineHeightWithSpacing()
            : 0.0F;
        const ImVec2 previewSize{
            ImGui::GetContentRegionAvail().x,
            std::max(180.0F, ImGui::GetContentRegionAvail().y - routeNoteHeight),
        };
        if (processedImage == nullptr) {
            DrawEmptyPreview(previewSize);
        } else if (previewMode == PreviewMode::Original) {
            DrawTexturePreview(
                g_originalTexture, processedImage->width, processedImage->height, previewSize);
        } else if (previewMode == PreviewMode::LineArt) {
            DrawTexturePreview(
                g_lineArtTexture, lineArtPreviewWidth, lineArtPreviewHeight, previewSize);
        } else if (executionPlan != nullptr) {
            DrawRoutePreview(*executionPlan, previewSize);
        }
        if (routeNoteHeight > 0.0F) {
            ImGui::TextColored(
                ImVec4(0.145F, 0.388F, 0.922F, 1.0F),
                "仅显示最终会落笔绘制的路线");
        }
        ImGui::EndChild();

        ImGui::End();
        ImGui::Render();

        constexpr float clearColor[4]{0.961F, 0.969F, 0.984F, 1.0F};
        g_deviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_deviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    drawingEngine.EmergencyStop();
    g_hotkeyCaptureActive.store(false);
    const bool settingsSaved = vrcdraw::SaveSettings(portablePaths->settings, settings);
    (void)settingsSaved;
    unregisterConfiguredHotkeys();

    if (g_originalTexture != nullptr) {
        g_originalTexture->Release();
        g_originalTexture = nullptr;
    }
    if (g_lineArtTexture != nullptr) {
        g_lineArtTexture->Release();
        g_lineArtTexture = nullptr;
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    overlayWindow.Destroy();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    DestroyIcon(appSmallIcon);
    DestroyIcon(appIcon);
    return 0;
}

namespace {

bool CreateDeviceD3D(const HWND window)
{
    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    swapChainDescription.BufferCount = 2;
    swapChainDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.OutputWindow = window;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.Windowed = TRUE;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel{};
    constexpr D3D_FEATURE_LEVEL featureLevels[]{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        static_cast<UINT>(std::size(featureLevels)),
        D3D11_SDK_VERSION,
        &swapChainDescription,
        &g_swapChain,
        &g_device,
        &featureLevel,
        &g_deviceContext);
    if (FAILED(result)) {
        return false;
    }
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_swapChain != nullptr) {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (g_deviceContext != nullptr) {
        g_deviceContext->Release();
        g_deviceContext = nullptr;
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_ID3D11Texture2D, reinterpret_cast<void**>(&backBuffer));
    if (backBuffer != nullptr) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
        backBuffer->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView != nullptr) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

LRESULT WINAPI WindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    if (g_hotkeyCaptureActive.load() &&
        (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
        const int virtualKey = static_cast<int>(wParam);
        if (IsModifierVirtualKey(virtualKey)) {
            return 0;
        }
        std::uint32_t modifiers = 0;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            modifiers |= MOD_CONTROL;
        }
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
            modifiers |= MOD_ALT;
        }
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
            modifiers |= MOD_SHIFT;
        }
        if ((GetKeyState(VK_LWIN) & 0x8000) != 0 ||
            (GetKeyState(VK_RWIN) & 0x8000) != 0) {
            modifiers |= MOD_WIN;
        }
        g_capturedHotkeyModifiers.store(modifiers);
        g_capturedHotkeyVirtualKey.store(virtualKey);
        return 0;
    }

    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
        return TRUE;
    }

    switch (message) {
    case WM_GETMINMAXINFO: {
        auto* minimum = reinterpret_cast<MINMAXINFO*>(lParam);
        minimum->ptMinTrackSize.x = 920;
        minimum->ptMinTrackSize.y = 640;
        return 0;
    }
    case WM_SIZE:
        if (g_device != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[32768]{};
        if (DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))) > 0) {
            g_droppedFile = std::filesystem::path(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_HOTKEY:
        if (wParam == kDrawingToggleHotkeyId) {
            g_drawingToggleRequested.store(true);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0U) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace
