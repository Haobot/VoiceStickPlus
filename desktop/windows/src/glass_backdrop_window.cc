#include "glass_backdrop_window.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace voicestick {

namespace {

constexpr int kBlurRadius = 14;
constexpr BYTE kSurfaceAlpha = 236;

int ClampByte(int value) {
    return std::clamp(value, 0, 255);
}

BYTE RoundedCoverage(int x, int y, int width, int height, int radius) {
    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float left = 0.5f;
    const float top = 0.5f;
    const float right = static_cast<float>(width) - 0.5f;
    const float bottom = static_cast<float>(height) - 0.5f;
    const float r = static_cast<float>(std::clamp(radius, 1, std::max(1, std::min(width, height) / 2)));

    const float dx = std::max({left + r - px, 0.0f, px - (right - r)});
    const float dy = std::max({top + r - py, 0.0f, py - (bottom - r)});
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float coverage = std::clamp(r + 0.5f - distance, 0.0f, 1.0f);
    return static_cast<BYTE>(coverage * static_cast<float>(kSurfaceAlpha));
}

void BoxBlur(std::vector<std::uint32_t>& pixels, int width, int height, int radius) {
    if (pixels.empty() || width <= 0 || height <= 0 || radius <= 0) return;

    std::vector<std::uint32_t> temp(pixels.size());
    const int diameter = radius * 2 + 1;

    for (int y = 0; y < height; ++y) {
        int red = 0;
        int green = 0;
        int blue = 0;
        for (int i = -radius; i <= radius; ++i) {
            const int x = std::clamp(i, 0, width - 1);
            const std::uint32_t pixel = pixels[static_cast<std::size_t>(y) * width + x];
            blue += pixel & 0xff;
            green += (pixel >> 8) & 0xff;
            red += (pixel >> 16) & 0xff;
        }
        for (int x = 0; x < width; ++x) {
            temp[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint32_t>(blue / diameter) |
                (static_cast<std::uint32_t>(green / diameter) << 8) |
                (static_cast<std::uint32_t>(red / diameter) << 16);

            const int remove_x = std::clamp(x - radius, 0, width - 1);
            const int add_x = std::clamp(x + radius + 1, 0, width - 1);
            const std::uint32_t remove_pixel = pixels[static_cast<std::size_t>(y) * width + remove_x];
            const std::uint32_t add_pixel = pixels[static_cast<std::size_t>(y) * width + add_x];
            blue += static_cast<int>(add_pixel & 0xff) - static_cast<int>(remove_pixel & 0xff);
            green += static_cast<int>((add_pixel >> 8) & 0xff) - static_cast<int>((remove_pixel >> 8) & 0xff);
            red += static_cast<int>((add_pixel >> 16) & 0xff) - static_cast<int>((remove_pixel >> 16) & 0xff);
        }
    }

    for (int x = 0; x < width; ++x) {
        int red = 0;
        int green = 0;
        int blue = 0;
        for (int i = -radius; i <= radius; ++i) {
            const int y = std::clamp(i, 0, height - 1);
            const std::uint32_t pixel = temp[static_cast<std::size_t>(y) * width + x];
            blue += pixel & 0xff;
            green += (pixel >> 8) & 0xff;
            red += (pixel >> 16) & 0xff;
        }
        for (int y = 0; y < height; ++y) {
            pixels[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint32_t>(blue / diameter) |
                (static_cast<std::uint32_t>(green / diameter) << 8) |
                (static_cast<std::uint32_t>(red / diameter) << 16);

            const int remove_y = std::clamp(y - radius, 0, height - 1);
            const int add_y = std::clamp(y + radius + 1, 0, height - 1);
            const std::uint32_t remove_pixel = temp[static_cast<std::size_t>(remove_y) * width + x];
            const std::uint32_t add_pixel = temp[static_cast<std::size_t>(add_y) * width + x];
            blue += static_cast<int>(add_pixel & 0xff) - static_cast<int>(remove_pixel & 0xff);
            green += static_cast<int>((add_pixel >> 8) & 0xff) - static_cast<int>((remove_pixel >> 8) & 0xff);
            red += static_cast<int>((add_pixel >> 16) & 0xff) - static_cast<int>((remove_pixel >> 16) & 0xff);
        }
    }
}

void ApplyRoundedAlpha(std::vector<std::uint32_t>& pixels, int width, int height, int radius) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const std::uint32_t pixel = pixels[index];
            const int alpha = RoundedCoverage(x, y, width, height, radius);
            const int blue = ClampByte(static_cast<int>(pixel & 0xff) * alpha / 255);
            const int green = ClampByte(static_cast<int>((pixel >> 8) & 0xff) * alpha / 255);
            const int red = ClampByte(static_cast<int>((pixel >> 16) & 0xff) * alpha / 255);
            pixels[index] = static_cast<std::uint32_t>(blue) |
                            (static_cast<std::uint32_t>(green) << 8) |
                            (static_cast<std::uint32_t>(red) << 16) |
                            (static_cast<std::uint32_t>(alpha) << 24);
        }
    }
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
    ApplyBackdrop();
}

