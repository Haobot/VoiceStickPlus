#include "xiaomi_keymap_dialog.h"

#include "dpi_util.h"
#include "key_spec.h"
#include "resource.h"

#include <CommCtrl.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <gdiplus.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace voicestick {

namespace {

std::wstring Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), len);
    return wide;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring GetEditText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<std::size_t>(len));
    return text;
}

// {0} 占位符替换（标题含设备 ID）。
std::wstring FormatText(std::wstring text, std::initializer_list<std::wstring> values) {
    int index = 0;
    for (const auto& value : values) {
        const std::wstring placeholder = L"{" + std::to_wstring(index++) + L"}";
        std::size_t pos = 0;
        while ((pos = text.find(placeholder, pos)) != std::wstring::npos) {
            text.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    }
    return text;
}

void AlignDialogData(std::vector<BYTE>* buffer, std::size_t alignment) {
    while (buffer->size() % alignment != 0) buffer->push_back(0);
}

void AppendDialogData(std::vector<BYTE>* buffer, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

void AppendDialogWord(std::vector<BYTE>* buffer, WORD value) {
    AppendDialogData(buffer, &value, sizeof(value));
}

void AppendDialogWideString(std::vector<BYTE>* buffer, const wchar_t* text) {
    if (!text) {
        AppendDialogWord(buffer, 0);
        return;
    }
    while (*text) {
        AppendDialogWord(buffer, static_cast<WORD>(*text));
        ++text;
    }
    AppendDialogWord(buffer, 0);
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst,
                 DWORD extra_style = 0) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | extra_style,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                  UINT id, HINSTANCE inst, DWORD style = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
}

HWND CreateEdit(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           inst, nullptr);
}

// 选中键名称用粗体大字：基于 UI 字体放大 1.5 倍并加粗。
HFONT CreateLargeBoldFont(HFONT base) {
    LOGFONTW lf{};
    if (GetObjectW(base, sizeof(lf), &lf) == 0) return nullptr;
    lf.lfHeight = lf.lfHeight * 3 / 2;
    lf.lfWeight = FW_BOLD;
    return CreateFontIndirectW(&lf);
}

// 遥控器照片热区表（原始图片坐标系 228×889；oval=椭圆框高亮，rect=矩形框；
// 命中检测按矩形包含即可，ok 与方向键边界相接处优先 ok）。
struct Hotspot {
    std::string_view id;  // xiaomi_buttons.h button_id；mic 不在可映射表内但可选中查看说明
    int x1, y1, x2, y2;
    bool oval;
};

constexpr int kImageWidth = 228;
constexpr int kImageHeight = 889;

constexpr Hotspot kHotspots[] = {
    {"power",         36,  18, 114,  96, true},
    {"mic",          116,  18, 192,  96, true},
    {"up",            67, 102, 157, 152, false},
    {"left",          18, 152,  67, 242, false},
    {"ok",            67, 152, 157, 242, true},
    {"right",        157, 152, 206, 242, false},
    {"down",          67, 242, 157, 292, false},
    {"back",          18, 303,  99, 382, true},
    {"volume_up",    125, 300, 208, 393, false},
    {"home",          18, 385,  99, 467, true},
    {"volume_down",  125, 393, 208, 488, false},
    {"menu",          18, 470,  99, 550, true},
    {"tv",           125, 470, 208, 550, true},
};
// kHotspots 中 "ok" 的索引（命中检测优先）。
constexpr int kOkHotspotIndex = 4;

// RCDATA 内嵌 PNG → GDI+ Bitmap（FindResource/LoadResource/LockResource
// + SHCreateMemStream 内存流解码）。失败返回 nullptr（调用方降级为无图）。
std::unique_ptr<Gdiplus::Bitmap> LoadPngFromResource(HINSTANCE instance, int resource_id) {
    // 工程未定义 UNICODE，RT_RCDATA 是 ANSI 宏；RCDATA 类型 ID 恒为 10，用宽版宏。
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resource_id), MAKEINTRESOURCEW(10));
    if (!resource) return nullptr;
    HGLOBAL handle = LoadResource(instance, resource);
    if (!handle) return nullptr;
    const void* data = LockResource(handle);
    const DWORD size = SizeofResource(instance, resource);
    if (!data || size == 0) return nullptr;
    IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(data), size);
    if (!stream) return nullptr;
    auto bitmap = std::make_unique<Gdiplus::Bitmap>(stream);
    stream->Release();
    if (bitmap->GetLastStatus() != Gdiplus::Ok) return nullptr;
    return bitmap;
}

