#pragma once

#include "app_config.h"

#include <Windows.h>

namespace voicestick {

class GlassBackdropWindow {
public:
    GlassBackdropWindow(HINSTANCE instance, HWND owner);
    ~GlassBackdropWindow();

    void Show(const RECT& bounds);
    void Move(const RECT& bounds);
    void Hide(bool animated = true);
    void SetCornerRadius(int radius_px);
    void SetTheme(OverlayThemeColor color);
    bool HasGlassEffect() const { return glass_effect_enabled_; }
    HWND hwnd() const { return hwnd_; }

private:
    void ApplyBackdrop();
    void ApplyRegion(const RECT& bounds);
    COLORREF ThemeTint() const;
    void PaintFallback();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    RECT last_bounds_{};
    bool bounds_valid_ = false;
    int corner_radius_px_ = 24;
    OverlayThemeColor theme_color_ = OverlayThemeColor::kWhite;
    bool glass_effect_enabled_ = false;
    bool visible_ = false;

    static constexpr DWORD kFadeDurationMs = 140;
};

} // namespace voicestick
