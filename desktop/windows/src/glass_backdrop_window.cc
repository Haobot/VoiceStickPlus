#include "glass_backdrop_window.h"

#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Gdiplus.lib")

namespace voicestick {

namespace {

constexpr DWORD kColorNone = 0xFFFFFFFE;
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaBorderColor = 34;
constexpr DWORD kDwmwaSystemBackdropType = 38;
constexpr int kDwmWindowCornerPreferenceRound = 2;
constexpr int kDwmSystemBackdropTypeTransientWindow = 3;
constexpr int kDwmSystemBackdropTypeAcrylic = 4;

enum AccentState {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};

struct AccentPolicy {
    int state = ACCENT_DISABLED;
    int flags = 0;
    int gradient_color = 0;
    int animation_id = 0;
};

struct WindowCompositionAttributeData {
    int attribute = 0;
    void* data = nullptr;
    SIZE_T size = 0;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

ULONG_PTR EnsureGdiplus() {
    static ULONG_PTR token = [] {
        ULONG_PTR startup_token = 0;
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&startup_token, &input, nullptr);
        return startup_token;
    }();
    return token;
}

void AddRoundedRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF rect, float radius) {
    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

int AccentColor(COLORREF tint, BYTE alpha) {
    const int red = GetRValue(tint);
    const int green = GetGValue(tint);
    const int blue = GetBValue(tint);
    return (static_cast<int>(alpha) << 24) |
           (blue << 16) |
           (green << 8) |
           red;
}

} // namespace

GlassBackdropWindow::GlassBackdropWindow(HINSTANCE instance, HWND owner)
    : instance_(instance), owner_(owner) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = GlassBackdropWindow::WndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"VoiceStickGlassBackdrop";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1, 1, owner_, nullptr, instance_, this);
    EnsureGdiplus();
    SetOpacity(0);
    ApplyBackdrop();
}

GlassBackdropWindow::~GlassBackdropWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void GlassBackdropWindow::Show(const RECT& bounds) {
    if (!hwnd_) return;
    const bool was_visible = visible_;
    visible_ = true;
    Move(bounds);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (!was_visible) StartFade(true);
}

void GlassBackdropWindow::Move(const RECT& bounds) {
    if (!hwnd_) return;
    if (bounds_valid_ && EqualRect(&last_bounds_, &bounds)) return;

    const int width = std::max(1L, bounds.right - bounds.left);
    const int height = std::max(1L, bounds.bottom - bounds.top);
    SetWindowPos(hwnd_, HWND_TOPMOST, bounds.left, bounds.top, width, height,
                 SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : 0));
    ApplyRegion(bounds);
    last_bounds_ = bounds;
    bounds_valid_ = true;
    if (visible_ && !glass_effect_enabled_) PaintFallback();
}

void GlassBackdropWindow::Hide(bool animated) {
    if (!hwnd_ || !visible_) return;
    visible_ = false;
    bounds_valid_ = false;
    if (animated) {
        StartFade(false);
    } else {
        KillTimer(hwnd_, kFadeTimerId);
        SetOpacity(0);
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void GlassBackdropWindow::SetCornerRadius(int radius_px) {
    corner_radius_px_ = std::max(1, radius_px);
    RECT bounds{};
    if (hwnd_ && GetWindowRect(hwnd_, &bounds)) ApplyRegion(bounds);
}

void GlassBackdropWindow::SetTheme(OverlayThemeColor color) {
    if (theme_color_ == color) return;
    theme_color_ = color;
    ApplyBackdrop();
    if (!glass_effect_enabled_) PaintFallback();
}

void GlassBackdropWindow::ApplyBackdrop() {
    if (!hwnd_) return;
    glass_effect_enabled_ = false;

    const int corner = kDwmWindowCornerPreferenceRound;
    DwmSetWindowAttribute(hwnd_, kDwmwaWindowCornerPreference, &corner, sizeof(corner));
    const DWORD border_color = kColorNone;
    DwmSetWindowAttribute(hwnd_, kDwmwaBorderColor, &border_color, sizeof(border_color));

    auto* user32 = GetModuleHandleW(L"user32.dll");
    auto* set_composition = user32
        ? reinterpret_cast<SetWindowCompositionAttributeFn>(
              GetProcAddress(user32, "SetWindowCompositionAttribute"))
        : nullptr;
    if (set_composition) {
        AccentPolicy accent{};
        accent.state = ACCENT_ENABLE_ACRYLICBLURBEHIND;
        accent.gradient_color = AccentColor(ThemeTint(), 156);
        WindowCompositionAttributeData data{};
        data.attribute = 19;
        data.data = &accent;
        data.size = sizeof(accent);
        if (set_composition(hwnd_, &data)) {
            glass_effect_enabled_ = true;
            return;
        }
    }

    const int acrylic_backdrop = kDwmSystemBackdropTypeAcrylic;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd_, kDwmwaSystemBackdropType,
                                        &acrylic_backdrop, sizeof(acrylic_backdrop)))) {
        glass_effect_enabled_ = true;
        return;
    }

    const int backdrop_type = kDwmSystemBackdropTypeTransientWindow;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd_, kDwmwaSystemBackdropType,
                                        &backdrop_type, sizeof(backdrop_type)))) {
        glass_effect_enabled_ = true;
        return;
    }

    if (set_composition) {
        AccentPolicy accent{};
        accent.state = ACCENT_ENABLE_BLURBEHIND;
        accent.gradient_color = AccentColor(ThemeTint(), 128);
        WindowCompositionAttributeData data{};
        data.attribute = 19;
        data.data = &accent;
        data.size = sizeof(accent);
        if (set_composition(hwnd_, &data)) glass_effect_enabled_ = true;
    }
}

