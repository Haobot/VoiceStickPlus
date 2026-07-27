#include "overlay_window.h"
#include "dpi_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d2d1.h>
#include <dwrite.h>
#include <objidl.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dwrite.lib")
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Windowscodecs.lib")

namespace voicestick {

namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kOverlayFontFamily = L"Microsoft YaHei UI";
constexpr float kTextLayoutMaxHeight = 10000.0f;

struct TextLayoutSize {
    float width = 0.0f;
    float height = 0.0f;
    float top = 0.0f;
    UINT32 line_count = 1;
};

struct BitmapRenderTarget {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    UINT stride = 0;
    UINT buffer_size = 0;
};

struct BackgroundBrightness {
    bool valid = false;
    double luma = 0.5;
};

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

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

BackgroundBrightness SampleBackgroundBrightness(const RECT& bounds) {
    const int source_width = bounds.right - bounds.left;
    const int source_height = bounds.bottom - bounds.top;
    if (source_width <= 0 || source_height <= 0) return {};

    constexpr int kSampleWidth = 24;
    constexpr int kSampleHeight = 12;
    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) return {};
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (!memory_dc) {
        ReleaseDC(nullptr, screen_dc);
        return {};
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kSampleWidth;
    info.bmiHeader.biHeight = -kSampleHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return {};
    }

    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    const BOOL copied = StretchBlt(memory_dc, 0, 0, kSampleWidth, kSampleHeight,
                                   screen_dc, bounds.left, bounds.top,
                                   source_width, source_height, SRCCOPY | CAPTUREBLT);
    SelectObject(memory_dc, old_bitmap);

    BackgroundBrightness result{};
    if (copied) {
        const auto* pixels = static_cast<const BYTE*>(bits);
        double total_luma = 0.0;
        for (int i = 0; i < kSampleWidth * kSampleHeight; ++i) {
            const BYTE b = pixels[i * 4 + 0];
            const BYTE g = pixels[i * 4 + 1];
            const BYTE r = pixels[i * 4 + 2];
            total_luma += (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
        }
        result.valid = true;
        result.luma = total_luma / static_cast<double>(kSampleWidth * kSampleHeight);
    }

    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return result;
}

IDWriteFactory* SharedDwriteFactory() {
    static ComPtr<IDWriteFactory> factory = [] {
        ComPtr<IDWriteFactory> created;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(created.GetAddressOf()));
        return created;
    }();
    return factory.Get();
}

ID2D1Factory* SharedD2dFactory() {
    static ComPtr<ID2D1Factory> factory = [] {
        ComPtr<ID2D1Factory> created;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, created.GetAddressOf());
        return created;
    }();
    return factory.Get();
}

IWICImagingFactory* SharedWicFactory() {
    static ComPtr<IWICImagingFactory> factory = [] {
        ComPtr<IWICImagingFactory> created;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&created));
        return created;
    }();
    return factory.Get();
}

ComPtr<IDWriteTextFormat> CreateTextFormat(float font_size,
                                           DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
    ComPtr<IDWriteTextFormat> format;
    auto* factory = SharedDwriteFactory();
    if (!factory) return format;

    if (FAILED(factory->CreateTextFormat(kOverlayFontFamily, nullptr, weight,
                                         DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL,
                                         font_size, L"zh-cn", &format))) {
        return {};
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetTrimming(nullptr, nullptr);
    return format;
}

ComPtr<IDWriteTextLayout> CreateTextLayout(const std::wstring& text,
                                           IDWriteTextFormat* format,
                                           float width,
                                           bool should_wrap,
                                           float line_spacing = 0.0f,
                                           float baseline = 0.0f) {
    ComPtr<IDWriteTextLayout> layout;
    auto* factory = SharedDwriteFactory();
    if (!factory || !format) return layout;

    const std::wstring measured = text.empty() ? std::wstring(L" ") : text;
    format->SetWordWrapping(should_wrap ? DWRITE_WORD_WRAPPING_WRAP
                                        : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (FAILED(factory->CreateTextLayout(measured.c_str(),
                                         static_cast<UINT32>(measured.size()),
                                         format,
                                         std::max(1.0f, width),
                                         kTextLayoutMaxHeight,
                                         &layout))) {
        return {};
    }
    if (line_spacing > 0.0f && baseline > 0.0f) {
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                               line_spacing,
                               std::min(baseline, line_spacing));
    }
    return layout;
}

TextLayoutSize MeasureText(const std::wstring& text, float font_size,
                           float width, bool should_wrap,
                           float line_spacing = 0.0f,
                           float baseline = 0.0f) {
    TextLayoutSize result;
    auto format = CreateTextFormat(font_size);
    auto layout = CreateTextLayout(text, format.Get(), width, should_wrap,
                                   line_spacing, baseline);
    if (!layout) {
        result.width = width;
        result.height = font_size;
        return result;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics))) {
        result.width = std::ceil(std::max(metrics.width, metrics.widthIncludingTrailingWhitespace));
        result.height = std::ceil(metrics.height);
        result.top = metrics.top;
        result.line_count = std::max<UINT32>(1, metrics.lineCount);
    }
    return result;
}

BitmapRenderTarget CreateBitmapRenderTarget(void* bits, int width, int height) {
    BitmapRenderTarget result;
    auto* d2d_factory = SharedD2dFactory();
    auto* wic_factory = SharedWicFactory();
    if (!d2d_factory || !wic_factory || !bits || width <= 0 || height <= 0) return result;

    result.stride = static_cast<UINT>(width * 4);
    result.buffer_size = static_cast<UINT>(result.stride * height);
    if (FAILED(wic_factory->CreateBitmapFromMemory(
            static_cast<UINT>(width), static_cast<UINT>(height),
            GUID_WICPixelFormat32bppPBGRA, result.stride, result.buffer_size,
            static_cast<BYTE*>(bits), &result.bitmap))) {
        return {};
    }

    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    if (FAILED(d2d_factory->CreateWicBitmapRenderTarget(result.bitmap.Get(), properties,
                                                        &result.target))) {
        return {};
    }
    return result;
}