// button_id → 本地化按键名（13 个：kXiaomiMappableButtons 12 键 + mic）。
StringId ButtonNameStringId(std::string_view button_id) {
    if (button_id == "power") return StringId::kXiaomiButtonPower;
    if (button_id == "mic") return StringId::kXiaomiButtonMic;
    if (button_id == "up") return StringId::kXiaomiButtonUp;
    if (button_id == "down") return StringId::kXiaomiButtonDown;
    if (button_id == "left") return StringId::kXiaomiButtonLeft;
    if (button_id == "right") return StringId::kXiaomiButtonRight;
    if (button_id == "ok") return StringId::kXiaomiButtonOk;
    if (button_id == "back") return StringId::kXiaomiButtonBack;
    if (button_id == "home") return StringId::kXiaomiButtonHome;
    if (button_id == "volume_up") return StringId::kXiaomiButtonVolumeUp;
    if (button_id == "volume_down") return StringId::kXiaomiButtonVolumeDown;
    if (button_id == "menu") return StringId::kXiaomiButtonMenu;
    return StringId::kXiaomiButtonTv;
}

constexpr wchar_t kCanvasClassName[] = L"VoiceStickXiaomiRemoteCanvas";

} // namespace

// 遥控器照片画布：自绘子窗口（RegisterClassExW 一次性注册，双缓冲 GDI+ 绘制），
// 图片等比缩放居中显示；热区悬停高亮 + 手型光标，点击命中后经 WM_COMMAND
//（LOWORD=控件 ID，HIWORD=热区索引）通知对话框。
class XiaomiRemoteCanvas {
public:
    XiaomiRemoteCanvas() = default;
    ~XiaomiRemoteCanvas() {
        if (hwnd_) DestroyWindow(hwnd_);
    }
    XiaomiRemoteCanvas(const XiaomiRemoteCanvas&) = delete;
    XiaomiRemoteCanvas& operator=(const XiaomiRemoteCanvas&) = delete;

    HWND Create(HINSTANCE instance, HWND parent, int x, int y, int w, int h, UINT control_id) {
        EnsureClassRegistered(instance);
        control_id_ = control_id;
        CreateWindowExW(0, kCanvasClassName, L"", WS_CHILD | WS_VISIBLE,
                        x, y, w, h, parent,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(control_id)),
                        instance, this);
        return hwnd_;
    }

    // 非拥有指针：对话框持有 Bitmap 所有权（画布窗口先于 Bitmap 销毁，见 Show()）。
    void SetImage(Gdiplus::Bitmap* image) {
        image_ = image;
        Invalidate();
    }

    void SetSelected(int index) {
        selected_ = index;
        Invalidate();
    }

    void SetHint(std::wstring text, UiLanguage language) {
        hint_ = std::move(text);
        language_ = language;
        Invalidate();
    }

    int selected() const { return selected_; }

