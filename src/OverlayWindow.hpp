#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

namespace vrcdraw {

struct OverlayState {
    std::wstring remaining{L"--:--"};
    float progress{};
};

class OverlayWindow {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance);
    void Destroy();
    void Show();
    void Hide();
    void Update(const OverlayState& state);
    [[nodiscard]] bool IsVisible() const;

private:
    static LRESULT CALLBACK StaticWindowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WindowProcedure(UINT message, WPARAM wParam, LPARAM lParam);

    void Paint();
    void RecreateFont();
    void PlaceOnCurrentMonitor();
    [[nodiscard]] int Scale(int value) const;

    HINSTANCE instance_{};
    HWND window_{};
    HFONT font_{};
    UINT dpi_{96};
    bool classRegistered_{};
    OverlayState state_{};
};

} // namespace vrcdraw