void DrawTextLayout(ID2D1RenderTarget* target, IDWriteTextLayout* layout,
                    float x, float y, BYTE alpha, BYTE rgb) {
    if (!target || !layout || alpha == 0) return;

    const float c = static_cast<float>(rgb) / 255.0f;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(c, c, c, static_cast<float>(alpha) / 255.0f), &brush))) {
        return;
    }
    target->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

} // namespace

OverlayWindow::OverlayWindow(HINSTANCE instance, HWND parent)
    : instance_(instance), parent_(parent) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayWindow::WndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"VoiceStickOverlayV2";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1, 1, parent_, nullptr, instance_, this);
    backdrop_ = std::make_unique<GlassBackdropWindow>(instance_, parent_);
    EnsureGdiplus();
}

OverlayWindow::~OverlayWindow() {
    if (hwnd_) {
        KillTimer(hwnd_, kAutoHideTimerId);
        KillTimer(hwnd_, kFadeTimerId);
        KillTimer(hwnd_, kAnimationTimerId);
        DestroyWindow(hwnd_);
    }
    ReleaseFrameBitmap();
}

void OverlayWindow::ShowListening() {
    Show(Mode::kListening, "Listening...");
}

void OverlayWindow::ShowPartial(const std::string& text) {
    // 最终结果显示：退出精修态，不再追加光标。
    refining_ = false;
    Show(Mode::kListening, text.empty() ? "Processing..." : text);
}

void OverlayWindow::AppendPartial(const std::string& text) {
    // 流式精修高频追加：跳过文字滚动过渡动画，直接显示当前累积文本。
    // 精修态下保持 kRefining 指示器（三点跳动），提示"正在改写"。
    Show(refining_ ? Mode::kRefining : Mode::kListening,
         text.empty() ? "Processing..." : text,
         /*hint=*/"", /*skip_text_transition=*/true);
}

void OverlayWindow::ShowRefining(const std::string& text) {
    // 进入精修态：kRefining 指示器（三点跳动）+ ASR 原文，让用户在 LLM 首 token
    // 到达前看到识别结果。文本本身不加光标（保持 static layer 复用，避免每帧重建
    // D2D 文本布局），"精修中"由指示器动效表达。
    refining_ = true;
    Show(Mode::kRefining, text.empty() ? "Processing..." : text,
         /*hint=*/"", /*skip_text_transition=*/true);
}

void OverlayWindow::ShowFinalCountdown(const std::string& text, std::function<void()> on_complete) {
    countdown_duration_ms_ = 1200;
    countdown_started_at_ms_ = GetTickCount64();
    Show(Mode::kCountdown, text);
    pending_callback_ = std::move(on_complete);
    SetTimer(hwnd_, kAutoHideTimerId, countdown_duration_ms_, nullptr);
}

void OverlayWindow::ShowPausedFinal(const std::string& text) {
    Show(Mode::kPaused, text, "Front: Send    Side: Cancel");
}

void OverlayWindow::ShowError(const std::string& text, std::function<void()> on_complete) {
    Show(Mode::kError, text.empty() ? "ASR Error" : text);
    pending_callback_ = std::move(on_complete);
    SetTimer(hwnd_, kAutoHideTimerId, 2000, nullptr);
}

void OverlayWindow::ShowTimedMessage(const std::string& text, int duration_ms,
                                     std::function<void()> on_complete) {
    duration_ms = std::max(duration_ms, 1);
    Show(Mode::kInfo, text);
    pending_callback_ = std::move(on_complete);
    SetTimer(hwnd_, kAutoHideTimerId, duration_ms, nullptr);
}

void OverlayWindow::SetThemeColor(OverlayThemeColor color) {
    if (theme_color_ == color) return;
    theme_color_ = color;
    if (color != OverlayThemeColor::kAuto) {
        resolved_theme_color_ = color;
    }
    if (backdrop_) backdrop_->SetTheme(resolved_theme_color_);
    InvalidateStaticLayer();
    if (mode_ != Mode::kHidden) UpdateLayeredBitmap();
}

void OverlayWindow::SetThemeSize(OverlayThemeSize size) {
    if (theme_size_ == size) return;
    theme_size_ = size;
    largest_visible_width_ = 0;
    largest_visible_height_ = 0;
    animated_window_width_ = 0;
    animated_window_height_ = 0;
    InvalidateStaticLayer();
    if (mode_ != Mode::kHidden) Reposition();
}

void OverlayWindow::SetPosition(OverlayPosition position) {
    if (position_ == position) return;
    position_ = position;
    if (mode_ != Mode::kHidden) Reposition();
}

void OverlayWindow::Hide(std::function<void()> on_hidden) {
    KillTimer(hwnd_, kAutoHideTimerId);
    KillTimer(hwnd_, kAnimationTimerId);
    refining_ = false;  // 退出精修态
    auto completion = on_hidden ? std::move(on_hidden) : std::move(pending_callback_);
    pending_callback_ = std::move(completion);
    StartFadeOut();
}

void OverlayWindow::OnTimer(UINT_PTR timer_id) {
    if (timer_id == kAutoHideTimerId) {
        KillTimer(hwnd_, kAutoHideTimerId);
        Hide();
    } else if (timer_id == kAnimationTimerId) {
        animation_frame_++;
        const bool window_moved = StepWindowAnimation();
        const bool text_transitioning = text_transition_started_at_ms_ != 0;
        // 仅在文字滚动过渡进行时才重建 static layer（含昂贵的 D2D 文本布局）。
        // window_moved 时 StepWindowAnimation 已自行 InvalidateStaticLayer；
        // kListening 静态文本时复用缓存 static layer，UpdateLayeredBitmap 只重绘动态指示器，
        // 消除每 16ms 全量重建文本布局导致的 UI 线程过载。
        if (!window_moved && text_transitioning) {
            InvalidateStaticLayer();
            UpdateLayeredBitmap();
        } else if (!window_moved && (mode_ == Mode::kListening || mode_ == Mode::kCountdown ||
                                     mode_ == Mode::kRefining)) {
            // 静态文本：仅重绘动态指示器（音浪条 / 倒计时环 / 精修三点跳动），不重建文本布局。
            UpdateLayeredBitmap();
        }
        if (!window_moved && mode_ != Mode::kListening && mode_ != Mode::kCountdown &&
            mode_ != Mode::kRefining && !text_transitioning) {
            KillTimer(hwnd_, kAnimationTimerId);
        }
    }
}