private:
    static void EnsureClassRegistered(HINSTANCE instance) {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = XiaomiRemoteCanvas::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kCanvasClassName;
        if (RegisterClassExW(&wc) != 0) registered = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        auto* canvas = reinterpret_cast<XiaomiRemoteCanvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(l_param);
            canvas = static_cast<XiaomiRemoteCanvas*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(canvas));
            canvas->hwnd_ = hwnd;
        }
        return canvas ? canvas->HandleMessage(message, w_param, l_param)
                      : DefWindowProcW(hwnd, message, w_param, l_param);
    }

    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
        switch (message) {
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_ERASEBKGND:
            return 1;  // 双缓冲自绘，不擦背景
        case WM_MOUSEMOVE: {
            if (!tracking_leave_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd_;
                TrackMouseEvent(&tme);
                tracking_leave_ = true;
            }
            const int hover = HitTest({GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)});
            if (hover != hover_) {
                hover_ = hover;
                Invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_leave_ = false;
            if (hover_ >= 0) {
                hover_ = -1;
                Invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const int hit = HitTest({GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)});
            if (hit >= 0) {
                selected_ = hit;
                Invalidate();
                SendMessageW(GetParent(hwnd_), WM_COMMAND,
                             MAKEWPARAM(control_id_, static_cast<WPARAM>(hit)),
                             reinterpret_cast<LPARAM>(hwnd_));
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l_param) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                SetCursor(LoadCursor(nullptr, HitTest(pt) >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, w_param, l_param);
    }

    void Invalidate() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 图片显示区域：等比缩放到可用高度，水平居中；底部留提示文字条。
    bool ComputeDisplay(float* origin_x, float* origin_y, float* scale) const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const UINT dpi = GetDpiForHwnd(hwnd_);
        const float margin = static_cast<float>(ScalePx(6, dpi));
        const float hint_h = static_cast<float>(ScalePx(24, dpi));
        const float avail_h = static_cast<float>(client.bottom) - hint_h - margin * 2.0f;
        if (avail_h <= 0.0f) return false;
        *scale = avail_h / static_cast<float>(kImageHeight);
        const float display_w = static_cast<float>(kImageWidth) * *scale;
        *origin_x = (static_cast<float>(client.right) - display_w) / 2.0f;
        *origin_y = margin;
        return true;
    }

    int HitTest(POINT pt) const {
        float origin_x = 0.0f, origin_y = 0.0f, scale = 1.0f;
        if (!ComputeDisplay(&origin_x, &origin_y, &scale)) return -1;
        const float ix = (static_cast<float>(pt.x) - origin_x) / scale;
        const float iy = (static_cast<float>(pt.y) - origin_y) / scale;
        if (ix < 0.0f || ix >= kImageWidth || iy < 0.0f || iy >= kImageHeight) return -1;
        const auto contains = [ix, iy](const Hotspot& hs) {
            return ix >= hs.x1 && ix <= hs.x2 && iy >= hs.y1 && iy <= hs.y2;
        };
        // ok 优先：ok 与方向键热区边界相接，重叠命中时取 ok。
        if (contains(kHotspots[kOkHotspotIndex])) return kOkHotspotIndex;
        for (int i = 0; i < static_cast<int>(std::size(kHotspots)); ++i) {
            if (i != kOkHotspotIndex && contains(kHotspots[i])) return i;
        }
        return -1;
    }

    void DrawOutline(Gdiplus::Graphics* graphics, int index, bool selected,
                     float origin_x, float origin_y, float scale, UINT dpi) const {
        const Hotspot& hs = kHotspots[index];
        // 选中 3px 蓝（RGB(22,119,255)），悬停 2px 浅蓝。
        Gdiplus::Pen pen(selected ? Gdiplus::Color(255, 22, 119, 255)
                                  : Gdiplus::Color(255, 145, 200, 255),
                         static_cast<float>(ScalePx(selected ? 3 : 2, dpi)));
        const float x = origin_x + hs.x1 * scale;
        const float y = origin_y + hs.y1 * scale;
        const float w = (hs.x2 - hs.x1) * scale;
        const float h = (hs.y2 - hs.y1) * scale;
        if (hs.oval) {
            graphics->DrawEllipse(&pen, x, y, w, h);
        } else {
            graphics->DrawRectangle(&pen, x, y, w, h);
        }
    }

    void OnPaint() {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;

        HDC mem_dc = CreateCompatibleDC(hdc);
        HBITMAP mem_bitmap = CreateCompatibleBitmap(hdc, width, height);
        HGDIOBJ old_bitmap = SelectObject(mem_dc, mem_bitmap);
        {
            Gdiplus::Graphics graphics(mem_dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            graphics.Clear(Gdiplus::Color(255, 255, 255, 255));

            const UINT dpi = GetDpiForHwnd(hwnd_);
            float origin_x = 0.0f, origin_y = 0.0f, scale = 1.0f;
            if (ComputeDisplay(&origin_x, &origin_y, &scale) && image_) {
                graphics.DrawImage(image_, origin_x, origin_y,
                                   static_cast<float>(kImageWidth) * scale,
                                   static_cast<float>(kImageHeight) * scale);
            }
            if (hover_ >= 0 && hover_ != selected_) {
                DrawOutline(&graphics, hover_, false, origin_x, origin_y, scale, dpi);
            }
            if (selected_ >= 0) {
                DrawOutline(&graphics, selected_, true, origin_x, origin_y, scale, dpi);
            }

            // 底部提示（GDI+ 不做字体回退链接，中文界面显式用雅黑避免方框）。
            if (!hint_.empty()) {
                const float hint_h = static_cast<float>(ScalePx(24, dpi));
                Gdiplus::FontFamily font_family(
                    language_ == UiLanguage::kSimplifiedChinese ? L"Microsoft YaHei" : L"Segoe UI");
                Gdiplus::Font font(&font_family, static_cast<float>(ScalePx(11, dpi)),
                                   Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush brush(Gdiplus::Color(255, 110, 110, 110));
                Gdiplus::StringFormat center_format;
                center_format.SetAlignment(Gdiplus::StringAlignmentCenter);
                center_format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                graphics.DrawString(hint_.c_str(), -1, &font,
                                    Gdiplus::RectF(0.0f, static_cast<float>(height) - hint_h,
                                                   static_cast<float>(width), hint_h),
                                    &center_format, &brush);
            }
        }
        BitBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
        SelectObject(mem_dc, old_bitmap);
        DeleteObject(mem_bitmap);
        DeleteDC(mem_dc);
        EndPaint(hwnd_, &ps);
    }

    HWND hwnd_ = nullptr;
    UINT control_id_ = 0;
    Gdiplus::Bitmap* image_ = nullptr;
    int hover_ = -1;
    int selected_ = -1;
    bool tracking_leave_ = false;
    std::wstring hint_;
    UiLanguage language_ = UiLanguage::kSystem;
};

XiaomiKeymapDialog::XiaomiKeymapDialog(HINSTANCE instance, HWND parent,
                                       std::string device_id,
                                       XiaomiSettings current,
                                       XiaomiSettings defaults,
                                       UiLanguage language)
    : instance_(instance), parent_(parent), device_id_(std::move(device_id)),
      current_(std::move(current)), defaults_(std::move(defaults)),
      language_(language), working_key_map_(current_.key_map) {}

XiaomiKeymapDialog::~XiaomiKeymapDialog() {
    capture_.Cancel();
    if (hwnd_) DestroyWindow(hwnd_);
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
    if (name_font_) {
        DeleteObject(name_font_);
        name_font_ = nullptr;
    }
}

void XiaomiKeymapDialog::Show() {
    // GDI+ 由本对话框自行初始化/配对关闭（win32_app 不持有 GDI+；
    // 参照 battery_monitor_dialog 的做法）。失败则降级为无图。
    Gdiplus::GdiplusStartupInput gdiplus_input;
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        gdiplus_token_ = 0;
    }
    DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), parent_,
                            XiaomiKeymapDialog::DialogProc, reinterpret_cast<LPARAM>(this));
    // GDI+ 对象必须先于 GdiplusShutdown 释放。
    image_.reset();
    if (gdiplus_token_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplus_token_);
        gdiplus_token_ = 0;
    }
}