void GlassBackdropWindow::ApplyRegion(const RECT& bounds) {
    const int width = std::max(1L, bounds.right - bounds.left);
    const int height = std::max(1L, bounds.bottom - bounds.top);
    const int diameter = std::max(1, corner_radius_px_ * 2);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    SetWindowRgn(hwnd_, region, TRUE);
}

void GlassBackdropWindow::SetOpacity(BYTE opacity) {
    current_opacity_ = opacity;
    if (hwnd_) SetLayeredWindowAttributes(hwnd_, 0, current_opacity_, LWA_ALPHA);
}

void GlassBackdropWindow::StartFade(bool show) {
    if (!hwnd_) return;
    KillTimer(hwnd_, kFadeTimerId);
    if (show) {
        if (current_opacity_ == 0) SetOpacity(1);
    }
    SetTimer(hwnd_, kFadeTimerId, kFadeStepMs, nullptr);
}

COLORREF GlassBackdropWindow::ThemeTint() const {
    switch (theme_color_) {
    case OverlayThemeColor::kPink:
        return RGB(255, 214, 230);
    case OverlayThemeColor::kGreen:
        return RGB(214, 242, 214);
    case OverlayThemeColor::kYellow:
        return RGB(255, 240, 184);
    case OverlayThemeColor::kBlue:
        return RGB(209, 232, 255);
    case OverlayThemeColor::kPurple:
        return RGB(230, 214, 255);
    case OverlayThemeColor::kWhite:
    default:
        return RGB(252, 252, 252);
    }
}

void GlassBackdropWindow::PaintFallback() {
    if (!hwnd_) return;
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) return;

    HDC dc = GetDC(hwnd_);
    if (!dc) return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetPageUnit(Gdiplus::UnitPixel);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const auto tint = ThemeTint();
    Gdiplus::RectF rect(0.5f, 0.5f,
                        static_cast<float>(client.right - client.left) - 1.0f,
                        static_cast<float>(client.bottom - client.top) - 1.0f);
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, rect, static_cast<float>(corner_radius_px_));
    Gdiplus::SolidBrush brush(Gdiplus::Color(176, GetRValue(tint), GetGValue(tint), GetBValue(tint)));
    graphics.FillPath(&brush, &path);
    Gdiplus::Pen border(Gdiplus::Color(64, 255, 255, 255), 1.0f);
    graphics.DrawPath(&border, &path);
    ReleaseDC(hwnd_, dc);
}

LRESULT CALLBACK GlassBackdropWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<GlassBackdropWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<GlassBackdropWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (!self->glass_effect_enabled_) self->PaintFallback();
        return 0;
    }
    case WM_TIMER:
        if (wp == kFadeTimerId) {
            if (self->visible_) {
                const int next = std::min<int>(kTargetOpacity, self->current_opacity_ + kFadeStep);
                self->SetOpacity(static_cast<BYTE>(next));
                if (next >= kTargetOpacity) KillTimer(hwnd, kFadeTimerId);
            } else {
                const int next = std::max<int>(0, self->current_opacity_ - kFadeStep);
                self->SetOpacity(static_cast<BYTE>(next));
                if (next == 0) {
                    KillTimer(hwnd, kFadeTimerId);
                    ShowWindow(hwnd, SW_HIDE);
                }
            }
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace voicestick
