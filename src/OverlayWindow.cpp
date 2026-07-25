#include "OverlayWindow.hpp"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <utility>

namespace vrcdraw {
namespace {

constexpr wchar_t kOverlayWindowClass[] = L"VRC-Draw.ProgressOverlay";
constexpr int kBaseWidth = 360;
constexpr int kBaseHeight = 92;
constexpr BYTE kWindowOpacity = 190;

UINT WindowDpi(const HWND window)
{
    const HDC deviceContext = GetDC(window);
    const int dpi = deviceContext == nullptr ? 96 : GetDeviceCaps(deviceContext, LOGPIXELSX);
    if (deviceContext != nullptr) {
        ReleaseDC(window, deviceContext);
    }
    return static_cast<UINT>(std::max(96, dpi));
}

void FillRoundedRectangle(
    HDC deviceContext,
    const RECT rectangle,
    const int radius,
    const COLORREF color)
{
    const HBRUSH brush = CreateSolidBrush(color);
    const HPEN pen = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ oldBrush = SelectObject(deviceContext, brush);
    const HGDIOBJ oldPen = SelectObject(deviceContext, pen);
    RoundRect(
        deviceContext,
        rectangle.left,
        rectangle.top,
        rectangle.right,
        rectangle.bottom,
        radius,
        radius);
    SelectObject(deviceContext, oldPen);
    SelectObject(deviceContext, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

} // namespace

OverlayWindow::~OverlayWindow()
{
    Destroy();
}

bool OverlayWindow::Create(const HINSTANCE instance)
{
    if (window_ != nullptr) {
        return true;
    }

    instance_ = instance;
    const WNDCLASSEXW windowClass{
        sizeof(WNDCLASSEXW),
        CS_HREDRAW | CS_VREDRAW,
        StaticWindowProcedure,
        0,
        0,
        instance_,
        nullptr,
        LoadCursorW(nullptr, IDC_ARROW),
        nullptr,
        nullptr,
        kOverlayWindowClass,
        nullptr,
    };
    classRegistered_ = RegisterClassExW(&windowClass) != 0;
    if (!classRegistered_ && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TRANSPARENT,
        kOverlayWindowClass,
        L"",
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kBaseWidth,
        kBaseHeight,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) {
        if (classRegistered_) {
            UnregisterClassW(kOverlayWindowClass, instance_);
            classRegistered_ = false;
        }
        return false;
    }

    dpi_ = WindowDpi(window_);
    RecreateFont();
    SetWindowPos(
        window_,
        HWND_TOPMOST,
        0,
        0,
        Scale(kBaseWidth),
        Scale(kBaseHeight),
        SWP_NOMOVE | SWP_NOACTIVATE);
    SetLayeredWindowAttributes(window_, 0, kWindowOpacity, LWA_ALPHA);

    constexpr DWORD kWindowCornerPreference = 33;
    constexpr int kRoundCorners = 2;
    DwmSetWindowAttribute(
        window_,
        static_cast<DWMWINDOWATTRIBUTE>(kWindowCornerPreference),
        &kRoundCorners,
        sizeof(kRoundCorners));
    return true;
}

void OverlayWindow::Destroy()
{
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (classRegistered_) {
        UnregisterClassW(kOverlayWindowClass, instance_);
        classRegistered_ = false;
    }
}

void OverlayWindow::Show()
{
    if (window_ == nullptr || IsVisible()) {
        return;
    }
    PlaceOnCurrentMonitor();
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    SetWindowPos(
        window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
}

void OverlayWindow::Hide()
{
    if (window_ != nullptr && IsVisible()) {
        ShowWindow(window_, SW_HIDE);
    }
}

void OverlayWindow::Update(const OverlayState& state)
{
    OverlayState normalized = state;
    normalized.progress = std::clamp(normalized.progress, 0.0F, 1.0F);
    const bool changed = state_.remaining != normalized.remaining ||
                         state_.completedStrokes != normalized.completedStrokes ||
                         state_.totalStrokes != normalized.totalStrokes ||
                         state_.progress != normalized.progress;
    if (!changed) {
        return;
    }
    state_ = std::move(normalized);
    if (IsVisible()) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

bool OverlayWindow::IsVisible() const
{
    return window_ != nullptr && IsWindowVisible(window_) != FALSE;
}

LRESULT CALLBACK OverlayWindow::StaticWindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    OverlayWindow* overlay = reinterpret_cast<OverlayWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        overlay = static_cast<OverlayWindow*>(create->lpCreateParams);
        overlay->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    }
    if (overlay == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return overlay->WindowProcedure(message, wParam, lParam);
}

LRESULT OverlayWindow::WindowProcedure(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        RecreateFont();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(
            window_,
            HWND_TOPMOST,
            suggested->left,
            suggested->top,
            Scale(kBaseWidth),
            Scale(kBaseHeight),
            SWP_NOACTIVATE);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        Hide();
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        window_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

void OverlayWindow::Paint()
{
    PAINTSTRUCT paint{};
    const HDC target = BeginPaint(window_, &paint);
    RECT client{};
    GetClientRect(window_, &client);

    const HDC buffer = CreateCompatibleDC(target);
    const HBITMAP bitmap =
        CreateCompatibleBitmap(target, client.right - client.left, client.bottom - client.top);
    const HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
    SetBkMode(buffer, TRANSPARENT);

    const HBRUSH background = CreateSolidBrush(RGB(12, 12, 14));
    FillRect(buffer, &client, background);
    DeleteObject(background);

    SelectObject(buffer, font_);
    SetTextColor(buffer, RGB(255, 255, 255));
    wchar_t progressText[96]{};
    std::swprintf(
        progressText,
        std::size(progressText),
        L"绘制进度：%zu / %zu 笔",
        state_.completedStrokes,
        state_.totalStrokes);
    RECT progressLabel{Scale(16), Scale(12), client.right / 2 + Scale(44), Scale(42)};
    DrawTextW(
        buffer,
        progressText,
        -1,
        &progressLabel,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const std::wstring remainingText = L"剩余：" + state_.remaining;
    RECT remainingLabel{
        client.right / 2, Scale(12), client.right - Scale(16), Scale(42)};
    DrawTextW(
        buffer,
        remainingText.c_str(),
        -1,
        &remainingLabel,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT progressBackground{
        Scale(16), Scale(56), client.right - Scale(16), Scale(72)};
    FillRoundedRectangle(buffer, progressBackground, Scale(10), RGB(72, 72, 76));
    RECT progressFill = progressBackground;
    progressFill.right = progressFill.left + static_cast<LONG>(std::lround(
        static_cast<float>(progressBackground.right - progressBackground.left) *
        state_.progress));
    if (progressFill.right > progressFill.left) {
        FillRoundedRectangle(buffer, progressFill, Scale(10), RGB(255, 255, 255));
    }

    BitBlt(
        target,
        0,
        0,
        client.right - client.left,
        client.bottom - client.top,
        buffer,
        0,
        0,
        SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(window_, &paint);
}

void OverlayWindow::RecreateFont()
{
    if (font_ != nullptr) {
        DeleteObject(font_);
    }
    font_ = CreateFontW(
        -Scale(15),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH,
        L"Microsoft YaHei UI");
}

void OverlayWindow::PlaceOnCurrentMonitor()
{
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO information{};
    information.cbSize = sizeof(MONITORINFO);
    GetMonitorInfoW(monitor, &information);
    const int width = Scale(kBaseWidth);
    const int height = Scale(kBaseHeight);
    SetWindowPos(
        window_,
        HWND_TOPMOST,
        information.rcWork.right - width - Scale(20),
        information.rcWork.top + Scale(20),
        width,
        height,
        SWP_NOACTIVATE);
}

int OverlayWindow::Scale(const int value) const
{
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

} // namespace vrcdraw
