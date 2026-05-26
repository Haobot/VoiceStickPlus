#pragma once

#include "app_config.h"
#include "glass_backdrop_window.h"

#include <Windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Gdiplus {
class Graphics;
}

namespace voicestick {

class OverlayWindow {
public:
    explicit OverlayWindow(HINSTANCE instance, HWND parent);
    ~OverlayWindow();

    void ShowListening();
    void ShowPartial(const std::string& text);
    void ShowFinalCountdown(const std::string& text, std::function<void()> on_complete);
    void ShowPausedFinal(const std::string& text);
    void ShowError(const std::string& text, std::function<void()> on_complete);
    void Hide(std::function<void()> on_hidden = {});
    void SetThemeColor(OverlayThemeColor color);
    void SetThemeSize(OverlayThemeSize size);
    void SetPosition(OverlayPosition position);

    HWND hwnd() const { return hwnd_; }
    void OnTimer(UINT_PTR timer_id);
    void OnPaint();
    void OnDpiChanged(UINT new_dpi, const RECT* suggested_rect);

private:
    enum class Mode { kListening, kCountdown, kPaused, kError, kHidden };

    void Show(Mode mode, const std::string& text, const std::string& hint = "");
    void Reposition();
    bool StepWindowAnimation();
    void ApplyAnimatedWindowBounds();
    POINT TargetWindowOrigin(const RECT& work_area, int width, int height) const;
    int VisualOffsetX(int width, int visual_width) const;
    int VisualOffsetY(int height, int visual_height) const;
    bool NeedsWindowAnimation() const;
    RECT BackdropBounds(int width, int height) const;
    void ResolveAutoThemeColor(const RECT& bounds);
    void SyncBackdrop(int width, int height, bool show);
    void ResizeBackdropWithoutRepaint(int width, int height);
    void UpdateLayeredBitmap();
    bool EnsureFrameBitmap(int width, int height);
    void ReleaseFrameBitmap();
    void InvalidateStaticLayer();
    void BuildStaticLayer(int width, int height);
    void RefreshDpi();
    int Dp(int px) const;
    float DpF(int px) const;
    int SizePx(int big_px, int medium_px, int small_px) const;
    float SizePxF(int big_px, int medium_px, int small_px) const;
    void PaintContent(Gdiplus::Graphics& graphics, int width, int height);
    void PaintText(void* bits, int width, int height);
    void PaintIndicator(Gdiplus::Graphics& graphics, int x, int y, int size);
    BYTE InkRgb() const;
    void StartFadeIn();
    void StartFadeOut();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<GlassBackdropWindow> backdrop_;
    RECT last_backdrop_bounds_{};
    bool backdrop_bounds_valid_ = false;
    Mode mode_ = Mode::kHidden;
    std::wstring text_;
    std::wstring hint_;
    std::function<void()> pending_callback_;
    int current_alpha_ = 0;
    int target_alpha_ = 0;
    int animation_frame_ = 0;
    int largest_visible_width_ = 0;
    int largest_visible_height_ = 0;
    int animated_window_width_ = 0;
    int animated_window_height_ = 0;
    int target_window_width_ = 0;
    int target_window_height_ = 0;
    int target_window_x_ = 0;
    int target_window_y_ = 0;
    ULONGLONG text_transition_started_at_ms_ = 0;
    float text_scroll_from_offset_ = 0.0f;
    float text_scroll_to_offset_ = 0.0f;
    float last_text_scroll_offset_ = 0.0f;
    OverlayThemeColor theme_color_ = OverlayThemeColor::kWhite;
    OverlayThemeColor resolved_theme_color_ = OverlayThemeColor::kWhite;
    OverlayThemeSize theme_size_ = OverlayThemeSize::kBig;
    OverlayPosition position_ = OverlayPosition::kCenter;
    ULONGLONG countdown_started_at_ms_ = 0;
    int countdown_duration_ms_ = 1200;
    UINT dpi_ = 96;
    bool should_wrap_text_ = false;
    HDC frame_dc_ = nullptr;
    HBITMAP frame_bitmap_ = nullptr;
    HGDIOBJ frame_old_bitmap_ = nullptr;
    void* frame_bits_ = nullptr;
    int frame_width_ = 0;
    int frame_height_ = 0;
    std::vector<BYTE> static_layer_bits_;
    bool static_layer_dirty_ = true;

    static constexpr UINT_PTR kAutoHideTimerId = 50;
    static constexpr UINT_PTR kFadeTimerId = 51;
    static constexpr UINT_PTR kAnimationTimerId = 52;
    static constexpr int kFadeStepMs = 16;
    static constexpr int kAnimationStepMs = 16;
    static constexpr int kTextTransitionMs = 140;
    static constexpr int kWindowWidthResizeStep = 40;
    static constexpr int kWindowHeightResizeStep = 18;
    static constexpr int kWindowResizeSnap = 6;
    static constexpr int kCornerRadius = 20;
    static constexpr int kMaxContentWidth = 620;
    static constexpr int kMinContentHeight = 84;
    static constexpr int kHorizontalPadding = 20;
    static constexpr int kVerticalPadding = 16;
    static constexpr int kIndicatorSize = 26;
    static constexpr int kContentSpacing = 0;
    static constexpr int kIndicatorCenterOffset = 8;
    static constexpr int kTextSafetySpacing = 0;
    static constexpr int kTextFontSize = 21;
    static constexpr float kTextLineHeightMultiplier = 1.5f;
    static constexpr float kTextBaselineMultiplier = 0.92f;
    static constexpr int kHintFontSize = 14;
    static constexpr BYTE kGlassScrimAlpha = 0;
    static constexpr BYTE kGlassBorderAlpha = 34;
    static constexpr BYTE kInkRgb = 246;
    static constexpr BYTE kTextAlpha = 248;
    static constexpr BYTE kTextShadowAlpha = 72;
    static constexpr BYTE kHintAlpha = 180;
    static constexpr BYTE kIndicatorAlpha = 218;
    static constexpr BYTE kIndicatorTrackAlpha = 72;
    static constexpr int kShadowPadding = 12;
    static constexpr int kShadowBlur = 0;
    static constexpr int kShadowYOffset = 0;
    static constexpr int kPositionMargin = 28;
    static constexpr int kMaxAlpha = 255;
};

} // namespace voicestick