GlassBackdropWindow::~GlassBackdropWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void GlassBackdropWindow::Show(const RECT& bounds) {
    if (!hwnd_ || !CanShow()) return;
    const bool was_visible = visible_;
    visible_ = true;
    Move(bounds);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void GlassBackdropWindow::Move(const RECT& bounds) {
    if (!hwnd_ || !CanShow()) return;
    if (bounds_valid_ && EqualRect(&last_bounds_, &bounds)) return;

    const int width = std::max(1L, bounds.right - bounds.left);
    const int height = std::max(1L, bounds.bottom - bounds.top);
    if (visible_) ShowWindow(hwnd_, SW_HIDE);
    SetWindowPos(hwnd_, HWND_TOPMOST, bounds.left, bounds.top, width, height,
                 SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOZORDER);
    last_bounds_ = bounds;
    bounds_valid_ = true;
    PaintCarrierSurface();
    if (visible_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void GlassBackdropWindow::ResizeWithoutRepaint(const RECT& bounds) {
    if (!hwnd_ || !CanShow()) return;
    if (bounds_valid_ && EqualRect(&last_bounds_, &bounds)) return;

    const int width = std::max(1L, bounds.right - bounds.left);
    const int height = std::max(1L, bounds.bottom - bounds.top);
    SetWindowPos(hwnd_, nullptr, bounds.left, bounds.top, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
    last_bounds_ = bounds;
    bounds_valid_ = true;
}

void GlassBackdropWindow::Hide(bool animated) {
    if (!hwnd_ || !visible_) return;
    visible_ = false;
    bounds_valid_ = false;
    ShowWindow(hwnd_, SW_HIDE);
}

void GlassBackdropWindow::SetCornerRadius(int radius_px) {
    corner_radius_px_ = std::max(1, radius_px);
    bounds_valid_ = false;
}

void GlassBackdropWindow::SetTheme(OverlayThemeColor color) {
    theme_color_ = color;
}

void GlassBackdropWindow::ApplyBackdrop() {
    glass_effect_enabled_ = hwnd_ != nullptr;
}

void GlassBackdropWindow::ApplyRegion(const RECT& bounds) {
}

void GlassBackdropWindow::PaintCarrierSurface() {
    if (!hwnd_) return;

    RECT window_rect{};
    if (!GetWindowRect(hwnd_, &window_rect)) return;
    const int width = std::max(1L, window_rect.right - window_rect.left);
    const int height = std::max(1L, window_rect.bottom - window_rect.top);

    HDC screen_dc = GetDC(nullptr);
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bitmap = bitmap ? SelectObject(memory_dc, bitmap) : nullptr;
    if (bits && bitmap && BitBlt(memory_dc, 0, 0, width, height, screen_dc,
                                 window_rect.left, window_rect.top, SRCCOPY | CAPTUREBLT)) {
        auto* raw_pixels = static_cast<std::uint32_t*>(bits);
        std::vector<std::uint32_t> pixels(raw_pixels, raw_pixels + static_cast<std::size_t>(width) * height);
        BoxBlur(pixels, width, height, kBlurRadius);
        ApplyRoundedAlpha(pixels, width, height, corner_radius_px_);
        std::copy(pixels.begin(), pixels.end(), raw_pixels);

        POINT destination{window_rect.left, window_rect.top};
        POINT source{};
        SIZE size{width, height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, screen_dc, &destination, &size, memory_dc,
                            &source, 0, &blend, ULW_ALPHA);
    }
    if (old_bitmap) SelectObject(memory_dc, old_bitmap);
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
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
    PaintCarrierSurface();
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
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace voicestick