void OverlayWindow::OnPaint() {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    EndPaint(hwnd_, &ps);
    UpdateLayeredBitmap();
}

void OverlayWindow::Show(Mode mode, const std::string& text, const std::string& hint,
                         bool skip_text_transition) {
    KillTimer(hwnd_, kAutoHideTimerId);
    // 切到非精修模式时退出精修态，避免 refining_ 残留导致后续 AppendPartial 误判。
    if (mode != Mode::kRefining) refining_ = false;
    const std::wstring next_text = Utf16FromUtf8(text);
    if (skip_text_transition) {
        // 流式追加：直接用当前滚动偏移作为目标，不启动 140ms 过渡动画，
        // 避免高频 token 反复重置动画导致 scroll_offset 中途跳动闪动。
        text_scroll_from_offset_ = last_text_scroll_offset_;
        text_scroll_to_offset_ = last_text_scroll_offset_;
        text_transition_started_at_ms_ = 0;
    } else if (mode_ != Mode::kHidden && next_text != text_) {
        text_scroll_from_offset_ = last_text_scroll_offset_;
        text_transition_started_at_ms_ = GetTickCount64();
    } else if (mode_ == Mode::kHidden) {
        text_transition_started_at_ms_ = 0;
        text_scroll_from_offset_ = 0.0f;
        text_scroll_to_offset_ = 0.0f;
        last_text_scroll_offset_ = 0.0f;
    }
    mode_ = mode;
    text_ = next_text;
    hint_ = hint.empty() ? std::wstring() : Utf16FromUtf8(hint);
    InvalidateStaticLayer();
    Reposition();

    if (mode == Mode::kListening || mode == Mode::kCountdown || mode == Mode::kRefining ||
        NeedsWindowAnimation() || text_transition_started_at_ms_ != 0) {
        SetTimer(hwnd_, kAnimationTimerId, kAnimationStepMs, nullptr);
    } else {
        KillTimer(hwnd_, kAnimationTimerId);
    }

    current_alpha_ = kMaxAlpha;
    target_alpha_ = kMaxAlpha;
    KillTimer(hwnd_, kFadeTimerId);
    UpdateLayeredBitmap();
}

