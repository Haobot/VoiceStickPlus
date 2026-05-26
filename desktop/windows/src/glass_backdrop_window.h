#pragma once

#include "app_config.h"

#include <Windows.h>

#include <cstdint>
#include <vector>

namespace voicestick {

class GlassBackdropWindow {
public:
    GlassBackdropWindow(HINSTANCE instance, HWND owner);
    ~GlassBackdropWindow();

    void Show(const RECT& bounds);
    void Move(const RECT& bounds);
    void Capture(const RECT& bounds);
    void ResizeWithoutRepaint(const RECT& bounds);
    void Hide(bool animated = true);
    void SetCornerRadius(int radius_px);
    void SetTheme(OverlayThemeColor color);
    bool HasGlassEffect() const { return glass_effect_enabled_; }
    bool CanShow() const { return glass_effect_enabled_; }
    HWND hwnd() const { return hwnd_; }

private:
    void ApplyBackdrop();
    void ApplyRegion(const RECT& bounds);
    void PaintCarrierSurface();
    void PaintCachedSurface(const RECT& bounds);
    void ApplyFrostedMaterial(std::vector<std::uint32_t>& pixels) const;
    int BlurRadius() const;
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
    std::vector<std::uint32_t> cached_pixels_;
    int cached_width_ = 0;
    int cached_height_ = 0;
    int cached_left_ = 0;
    int cached_top_ = 0;

    static constexpr DWORD kFadeDurationMs = 140;
};

} // namespace voicestick
