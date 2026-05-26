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
    void SetOpacity(BYTE opacity);
    void StartFade(bool show);
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
    BYTE current_opacity_ = 0;
    bool glass_effect_enabled_ = false;
    bool visible_ = false;

    static constexpr UINT_PTR kFadeTimerId = 70;
    static constexpr int kFadeStepMs = 16;
    static constexpr BYTE kTargetOpacity = 236;
    static constexpr BYTE kFadeStep = 28;
};

} // namespace voicestick