void OverlayWindow::Reposition() {
    RefreshDpi();

    RECT work_area = GetWorkAreaForCursor();
    const int screen_w = work_area.right - work_area.left;
    const int screen_h = work_area.bottom - work_area.top;

    const int shadow_padding = Dp(kShadowPadding);
    const int horizontal_padding = SizePx(kHorizontalPadding, 16, 14);
    const int vertical_padding = SizePx(kVerticalPadding, 12, 10);
    const int indicator_size = SizePx(kIndicatorSize, 22, 18);
    const int indicator_left_margin = IndicatorLeftMargin();
    const int text_indicator_gap = TextIndicatorGap();
    const int min_content_height = SizePx(kMinContentHeight, 68, 56);
    const int side_chrome_width = indicator_left_margin + indicator_size + text_indicator_gap + horizontal_padding;

    // Same layout algorithm for all modes, mirroring the macOS implementation.
    const int available_max_width = std::min(SizePx(kMaxContentWidth, 500, 380),
                                             screen_w - Dp(48) - shadow_padding * 2);
    const int max_text_width = std::max(1, available_max_width - side_chrome_width);

    const float text_font_size = SizePxF(kTextFontSize, 18, 16);
    const float text_line_height = text_font_size * kTextLineHeightMultiplier;
    const float text_baseline = text_font_size * kTextBaselineMultiplier;
    const auto single_line_text = MeasureText(text_, text_font_size,
                                              static_cast<float>(max_text_width) * 4.0f,
                                              false, text_line_height, text_baseline);
    const int text_width = max_text_width;
    int content_width = text_width + side_chrome_width;

    should_wrap_text_ = false;
    const auto laid_out_text = should_wrap_text_
        ? MeasureText(text_, text_font_size, static_cast<float>(text_width), true,
                      text_line_height, text_baseline)
        : single_line_text;
    const float text_height = laid_out_text.height;
    const float hint_height = hint_.empty()
        ? 0.0f
        : MeasureText(hint_, SizePxF(kHintFontSize, 12, 11), static_cast<float>(text_width), false).height +
              SizePxF(8, 6, 5);

    const int max_content_height = std::max(min_content_height,
                                            screen_h - Dp(120) - shadow_padding * 2);
    const int desired_content_height = static_cast<int>(std::ceil(
        vertical_padding * 2 + std::max(text_height + hint_height,
                                        static_cast<float>(indicator_size))));
    int content_height = std::min(max_content_height,
                                  std::max(min_content_height, desired_content_height));

    // Width and height only grow while visible (reset on hide).
    content_width = std::max(content_width, largest_visible_width_);
    content_height = std::max(content_height, largest_visible_height_);
    largest_visible_width_ = content_width;
    largest_visible_height_ = content_height;

    const bool first_layout = animated_window_width_ <= 0 || animated_window_height_ <= 0;
    target_window_width_ = content_width + shadow_padding * 2;
    target_window_height_ = content_height + shadow_padding * 2;
    const POINT origin = TargetWindowOrigin(work_area, target_window_width_, target_window_height_);
    target_window_x_ = origin.x;
    target_window_y_ = origin.y;

    if (first_layout) {
        animated_window_width_ = target_window_width_;
        animated_window_height_ = target_window_height_;
    }

    ResolveAutoThemeColor(BackdropBounds(target_window_width_, target_window_height_));

    const bool resizing = !first_layout && NeedsWindowAnimation();
    if (resizing && backdrop_) {
        const int saved_width = animated_window_width_;
        const int saved_height = animated_window_height_;
        animated_window_width_ = target_window_width_;
        animated_window_height_ = target_window_height_;
        SyncBackdrop(target_window_width_, target_window_height_, true);
        animated_window_width_ = saved_width;
        animated_window_height_ = saved_height;
        ResizeBackdropWithoutRepaint(target_window_width_, target_window_height_);
    } else {
        const RECT backdrop_bounds = BackdropBounds(target_window_width_, target_window_height_);
        const bool backdrop_needs_sync = !IsWindowVisible(hwnd_) ||
            !backdrop_bounds_valid_ || !EqualRect(&last_backdrop_bounds_, &backdrop_bounds);
        if (backdrop_needs_sync) {
            SyncBackdrop(target_window_width_, target_window_height_, true);
        }
    }
    if (target_window_width_ > 0 && target_window_height_ > 0) {
        SetWindowPos(hwnd_, HWND_TOPMOST, target_window_x_, target_window_y_,
                     target_window_width_, target_window_height_,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    UpdateLayeredBitmap();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

bool OverlayWindow::NeedsWindowAnimation() const {
    return animated_window_width_ != target_window_width_ ||
           animated_window_height_ != target_window_height_;
}

bool OverlayWindow::StepWindowAnimation() {
    if (!NeedsWindowAnimation()) return false;

    auto step = [this](int current, int target, int step_size) {
        const int delta = target - current;
        const int distance = std::abs(delta);
        if (distance <= Dp(kWindowResizeSnap)) return target;
        const int direction = delta > 0 ? 1 : -1;
        return current + direction * std::min(distance, Dp(step_size));
    };

    animated_window_width_ = step(animated_window_width_, target_window_width_,
                                  kWindowWidthResizeStep);
    animated_window_height_ = step(animated_window_height_, target_window_height_,
                                   kWindowHeightResizeStep);
    InvalidateStaticLayer();
    ResizeBackdropWithoutRepaint(target_window_width_, target_window_height_);
    ApplyAnimatedWindowBounds();
    return true;
}

void OverlayWindow::ApplyAnimatedWindowBounds() {
    UpdateLayeredBitmap();
}

RECT OverlayWindow::BackdropBounds(int width, int height) const {
    const int shadow_padding = Dp(kShadowPadding);
    const int visual_width = std::clamp(animated_window_width_, 1, width);
    const int visual_height = std::clamp(animated_window_height_, 1, height);
    const int visual_x = VisualOffsetX(width, visual_width);
    const int visual_y = VisualOffsetY(height, visual_height);
    const int inset = shadow_padding + 1;
    return RECT{
        target_window_x_ + visual_x + inset,
        target_window_y_ + visual_y + inset,
        target_window_x_ + visual_x + visual_width - inset,
        target_window_y_ + visual_y + visual_height - inset,
    };
}

void OverlayWindow::SyncBackdrop(int width, int height, bool show) {
    if (!backdrop_ || width <= 0 || height <= 0) return;
    const RECT bounds = BackdropBounds(width, height);
    const bool bounds_changed = !backdrop_bounds_valid_ || !EqualRect(&last_backdrop_bounds_, &bounds);
    if (!bounds_changed && IsWindowVisible(backdrop_->hwnd())) return;

    const bool content_visible = IsWindowVisible(hwnd_) != FALSE;
    if (content_visible) ShowWindow(hwnd_, SW_HIDE);
    if (show) {
        backdrop_->Show(bounds);
    } else {
        backdrop_->Move(bounds);
    }
    if (content_visible) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (show || bounds_changed) {
        SetWindowPos(backdrop_->hwnd(), hwnd_, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    last_backdrop_bounds_ = bounds;
    backdrop_bounds_valid_ = true;
}

void OverlayWindow::ResizeBackdropWithoutRepaint(int width, int height) {
    if (!backdrop_ || width <= 0 || height <= 0 || !backdrop_bounds_valid_) return;
    const RECT bounds = BackdropBounds(width, height);
    if (EqualRect(&last_backdrop_bounds_, &bounds)) return;
    backdrop_->ResizeWithoutRepaint(bounds);
    SetWindowPos(backdrop_->hwnd(), hwnd_, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    last_backdrop_bounds_ = bounds;
}

void OverlayWindow::ResolveAutoThemeColor(const RECT& bounds) {
    if (theme_color_ != OverlayThemeColor::kAuto) return;
    const auto brightness = SampleBackgroundBrightness(bounds);
    if (!brightness.valid) return;

    OverlayThemeColor next_color = resolved_theme_color_;
    if (brightness.luma <= 0.42) {
        next_color = OverlayThemeColor::kWhite;
    } else if (brightness.luma >= 0.58) {
        next_color = OverlayThemeColor::kBlack;
    }
    if (next_color == resolved_theme_color_) return;

    resolved_theme_color_ = next_color;
    if (backdrop_) backdrop_->SetTheme(resolved_theme_color_);
    InvalidateStaticLayer();
}

POINT OverlayWindow::TargetWindowOrigin(const RECT& work_area, int width, int height) const {
    const int margin = Dp(kPositionMargin);
    const int screen_w = work_area.right - work_area.left;
    const int screen_h = work_area.bottom - work_area.top;
    switch (position_) {
    case OverlayPosition::kTopLeft:
        return POINT{work_area.left + margin, work_area.top + margin};
    case OverlayPosition::kTopRight:
        return POINT{work_area.right - margin - width, work_area.top + margin};
    case OverlayPosition::kBottomLeft:
        return POINT{work_area.left + margin, work_area.bottom - margin - height};
    case OverlayPosition::kBottomRight:
        return POINT{work_area.right - margin - width, work_area.bottom - margin - height};
    case OverlayPosition::kBottomCenter: {
        const int bottom_gap = std::max(margin * 3, screen_h / 9);
        return POINT{work_area.left + (screen_w - width) / 2,
                     work_area.bottom - bottom_gap - height};
    }
    case OverlayPosition::kCenter:
    default:
        return POINT{work_area.left + (screen_w - width) / 2,
                     work_area.top + (screen_h - height) / 2};
    }
}

int OverlayWindow::VisualOffsetX(int width, int visual_width) const {
    switch (position_) {
    case OverlayPosition::kTopLeft:
    case OverlayPosition::kBottomLeft:
        return 0;
    case OverlayPosition::kTopRight:
    case OverlayPosition::kBottomRight:
        return width - visual_width;
    case OverlayPosition::kBottomCenter:
    case OverlayPosition::kCenter:
    default:
        return (width - visual_width) / 2;
    }
}

int OverlayWindow::VisualOffsetY(int height, int visual_height) const {
    switch (position_) {
    case OverlayPosition::kBottomLeft:
    case OverlayPosition::kBottomRight:
    case OverlayPosition::kBottomCenter:
        return height - visual_height;
    case OverlayPosition::kTopLeft:
    case OverlayPosition::kTopRight:
        return 0;
    case OverlayPosition::kCenter:
    default:
        return (height - visual_height) / 2;
    }
}

void OverlayWindow::RefreshDpi() {
    dpi_ = GetDpiForCursorMonitor();
    if (backdrop_) backdrop_->SetCornerRadius(Dp(kCornerRadius));
}

int OverlayWindow::Dp(int px) const {
    return voicestick::ScalePx(px, dpi_);
}

float OverlayWindow::DpF(int px) const {
    return ScaleF(px, dpi_);
}

int OverlayWindow::SizePx(int big_px, int medium_px, int small_px) const {
    switch (theme_size_) {
    case OverlayThemeSize::kMedium:
        return Dp(medium_px);
    case OverlayThemeSize::kSmall:
        return Dp(small_px);
    case OverlayThemeSize::kBig:
    default:
        return Dp(big_px);
    }
}

float OverlayWindow::SizePxF(int big_px, int medium_px, int small_px) const {
    switch (theme_size_) {
    case OverlayThemeSize::kMedium:
        return DpF(medium_px);
    case OverlayThemeSize::kSmall:
        return DpF(small_px);
    case OverlayThemeSize::kBig:
    default:
        return DpF(big_px);
    }
}

int OverlayWindow::IndicatorLeftMargin() const {
    return SizePx(18, 15, 13);
}

int OverlayWindow::TextIndicatorGap() const {
    return SizePx(9, 8, 7);
}

void OverlayWindow::UpdateLayeredBitmap() {
    RECT window_rect{};
    GetWindowRect(hwnd_, &window_rect);
    const int width = target_window_width_ > 0
        ? target_window_width_
        : window_rect.right - window_rect.left;
    const int height = target_window_height_ > 0
        ? target_window_height_
        : window_rect.bottom - window_rect.top;
    if (width <= 0 || height <= 0) return;
    if (!EnsureFrameBitmap(width, height)) return;

    if (static_layer_dirty_ ||
        static_cast<int>(static_layer_bits_.size()) != width * height * 4) {
        BuildStaticLayer(width, height);
    } else {
        std::memcpy(frame_bits_, static_layer_bits_.data(), static_layer_bits_.size());
    }

    HDC screen_dc = GetDC(nullptr);
    {
        Gdiplus::Graphics graphics(frame_dc_);
        graphics.SetPageUnit(Gdiplus::UnitPixel);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const int shadow_padding = Dp(kShadowPadding);
        const int indicator_size = SizePx(kIndicatorSize, 22, 18);
        const int visual_width = std::clamp(animated_window_width_, 1, width);
        const int visual_height = std::clamp(animated_window_height_, 1, height);
        const int visual_x = VisualOffsetX(width, visual_width);
        const int visual_y = VisualOffsetY(height, visual_height);
        const int content_height = visual_height - shadow_padding * 2;
        const int indicator_x = visual_x + shadow_padding + IndicatorLeftMargin();
        const int indicator_y = visual_y + shadow_padding + (content_height - indicator_size) / 2;
        PaintIndicator(graphics, indicator_x, indicator_y, indicator_size);
    }

    const int destination_x = target_window_width_ > 0 ? target_window_x_ : window_rect.left;
    const int destination_y = target_window_height_ > 0 ? target_window_y_ : window_rect.top;
    POINT destination = {destination_x, destination_y};
    POINT source = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, static_cast<BYTE>(current_alpha_), AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen_dc, &destination, &size, frame_dc_, &source, 0, &blend, ULW_ALPHA);

    ReleaseDC(nullptr, screen_dc);
}

bool OverlayWindow::EnsureFrameBitmap(int width, int height) {
    if (frame_dc_ && frame_bitmap_ && frame_width_ == width && frame_height_ == height) {
        return true;
    }

    ReleaseFrameBitmap();

    HDC screen_dc = GetDC(nullptr);
    frame_dc_ = CreateCompatibleDC(screen_dc);

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    frame_bitmap_ = CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS,
                                     &frame_bits_, nullptr, 0);
    ReleaseDC(nullptr, screen_dc);

    if (!frame_dc_ || !frame_bitmap_ || !frame_bits_) {
        ReleaseFrameBitmap();
        return false;
    }

    frame_old_bitmap_ = SelectObject(frame_dc_, frame_bitmap_);
    frame_width_ = width;
    frame_height_ = height;
    InvalidateStaticLayer();
    return true;
}

void OverlayWindow::ReleaseFrameBitmap() {
    if (frame_dc_ && frame_old_bitmap_) {
        SelectObject(frame_dc_, frame_old_bitmap_);
    }
    if (frame_bitmap_) {
        DeleteObject(frame_bitmap_);
    }
    if (frame_dc_) {
        DeleteDC(frame_dc_);
    }
    frame_dc_ = nullptr;
    frame_bitmap_ = nullptr;
    frame_old_bitmap_ = nullptr;
    frame_bits_ = nullptr;
    frame_width_ = 0;
    frame_height_ = 0;
    static_layer_bits_.clear();
    static_layer_dirty_ = true;
}

void OverlayWindow::InvalidateStaticLayer() {
    static_layer_dirty_ = true;
}

void OverlayWindow::BuildStaticLayer(int width, int height) {
    const std::size_t byte_count = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4;
    static_layer_bits_.assign(byte_count, 0);
    std::memset(frame_bits_, 0, byte_count);

    {
        Gdiplus::Graphics graphics(frame_dc_);
        graphics.SetPageUnit(Gdiplus::UnitPixel);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        PaintContent(graphics, width, height);
    }
    PaintText(frame_bits_, width, height);
    std::memcpy(static_layer_bits_.data(), frame_bits_, byte_count);
    static_layer_dirty_ = false;
}

void OverlayWindow::PaintContent(Gdiplus::Graphics& graphics, int width, int height) {
    const int shadow_padding = Dp(kShadowPadding);
    const int shadow_blur = Dp(kShadowBlur);
    const int shadow_y_offset = Dp(kShadowYOffset);
    const int corner_radius = Dp(kCornerRadius);
    const int visual_width = std::clamp(animated_window_width_, 1, width);
    const int visual_height = std::clamp(animated_window_height_, 1, height);
    const int visual_x = VisualOffsetX(width, visual_width);
    const int visual_y = VisualOffsetY(height, visual_height);

    Gdiplus::RectF background_rect(static_cast<float>(visual_x + shadow_padding) + 0.5f,
                                   static_cast<float>(visual_y + shadow_padding) + 0.5f,
                                   static_cast<float>(visual_width - shadow_padding * 2) - 1.0f,
                                   static_cast<float>(visual_height - shadow_padding * 2) - 1.0f);

    for (int layer = shadow_blur; layer >= 1; --layer) {
        const float t = static_cast<float>(shadow_blur - layer + 1) / static_cast<float>(std::max(1, shadow_blur));
        const float spread = static_cast<float>(layer);
        const BYTE alpha = static_cast<BYTE>(std::clamp(28.0f * t * t, 2.0f, 28.0f));
        Gdiplus::RectF shadow_rect(background_rect.X - spread,
                                   background_rect.Y - spread + static_cast<float>(shadow_y_offset),
                                   background_rect.Width + spread * 2.0f,
                                   background_rect.Height + spread * 2.0f);
        Gdiplus::GraphicsPath shadow_path;
        AddRoundedRect(shadow_path, shadow_rect, static_cast<float>(corner_radius) + spread);
        Gdiplus::SolidBrush shadow_brush(Gdiplus::Color(alpha, 0, 0, 0));
        graphics.FillPath(&shadow_brush, &shadow_path);
    }

    Gdiplus::GraphicsPath background_path;
    AddRoundedRect(background_path, background_rect, static_cast<float>(corner_radius));

    if (resolved_theme_color_ == OverlayThemeColor::kBlack) {
        Gdiplus::SolidBrush scrim_brush(Gdiplus::Color(1, 255, 255, 255));
        graphics.FillPath(&scrim_brush, &background_path);
        Gdiplus::Pen border_pen(Gdiplus::Color(68, 16, 16, 16), std::max(1.0f, DpF(1)));
        graphics.DrawPath(&border_pen, &background_path);
        return;
    }

    if (resolved_theme_color_ == OverlayThemeColor::kWhite) {
        Gdiplus::SolidBrush scrim_brush(Gdiplus::Color(1, 0, 0, 0));
        graphics.FillPath(&scrim_brush, &background_path);
    } else {
        Gdiplus::SolidBrush scrim_brush(Gdiplus::Color(kGlassScrimAlpha, 24, 24, 27));
        graphics.FillPath(&scrim_brush, &background_path);
    }

    Gdiplus::Pen border_pen(Gdiplus::Color(kGlassBorderAlpha, 255, 255, 255),
                            std::max(1.0f, DpF(1)));
    graphics.DrawPath(&border_pen, &background_path);
}

void OverlayWindow::PaintText(void* bits, int width, int height) {
    const BYTE ink_rgb = InkRgb();
    const int shadow_padding = Dp(kShadowPadding);
    const int horizontal_padding = SizePx(kHorizontalPadding, 16, 14);
    const int indicator_size = SizePx(kIndicatorSize, 22, 18);
    const int indicator_left_margin = IndicatorLeftMargin();
    const int text_indicator_gap = TextIndicatorGap();
    const int visual_width = std::clamp(animated_window_width_, 1, width);
    const int visual_height = std::clamp(animated_window_height_, 1, height);
    const int visual_x = VisualOffsetX(width, visual_width);
    const int visual_y = VisualOffsetY(height, visual_height);
    const float text_x = static_cast<float>(visual_x + shadow_padding + indicator_left_margin + indicator_size + text_indicator_gap);
    const float text_width = static_cast<float>(visual_width - shadow_padding * 2 -
                                                indicator_left_margin - indicator_size - text_indicator_gap - horizontal_padding);

    const float text_font_size = SizePxF(kTextFontSize, 18, 16);
    const float text_line_height = text_font_size * kTextLineHeightMultiplier;
    const float text_baseline = text_font_size * kTextBaselineMultiplier;
    struct FlowTextLayout {
        ComPtr<IDWriteTextLayout> layout;
        TextLayoutSize metrics;
        float draw_x = 0.0f;
    };
    auto make_flow_layout = [&](const std::wstring& value) {
        FlowTextLayout result;
        const auto measured_text = MeasureText(value, text_font_size, text_width * 8.0f,
                                               false, text_line_height, text_baseline);
        const float layout_width = std::max(text_width, measured_text.width + SizePxF(8, 6, 5));
        auto text_format = CreateTextFormat(text_font_size);
        if (text_format) text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        result.layout = CreateTextLayout(value, text_format.Get(), layout_width,
                                         false, text_line_height, text_baseline);
        if (result.layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(result.layout->GetMetrics(&metrics))) {
                result.metrics.width =
                    std::ceil(std::max(metrics.width, metrics.widthIncludingTrailingWhitespace));
                result.metrics.height = std::ceil(metrics.height);
                result.metrics.top = metrics.top;
                result.metrics.line_count = std::max<UINT32>(1, metrics.lineCount);
            }
        }
        result.draw_x = text_x - std::max(0.0f, result.metrics.width - text_width);
        return result;
    };
    FlowTextLayout current_text = make_flow_layout(text_);
    TextLayoutSize text_metrics = current_text.metrics;

    TextLayoutSize hint_metrics;
    ComPtr<IDWriteTextLayout> hint_layout;
    if (!hint_.empty()) {
        auto hint_format = CreateTextFormat(SizePxF(kHintFontSize, 12, 11), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (hint_format) hint_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        hint_layout = CreateTextLayout(hint_, hint_format.Get(), text_width, false);
        if (hint_layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(hint_layout->GetMetrics(&metrics))) {
                hint_metrics.width = std::ceil(std::max(metrics.width, metrics.widthIncludingTrailingWhitespace));
                hint_metrics.height = std::ceil(metrics.height);
                hint_metrics.top = metrics.top;
            }
        }
    }

    const float gap = hint_.empty() ? 0.0f : SizePxF(8, 6, 5);
    const float text_visual_top = text_metrics.top;
    const float text_visual_bottom = text_metrics.top + text_metrics.height;
    const float hint_y = text_metrics.height + gap;
    const float block_visual_top = text_visual_top;
    const float block_visual_bottom = hint_.empty()
        ? text_visual_bottom
        : std::max(text_visual_bottom, hint_y + hint_metrics.top + hint_metrics.height);
    const float block_visual_height = block_visual_bottom - block_visual_top;
    const float content_top = static_cast<float>(visual_y + shadow_padding);
    const float content_height = static_cast<float>(visual_height - shadow_padding * 2);
    const float vertical_optical_offset = hint_.empty() ? SizePxF(3, 2, 2) : 0.0f;
    const float block_y = content_top + std::max(0.0f, (content_height - block_visual_height) / 2.0f) - block_visual_top + vertical_optical_offset;

    auto render_target = CreateBitmapRenderTarget(bits, width, height);
    if (!render_target.target || !render_target.bitmap) return;
    const D2D1_RECT_F text_clip = D2D1::RectF(
        text_x,
        static_cast<float>(visual_y + shadow_padding),
        text_x + text_width,
        static_cast<float>(visual_y + visual_height - shadow_padding));
    text_scroll_to_offset_ = std::max(0.0f, current_text.metrics.width - text_width);
    float scroll_offset = text_scroll_to_offset_;
    if (text_transition_started_at_ms_ != 0) {
        const ULONGLONG elapsed_ms = GetTickCount64() - text_transition_started_at_ms_;
        const float progress = std::clamp(static_cast<float>(elapsed_ms) /
                                          static_cast<float>(kTextTransitionMs), 0.0f, 1.0f);
        const float eased = 1.0f - (1.0f - progress) * (1.0f - progress);
        scroll_offset = text_scroll_from_offset_ +
                        (text_scroll_to_offset_ - text_scroll_from_offset_) * eased;
        if (progress >= 1.0f) {
            text_transition_started_at_ms_ = 0;
        }
    }
    last_text_scroll_offset_ = scroll_offset;
    const float centered_offset = std::max(0.0f, (text_width - current_text.metrics.width) / 2.0f);
    const float text_draw_x = text_x + centered_offset - scroll_offset;
    render_target.target->BeginDraw();
    render_target.target->PushAxisAlignedClip(text_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float shadow_offset = std::max(1.0f, DpF(1));
    DrawTextLayout(render_target.target.Get(), current_text.layout.Get(), text_draw_x + shadow_offset,
                   block_y + shadow_offset, kTextShadowAlpha, 0);
    const float hint_draw_x = text_x + std::max(0.0f, (text_width - hint_metrics.width) / 2.0f);
    DrawTextLayout(render_target.target.Get(), hint_layout.Get(), hint_draw_x + shadow_offset,
                   block_y + text_metrics.height + gap + shadow_offset,
                   kTextShadowAlpha, 0);
    DrawTextLayout(render_target.target.Get(), current_text.layout.Get(), text_draw_x, block_y,
                   kTextAlpha, ink_rgb);
    DrawTextLayout(render_target.target.Get(), hint_layout.Get(), hint_draw_x,
                   block_y + text_metrics.height + gap, kHintAlpha, ink_rgb);
    render_target.target->PopAxisAlignedClip();
    if (SUCCEEDED(render_target.target->EndDraw())) {
        render_target.bitmap->CopyPixels(nullptr, render_target.stride,
                                         render_target.buffer_size,
                                         static_cast<BYTE*>(bits));
    }
}

BYTE OverlayWindow::InkRgb() const {
    return resolved_theme_color_ == OverlayThemeColor::kBlack ? 16 : kInkRgb;
}

void OverlayWindow::PaintIndicator(Gdiplus::Graphics& graphics, int x, int y, int size) {
    const BYTE ink_rgb = InkRgb();
    const int cx = x + size / 2;
    const int cy = y + size / 2;

    if (mode_ == Mode::kListening) {
        const int bar_width = std::max(2, SizePx(3, 3, 2));
        const int spacing = SizePx(4, 3, 3);
        const int num_bars = 3;
        const int total_w = num_bars * bar_width + (num_bars - 1) * spacing;
        const int start_x = cx - total_w / 2;
        const double elapsed = static_cast<double>(GetTickCount64() % 100000) / 1000.0;

        Gdiplus::SolidBrush bar_brush(Gdiplus::Color(kIndicatorAlpha, ink_rgb, ink_rgb, ink_rgb));
        for (int i = 0; i < num_bars; ++i) {
            const double phase = elapsed * 4.2 + i * 0.9;
            const int bar_h = SizePx(7, 6, 5) + static_cast<int>(SizePx(9, 7, 6) * (0.5 + 0.5 * std::sin(phase)));
            const int bx = start_x + i * (bar_width + spacing);
            const int by = cy - bar_h / 2;
            Gdiplus::GraphicsPath bar_path;
            AddRoundedRect(bar_path,
                           Gdiplus::RectF(static_cast<float>(bx), static_cast<float>(by),
                                          static_cast<float>(bar_width), static_cast<float>(bar_h)),
                           static_cast<float>(bar_width) / 2.0f);
            graphics.FillPath(&bar_brush, &bar_path);
        }
    } else if (mode_ == Mode::kRefining) {
        // 精修态：三个圆点依次起伏，节奏平缓，表"正在改写"（区别于 kListening 的音浪条）。
        const int dot_diameter = std::max(SizePx(6, 5, 4), 4);
        const int spacing = SizePx(4, 3, 3);
        const int num_dots = 3;
        const int total_w = num_dots * dot_diameter + (num_dots - 1) * spacing;
        const int start_x = cx - total_w / 2;
        const double elapsed = static_cast<double>(GetTickCount64() % 100000) / 1000.0;

        Gdiplus::SolidBrush dot_brush(Gdiplus::Color(kIndicatorAlpha, ink_rgb, ink_rgb, ink_rgb));
        for (int i = 0; i < num_dots; ++i) {
            // 每个点相位错开 0.35s，起伏幅度较小，呈"思考"般的轻柔跳动。
            const double phase = elapsed * 3.0 + i * 0.35;
            const double wave = 0.5 + 0.5 * std::sin(phase * 2.0 * 3.14159265358979 / 1.2);
            const int dx = start_x + i * (dot_diameter + spacing);
            // 垂直中心随波动上移（wave 越大越靠上），幅度限制在点直径内。
            const int dy = cy - dot_diameter / 2 - static_cast<int>(wave * dot_diameter * 0.5);
            Gdiplus::GraphicsPath dot_path;
            AddRoundedRect(dot_path,
                           Gdiplus::RectF(static_cast<float>(dx), static_cast<float>(dy),
                                          static_cast<float>(dot_diameter),
                                          static_cast<float>(dot_diameter)),
                           static_cast<float>(dot_diameter) / 2.0f);
            graphics.FillPath(&dot_brush, &dot_path);
        }
    } else if (mode_ == Mode::kCountdown) {
        Gdiplus::Pen track_pen(Gdiplus::Color(kIndicatorTrackAlpha, ink_rgb,
                                              ink_rgb, ink_rgb), SizePxF(3, 2, 2));
        Gdiplus::Pen ring_pen(Gdiplus::Color(kIndicatorAlpha, ink_rgb, ink_rgb, ink_rgb), SizePxF(3, 2, 2));
        ring_pen.SetStartCap(Gdiplus::LineCapRound);
        ring_pen.SetEndCap(Gdiplus::LineCapRound);
        const int inset = SizePx(5, 4, 4);
        Gdiplus::RectF ring_rect(static_cast<Gdiplus::REAL>(x + inset),
                                 static_cast<Gdiplus::REAL>(y + inset),
                                 static_cast<Gdiplus::REAL>(size - inset * 2),
                                 static_cast<Gdiplus::REAL>(size - inset * 2));
        graphics.DrawEllipse(&track_pen, ring_rect);

        const ULONGLONG elapsed_ms = countdown_started_at_ms_ == 0
            ? 0
            : GetTickCount64() - countdown_started_at_ms_;
        const float remaining = std::clamp(
            1.0f - static_cast<float>(elapsed_ms) / static_cast<float>(countdown_duration_ms_),
            0.0f, 1.0f);
        if (remaining > 0.0f) {
            graphics.DrawArc(&ring_pen, ring_rect, -90.0f, -360.0f * remaining);
        }
    } else if (mode_ == Mode::kPaused) {
        Gdiplus::Pen ring_pen(Gdiplus::Color(kIndicatorAlpha, ink_rgb, ink_rgb, ink_rgb), SizePxF(3, 2, 2));
        const int inset = SizePx(5, 4, 4);
        graphics.DrawEllipse(&ring_pen, x + inset, y + inset, size - inset * 2, size - inset * 2);
    } else if (mode_ == Mode::kError) {
        Gdiplus::Pen ring_pen(Gdiplus::Color(255, 200, 60, 60), SizePxF(3, 2, 2));
        const int inset = SizePx(5, 4, 4);
        graphics.DrawEllipse(&ring_pen, x + inset, y + inset, size - inset * 2, size - inset * 2);

        const int x_inset = size / 3;
        graphics.DrawLine(&ring_pen, x + x_inset, y + x_inset,
                          x + size - x_inset, y + size - x_inset);
        graphics.DrawLine(&ring_pen, x + size - x_inset, y + x_inset,
                          x + x_inset, y + size - x_inset);
    } else if (mode_ == Mode::kInfo) {
        // 中性信息指示：实心圆点。
        Gdiplus::SolidBrush dot_brush(Gdiplus::Color(kIndicatorAlpha, ink_rgb, ink_rgb, ink_rgb));
        const int inset = size / 4;
        graphics.FillEllipse(&dot_brush, x + inset, y + inset, size - inset * 2, size - inset * 2);
    }
}

void OverlayWindow::StartFadeIn() {
    current_alpha_ = kMaxAlpha;
    target_alpha_ = kMaxAlpha;
    KillTimer(hwnd_, kFadeTimerId);
    UpdateLayeredBitmap();
}

void OverlayWindow::StartFadeOut() {
    KillTimer(hwnd_, kFadeTimerId);
    current_alpha_ = 0;
    target_alpha_ = 0;
    UpdateLayeredBitmap();
    ShowWindow(hwnd_, SW_HIDE);
    if (backdrop_) backdrop_->Hide();
    backdrop_bounds_valid_ = false;
    mode_ = Mode::kHidden;
    largest_visible_width_ = 0;
    largest_visible_height_ = 0;
    animated_window_width_ = 0;
    animated_window_height_ = 0;
    target_window_width_ = 0;
    target_window_height_ = 0;
    target_window_x_ = 0;
    target_window_y_ = 0;
    ReleaseFrameBitmap();
    auto callback = std::move(pending_callback_);
    pending_callback_ = {};
    if (callback) callback();
}

void OverlayWindow::OnDpiChanged(UINT new_dpi, const RECT* suggested_rect) {
    dpi_ = new_dpi != 0 ? new_dpi : 96;
    ReleaseFrameBitmap();
    if (mode_ != Mode::kHidden) {
        largest_visible_width_ = 0;
        largest_visible_height_ = 0;
        animated_window_width_ = 0;
        animated_window_height_ = 0;
        Reposition();
    } else if (suggested_rect) {
        SetWindowPos(hwnd_, nullptr, suggested_rect->left, suggested_rect->top,
                     suggested_rect->right - suggested_rect->left,
                     suggested_rect->bottom - suggested_rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<OverlayWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT:
        self->OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        self->OnTimer(static_cast<UINT_PTR>(wp));
        return 0;
    case WM_DPICHANGED: {
        UINT new_dpi = HIWORD(wp);
        auto* rect = reinterpret_cast<const RECT*>(lp);
        self->OnDpiChanged(new_dpi, rect);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace voicestick