INT_PTR CALLBACK XiaomiKeymapDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param,
                                                LPARAM l_param) {
    auto* dialog = reinterpret_cast<XiaomiKeymapDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<XiaomiKeymapDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        const auto language = EffectiveUiLanguage(dialog->language_);
        dialog->title_ = FormatText(TrW(StringId::kXiaomiKeymapTitle, language),
                                    {Utf16(dialog->device_id_)});
        SetWindowTextW(hwnd, dialog->title_.c_str());
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        RECT desired{0, 0, dialog->Dp(kClientWidth), dialog->Dp(kClientHeight)};
        AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dialog->dpi_);
        SetWindowPos(hwnd, nullptr, 0, 0, desired.right - desired.left,
                     desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);
        dialog->BuildControls();
        RECT window_rect{};
        GetWindowRect(hwnd, &window_rect);
        const int window_width = window_rect.right - window_rect.left;
        const int window_height = window_rect.bottom - window_rect.top;
        RECT work_area = GetWorkAreaForWindow(hwnd);
        const int x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
        const int y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR XiaomiKeymapDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kIdCanvas:
            OnCanvasButtonClicked(static_cast<int>(HIWORD(w_param)));
            return TRUE;
        case kIdCapture:
            StartCapture();
            return TRUE;
        case kIdApply:
            ApplyManualInput();
            return TRUE;
        case kIdClear:
            ClearMapping();
            return TRUE;
        case kIdRestoreDefaults:
            RestoreDefaults();
            return TRUE;
        case kIdSave:
            SaveSettings();
            return TRUE;
        case kIdCancel:
            capture_.Cancel();
            EndDialog(hwnd_, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        capture_.Cancel();
        EndDialog(hwnd_, IDCANCEL);
        return TRUE;
    case WM_TIMER:
        if (w_param == kCaptureHintTimerId) {
            // 录入期间长时间无键盘事件：大概率前台是提权窗口（UIPI 隔离钩子事件）。
            // 只提示一次，不中断捕获。
            KillTimer(hwnd_, kCaptureHintTimerId);
            if (capture_.active()) {
                const auto language = EffectiveUiLanguage(language_);
                MessageBoxW(hwnd_, TrW(StringId::kHotkeyCaptureTimeoutBody, language).c_str(),
                            TrW(StringId::kHotkeyCaptureTimeoutTitle, language).c_str(),
                            MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        const auto control = reinterpret_cast<HWND>(l_param);
        if (std::find(label_controls_.begin(), label_controls_.end(), control) !=
            label_controls_.end()) {
            auto dc = reinterpret_cast<HDC>(w_param);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }
    case WM_DESTROY:
        StopCaptureHintTimer();
        capture_.Cancel();
        hwnd_ = nullptr;
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

LPCDLGTEMPLATE XiaomiKeymapDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog_template{};
    dialog_template.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog_template.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog_template.cdit = 0;
    dialog_template.x = 0;
    dialog_template.y = 0;
    dialog_template.cx = 300;
    dialog_template.cy = 210;

    AppendDialogData(&dialog_template_, &dialog_template, sizeof(dialog_template));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, L"VoiceStick");
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void XiaomiKeymapDialog::BuildControls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ui_font_ = CreateUiFont(dpi_);
    name_font_ = CreateLargeBoldFont(ui_font_);
    const auto language = EffectiveUiLanguage(language_);

    auto remember = [&](HWND control) {
        if (control) all_controls_.push_back(control);
        return control;
    };
    auto remember_label = [&](HWND control) {
        if (control) label_controls_.push_back(control);
        return remember(control);
    };

    // ===== 左侧：遥控器照片画布 =====
    canvas_ = std::make_unique<XiaomiRemoteCanvas>();
    canvas_->Create(instance_, hwnd_, Dp(10), Dp(10), Dp(180), Dp(600), kIdCanvas);
    canvas_->SetHint(TrW(StringId::kXiaomiKeymapClickHint, language), language);
    if (gdiplus_token_ != 0) {
        image_ = LoadPngFromResource(instance_, IDR_XIAOMI_REMOTE_PNG);
        if (image_) canvas_->SetImage(image_.get());
    }

    // ===== 右侧：选中键信息与映射编辑 =====
    const int rx = Dp(210);
    const int rw = Dp(kClientWidth) - rx - Dp(10);

    key_name_label_ = remember_label(CreateLabel(
        hwnd_, L"", rx, Dp(12), rw, Dp(32), instance_));
    mapping_label_ = remember_label(CreateLabel(
        hwnd_, TrW(StringId::kXiaomiKeymapCurrentMapping, language).c_str(),
        rx, Dp(56), rw, Dp(20), instance_));
    mapping_value_ = remember_label(CreateLabel(
        hwnd_, L"", rx, Dp(78), rw, Dp(22), instance_));
    capture_button_ = remember(CreateButton(
        hwnd_, TrW(StringId::kXiaomiKeymapCapture, language).c_str(),
        rx, Dp(112), rw, Dp(32), kIdCapture, instance_));
    manual_hint_label_ = remember_label(CreateLabel(
        hwnd_, TrW(StringId::kXiaomiKeymapManualHint, language).c_str(),
        rx, Dp(156), rw, Dp(20), instance_));
    manual_edit_ = remember(CreateEdit(
        hwnd_, rx, Dp(178), rw - Dp(100), Dp(24), kIdManualEdit, instance_));
    apply_button_ = remember(CreateButton(
        hwnd_, TrW(StringId::kXiaomiKeymapApply, language).c_str(),
        rx + rw - Dp(90), Dp(178), Dp(90), Dp(24), kIdApply, instance_));
    clear_button_ = remember(CreateButton(
        hwnd_, TrW(StringId::kXiaomiKeymapClear, language).c_str(),
        rx, Dp(216), Dp(150), Dp(30), kIdClear, instance_));
    // mic 键说明文案：仅 mic 选中时显示（创建时不带 WS_VISIBLE）。
    mic_note_label_ = CreateWindowExW(
        0, L"STATIC", TrW(StringId::kXiaomiKeymapMicNote, language).c_str(),
        WS_CHILD | SS_LEFT, rx, Dp(56), rw, Dp(190), hwnd_, nullptr, instance_, nullptr);
    remember_label(mic_note_label_);

    const int btn_y = Dp(kClientHeight) - Dp(45);
    restore_defaults_button_ = remember(CreateButton(
        hwnd_, TrW(StringId::kEncoderSettingsRestoreDefaults, language).c_str(),
        Dp(10), btn_y, Dp(110), Dp(30), kIdRestoreDefaults, instance_));
    save_button_ = remember(CreateButton(
        hwnd_, TrW(StringId::kSave, language).c_str(),
        Dp(kClientWidth - 200), btn_y, Dp(80), Dp(30), kIdSave, instance_, BS_DEFPUSHBUTTON));
    cancel_button_ = remember(CreateButton(
        hwnd_, TrW(StringId::kCancel, language).c_str(),
        Dp(kClientWidth - 105), btn_y, Dp(80), Dp(30), kIdCancel, instance_));

    for (HWND control : all_controls_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }
    if (name_font_ && key_name_label_) {
        SendMessageW(key_name_label_, WM_SETFONT, reinterpret_cast<WPARAM>(name_font_), TRUE);
    }

    // 默认选中第一个按键（power），给出即时反馈。
    selected_hotspot_ = 0;
    canvas_->SetSelected(0);
    RefreshSidePanel();
}

void XiaomiKeymapDialog::OnCanvasButtonClicked(int hotspot_index) {
    if (hotspot_index < 0 || hotspot_index >= static_cast<int>(std::size(kHotspots))) return;
    // 切换按键时中止进行中的捕获（Cancel 不触发回调，手动恢复按钮文案）。
    if (capture_.active()) {
        capture_.Cancel();
        RestoreCaptureButtonText();
    }
    selected_hotspot_ = hotspot_index;
    canvas_->SetSelected(hotspot_index);
    RefreshSidePanel();
}

void XiaomiKeymapDialog::RefreshSidePanel() {
    if (!hwnd_ || selected_hotspot_ < 0) return;
    const auto language = EffectiveUiLanguage(language_);
    const Hotspot& hotspot = kHotspots[selected_hotspot_];
    const bool is_mic = hotspot.id == "mic";
    SetWindowTextW(key_name_label_, TrW(ButtonNameStringId(hotspot.id), language).c_str());

    // mic 为语音输入专用：右侧只显示说明文案，录入/手动/清除全部禁用。
    ShowWindow(mapping_label_, is_mic ? SW_HIDE : SW_SHOW);
    ShowWindow(mapping_value_, is_mic ? SW_HIDE : SW_SHOW);
    ShowWindow(mic_note_label_, is_mic ? SW_SHOW : SW_HIDE);
    EnableWindow(capture_button_, !is_mic);
    EnableWindow(manual_edit_, !is_mic);
    EnableWindow(apply_button_, !is_mic);
    EnableWindow(clear_button_, !is_mic);
    if (is_mic) return;

    const std::string button_id(hotspot.id);
    const auto it = working_key_map_.find(button_id);
    if (it != working_key_map_.end() && !it->second.empty()) {
        // 显示时规范化（配置里可能是手写的 "ctrl+shift+v"），解析失败则原文展示。
        const auto spec = ParseKeySpec(it->second);
        SetWindowTextW(mapping_value_,
                       Utf16(spec ? spec->display_text : it->second).c_str());
    } else {
        SetWindowTextW(mapping_value_,
                       TrW(StringId::kXiaomiKeymapNotMapped, language).c_str());
    }
}

void XiaomiKeymapDialog::StartCapture() {
    if (selected_hotspot_ < 0 || capture_.active()) return;
    const auto language = EffectiveUiLanguage(language_);
    SetWindowTextW(capture_button_,
                   TrW(StringId::kXiaomiKeymapCapturing, language).c_str());
    const int hotspot = selected_hotspot_;
    capture_.on_captured = [this, hotspot](const ShortcutCapture::Result& result) {
        const std::string button_id(kHotspots[hotspot].id);
        // 覆盖旧值（含覆盖为空串的显式取消）。
        working_key_map_[button_id] =
            MakeKeySpecFromVk(result.modifiers, result.vk).display_text;
        RestoreCaptureButtonText();
        RefreshSidePanel();
    };
    capture_.on_rejected_no_modifier = [this](UINT) {
        // require_modifier=false 不会触发；防御性恢复按钮文案。
        RestoreCaptureButtonText();
    };
    capture_.on_cancelled = [this]() {
        RestoreCaptureButtonText();
    };
    ShortcutCapture::Options options;
    options.require_modifier = false;  // 按键映射场景：单键也可
    capture_.Start(options);
    // 录入超时提示：kCaptureHintTimeoutMs 内无任何键盘事件时弹一次 UIPI 引导
    //（捕获不中断，用户关掉提示后仍可继续按键）。
    if (hwnd_ && capture_.active()) {
        SetTimer(hwnd_, kCaptureHintTimerId, kCaptureHintTimeoutMs, nullptr);
    }
}

void XiaomiKeymapDialog::RestoreCaptureButtonText() {
    // 捕获结束的公共汇合点：停掉超时提示定时器（幂等）。
    StopCaptureHintTimer();
    if (!capture_button_) return;
    SetWindowTextW(capture_button_,
                   TrW(StringId::kXiaomiKeymapCapture, EffectiveUiLanguage(language_)).c_str());
}

void XiaomiKeymapDialog::StopCaptureHintTimer() {
    if (hwnd_) {
        KillTimer(hwnd_, kCaptureHintTimerId);
    }
}

void XiaomiKeymapDialog::ApplyManualInput() {
    if (selected_hotspot_ < 0) return;
    const std::string text = Utf8(GetEditText(manual_edit_));
    const auto spec = ParseKeySpec(text);
    if (!spec.has_value()) {
        MessageBoxW(hwnd_, TrW(StringId::kXiaomiKeymapInvalid,
                               EffectiveUiLanguage(language_)).c_str(),
                    title_.c_str(), MB_OK | MB_ICONWARNING);
        return;  // 非法输入不写入工作副本
    }
    const std::string button_id(kHotspots[selected_hotspot_].id);
    working_key_map_[button_id] = spec->display_text;
    SetWindowTextW(manual_edit_, L"");
    RefreshSidePanel();
}

void XiaomiKeymapDialog::ClearMapping() {
    if (selected_hotspot_ < 0) return;
    const std::string button_id(kHotspots[selected_hotspot_].id);
    working_key_map_[button_id] = "";  // 显式取消（含取消全局默认）
    RefreshSidePanel();
}

void XiaomiKeymapDialog::RestoreDefaults() {
    working_key_map_ = defaults_.key_map;
    RefreshSidePanel();
}

void XiaomiKeymapDialog::SaveSettings() {
    // 捕获进行中保存：先取消，避免回调写到已关闭对话框。
    capture_.Cancel();
    XiaomiSettings edited = current_;
    edited.key_map = working_key_map_;
    current_ = edited;
    EndDialog(hwnd_, IDOK);
    if (on_settings_changed) {
        // 与全局默认一致 → nullopt（调用方清除覆盖，回落默认）。
        if (current_ == defaults_) {
            on_settings_changed(device_id_, std::nullopt);
        } else {
            on_settings_changed(device_id_, current_);
        }
    }
}

int XiaomiKeymapDialog::Dp(int px) const {
    return ScalePx(px, dpi_);
}

} // namespace voicestick
