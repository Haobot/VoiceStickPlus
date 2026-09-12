#include "xiaomi_keymap_dialog.h"

#include "dpi_util.h"
#include "key_spec.h"
#include "log.h"
#include "resource.h"
#include "xiaomi_buttons.h"

#include <CommCtrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
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
                                        static_cast<int>(text.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
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

// 按键 ID → 本地化按键名。
StringId ButtonNameId(std::string_view button_id) {
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

// 画布热区：设计稿 600×1280 像素（与 scripts/make_xiaomi_remote_png.ps1 一致），
// 坐标归一化到图片宽/高，半径相对图片宽。生成图改动时必须同步本表。
struct CanvasButton {
    std::string_view id;
    double cx;
    double cy;
    double r;  // 相对图片宽
};

constexpr double kCanvasWidth = 600.0;
constexpr double kCanvasHeight = 1280.0;

constexpr CanvasButton kCanvasButtons[] = {
    {"power", 300.0 / kCanvasWidth, 130.0 / kCanvasHeight, 55.0 / kCanvasWidth},
    {"mic", 300.0 / kCanvasWidth, 250.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"up", 300.0 / kCanvasWidth, 350.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"left", 185.0 / kCanvasWidth, 470.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"ok", 300.0 / kCanvasWidth, 470.0 / kCanvasHeight, 62.0 / kCanvasWidth},
    {"right", 415.0 / kCanvasWidth, 470.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"down", 300.0 / kCanvasWidth, 590.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"back", 210.0 / kCanvasWidth, 720.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"home", 390.0 / kCanvasWidth, 720.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"volume_up", 210.0 / kCanvasWidth, 840.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"volume_down", 390.0 / kCanvasWidth, 840.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"menu", 300.0 / kCanvasWidth, 960.0 / kCanvasHeight, 48.0 / kCanvasWidth},
    {"tv", 300.0 / kCanvasWidth, 1080.0 / kCanvasHeight, 55.0 / kCanvasWidth},
};

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

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                  UINT id, HINSTANCE inst, DWORD style = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
}

} // namespace

XiaomiKeymapDialog::XiaomiKeymapDialog(HINSTANCE instance, HWND parent,
                                       std::string device_id,
                                       XiaomiSettings current,
                                       XiaomiSettings defaults,
                                       UiLanguage language)
    : instance_(instance), parent_(parent), device_id_(std::move(device_id)),
      current_(std::move(current)), defaults_(std::move(defaults)),
      language_(language) {}

XiaomiKeymapDialog::~XiaomiKeymapDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (ui_font_) DeleteObject(ui_font_);
    if (title_font_) DeleteObject(title_font_);
}

void XiaomiKeymapDialog::Show() {
    DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), parent_,
                            XiaomiKeymapDialog::DialogProc, reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK XiaomiKeymapDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param,
                                                LPARAM l_param) {
    auto* dialog = reinterpret_cast<XiaomiKeymapDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<XiaomiKeymapDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        Gdiplus::GdiplusStartupInput gdiplus_input;
        if (Gdiplus::GdiplusStartup(&dialog->gdiplus_token_, &gdiplus_input, nullptr) ==
            Gdiplus::Ok) {
            // RCDATA 内嵌 PNG → 内存流 → GDI+ 解码（图片随 exe 分发，无外部依赖）。
            if (const HRSRC res = FindResourceW(dialog->instance_,
                                                MAKEINTRESOURCEW(IDR_XIAOMI_REMOTE_PNG),
                                                MAKEINTRESOURCEW(10) /* RT_RCDATA */)) {
                if (const HGLOBAL handle = LoadResource(dialog->instance_, res)) {
                    const void* data = LockResource(handle);
                    const DWORD size = SizeofResource(dialog->instance_, res);
                    if (data && size > 0) {
                        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size)) {
                            if (void* dst = GlobalLock(mem)) {
                                std::memcpy(dst, data, size);
                                GlobalUnlock(mem);
                                IStream* stream = nullptr;
                                if (SUCCEEDED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
                                    dialog->remote_image_ = Gdiplus::Bitmap::FromStream(stream);
                                    stream->Release();
                                } else {
                                    GlobalFree(mem);
                                }
                            } else {
                                GlobalFree(mem);
                            }
                        }
                    }
                }
            }
        }
        SetWindowTextW(hwnd, FormatText(TrW(StringId::kXiaomiKeymapTitle,
                                            EffectiveUiLanguage(dialog->language_)),
                                        {Utf16(dialog->device_id_)}).c_str());
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        RECT desired{0, 0, dialog->Dp(kClientWidth), dialog->Dp(kClientHeight)};
        AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dialog->dpi_);
        SetWindowPos(hwnd, nullptr, 0, 0, desired.right - desired.left,
                     desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);
        dialog->BuildControls();
        dialog->SelectButton("power");
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
        case kIdCapture:
            StartCapture();
            return TRUE;
        case kIdApply:
            ApplyManualInput();
            return TRUE;
        case kIdClear:
            ApplyMapping(selected_button_, "");
            return TRUE;
        case kIdClose:
            EndDialog(hwnd_, IDOK);
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        std::string button_id;
        if (!capturing_ && HitTestCanvas(point, &button_id)) {
            SelectButton(button_id);
        }
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        PaintCanvas(dc);
        EndPaint(hwnd_, &ps);
        return TRUE;
    }
    case WM_CLOSE:
        EndDialog(hwnd_, IDOK);
        return TRUE;
    case WM_DESTROY: {
        delete static_cast<Gdiplus::Image*>(remote_image_);
        remote_image_ = nullptr;
        if (gdiplus_token_) {
            Gdiplus::GdiplusShutdown(gdiplus_token_);
            gdiplus_token_ = 0;
        }
        hwnd_ = nullptr;
        return TRUE;
    }
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
    dialog_template.cx = 400;
    dialog_template.cy = 300;

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
    title_font_ = CreateUiFontBold(dpi_);
    const auto language = EffectiveUiLanguage(language_);

    button_name_label_ = CreateLabel(hwnd_, L"", 0, 0, 0, 0, instance_);
    mapping_value_label_ = CreateLabel(hwnd_, L"", 0, 0, 0, 0, instance_);
    mic_note_label_ = CreateLabel(hwnd_, TrW(StringId::kXiaomiKeymapMicNote, language).c_str(),
                                  0, 0, 0, 0, instance_);
    capture_button_ = CreateButton(
        hwnd_, TrW(StringId::kXiaomiKeymapCapture, language).c_str(),
        0, 0, 0, 0, kIdCapture, instance_);
    manual_hint_label_ = CreateLabel(
        hwnd_, TrW(StringId::kXiaomiKeymapManualHint, language).c_str(),
        0, 0, 0, 0, instance_);
    manual_edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdApply)),
                                   instance_, nullptr);
    apply_button_ = CreateButton(hwnd_, TrW(StringId::kXiaomiKeymapApply, language).c_str(),
                                 0, 0, 0, 0, kIdApply, instance_, BS_DEFPUSHBUTTON);
    clear_button_ = CreateButton(hwnd_, TrW(StringId::kXiaomiKeymapClear, language).c_str(),
                                 0, 0, 0, 0, kIdClear, instance_);
    click_hint_label_ = CreateLabel(hwnd_, TrW(StringId::kXiaomiKeymapClickHint, language).c_str(),
                                    0, 0, 0, 0, instance_);
    close_button_ = CreateButton(hwnd_, TrW(StringId::kClose, language).c_str(),
                                 0, 0, 0, 0, kIdClose, instance_);

    // 对话框背景（COLOR_BTNFACE）上绘制静态文本。
    for (HWND control : {button_name_label_, mapping_value_label_, mic_note_label_,
                         manual_hint_label_, click_hint_label_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }
    SendMessageW(button_name_label_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
    for (HWND control : {capture_button_, apply_button_, clear_button_, close_button_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }
    SendMessageW(manual_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);

    // 编辑框回车 = 应用。
    SendMessageW(manual_edit_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"ctrl+shift+v"));

    Relayout();
}

void XiaomiKeymapDialog::Relayout() {
    if (!hwnd_) return;

    // 左侧画布：图片 600:1280（宽:高），contain 适配 150×340 区域后垂直居中。
    const int canvas_x = Dp(10);
    const int canvas_y = Dp(10);
    const int canvas_w = Dp(150);
    const int canvas_h = Dp(340);
    const int image_w = canvas_w;
    const int image_h = static_cast<int>(image_w * kCanvasHeight / kCanvasWidth);
    image_rect_ = {canvas_x, canvas_y + (canvas_h - image_h) / 2, image_w, image_h};

    const int right_x = Dp(175);
    const int right_w = Dp(kClientWidth - 185);

    SetWindowPos(button_name_label_, nullptr, right_x, Dp(42), right_w, Dp(26),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(mapping_value_label_, nullptr, right_x, Dp(72), right_w, Dp(20),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(mic_note_label_, nullptr, right_x, Dp(100), right_w, Dp(48),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(capture_button_, nullptr, right_x, Dp(160), Dp(160), Dp(32),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(manual_hint_label_, nullptr, right_x, Dp(206), right_w, Dp(20),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(manual_edit_, nullptr, right_x, Dp(230), right_w - Dp(115), Dp(26),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(apply_button_, nullptr, Dp(kClientWidth - 105), Dp(228), Dp(95), Dp(30),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(clear_button_, nullptr, right_x, Dp(272), Dp(160), Dp(32),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(click_hint_label_, nullptr, canvas_x, Dp(358), canvas_w, Dp(40),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(close_button_, nullptr, Dp(kClientWidth - 105), Dp(390), Dp(95), Dp(30),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void XiaomiKeymapDialog::PaintCanvas(HDC dc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetSysColorBrush(COLOR_BTNFACE)));

    // 画布底板：浅色内嵌矩形，与对话框背景区分。
    const int canvas_x = Dp(10);
    const int canvas_y = Dp(10);
    const int canvas_w = Dp(150);
    const int canvas_h = Dp(340);
    RECT canvas_rect{canvas_x, canvas_y, canvas_x + canvas_w, canvas_y + canvas_h};
    FillRect(dc, &canvas_rect, reinterpret_cast<HBRUSH>(GetSysColorBrush(COLOR_WINDOW)));

    if (!remote_image_) return;
    auto* image = static_cast<Gdiplus::Image*>(remote_image_);
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.DrawImage(image,
                       static_cast<float>(image_rect_.left),
                       static_cast<float>(image_rect_.top),
                       static_cast<float>(image_rect_.right - image_rect_.left),
                       static_cast<float>(image_rect_.bottom - image_rect_.top));

    // 选中按键：圆角描边高亮。
    for (const CanvasButton& button : kCanvasButtons) {
        if (button.id != selected_button_) continue;
        Gdiplus::Pen pen(Gdiplus::Color(255, 0, 120, 215), 3.0f);
        const double scale_x = static_cast<double>(image_rect_.right - image_rect_.left);
        const double scale_y = static_cast<double>(image_rect_.bottom - image_rect_.top);
        const double center_x = image_rect_.left + button.cx * scale_x;
        const double center_y = image_rect_.top + button.cy * scale_y;
        const double radius = button.r * scale_x + 3.0;
        graphics.DrawEllipse(&pen, static_cast<float>(center_x - radius),
                             static_cast<float>(center_y - radius),
                             static_cast<float>(radius * 2.0),
                             static_cast<float>(radius * 2.0));
    }
}

bool XiaomiKeymapDialog::HitTestCanvas(POINT point, std::string* button_id) const {
    if (point.x < image_rect_.left || point.x >= image_rect_.right ||
        point.y < image_rect_.top || point.y >= image_rect_.bottom) {
        return false;
    }
    // 客户区坐标 → 设计稿像素（600×1280），与热区表同尺度比较。
    const double scale_x = static_cast<double>(image_rect_.right - image_rect_.left);
    const double scale_y = static_cast<double>(image_rect_.bottom - image_rect_.top);
    const double x = (point.x - image_rect_.left) / scale_x * kCanvasWidth;
    const double y = (point.y - image_rect_.top) / scale_y * kCanvasHeight;
    const CanvasButton* best = nullptr;
    double best_dist = 0.0;
    for (const CanvasButton& button : kCanvasButtons) {
        const double cx = button.cx * kCanvasWidth;
        const double cy = button.cy * kCanvasHeight;
        const double r = button.r * kCanvasWidth * 1.25;  // 宽松 25% 命中余量
        const double dist = std::hypot(x - cx, y - cy);
        if (dist <= r && (!best || dist < best_dist)) {
            best = &button;
            best_dist = dist;
        }
    }
    if (!best) return false;
    *button_id = std::string(best->id);
    return true;
}

void XiaomiKeymapDialog::SelectButton(const std::string& button_id) {
    selected_button_ = button_id;
    UpdateRightPanel();
    // 重绘画布高亮。
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

void XiaomiKeymapDialog::UpdateRightPanel() {
    const auto language = EffectiveUiLanguage(language_);
    const bool is_mic = selected_button_ == "mic";

    SetWindowTextW(button_name_label_, TrW(ButtonNameId(selected_button_), language).c_str());

    // 当前映射：key_map 空串/缺失 → 未映射（系统原生行为）。
    std::wstring mapping_text;
    if (!is_mic) {
        const auto it = current_.key_map.find(selected_button_);
        if (it != current_.key_map.end() && !it->second.empty()) {
            if (const auto spec = ParseKeySpec(it->second)) {
                mapping_text = Utf16(spec->display_text);
            }
        }
    }
    if (mapping_text.empty()) {
        mapping_text = TrW(StringId::kXiaomiKeymapNotMapped, language);
    }
    SetWindowTextW(mapping_value_label_, mapping_text.c_str());

    ShowWindow(mic_note_label_, is_mic ? SW_SHOW : SW_HIDE);
    const BOOL enable = is_mic ? FALSE : TRUE;
    EnableWindow(capture_button_, enable);
    EnableWindow(manual_edit_, enable);
    EnableWindow(apply_button_, enable);
    EnableWindow(clear_button_, enable);
}

void XiaomiKeymapDialog::StartCapture() {
    if (capturing_ || selected_button_ == "mic") return;
    const auto language = EffectiveUiLanguage(language_);
    capturing_ = true;
    SetWindowTextW(capture_button_,
                   TrW(StringId::kXiaomiKeymapCapturing, language).c_str());
    EnableWindow(manual_edit_, FALSE);
    EnableWindow(apply_button_, FALSE);
    EnableWindow(clear_button_, FALSE);
    const std::string button = selected_button_;
    capture_.on_captured = [this, button](const ShortcutCapture::Result& result) {
        const KeySpec spec = MakeKeySpecFromVk(result.modifiers, result.vk);
        ApplyMapping(button, ToLowerAscii(spec.display_text));
    };
    capture_.on_rejected_no_modifier = nullptr;
    capture_.on_cancelled = [this]() {
        // 仅在仍处于捕获态时恢复（ApplyMapping 回调链可能已复位界面）。
        if (!capturing_) return;
        capturing_ = false;
        const auto lang = EffectiveUiLanguage(language_);
        SetWindowTextW(capture_button_, TrW(StringId::kXiaomiKeymapCapture, lang).c_str());
        EnableWindow(manual_edit_, TRUE);
        EnableWindow(apply_button_, TRUE);
        EnableWindow(clear_button_, TRUE);
    };
    ShortcutCapture::Options options;
    options.require_modifier = false;  // 按键映射允许单键（如 esc、f5）。
    if (!capture_.Start(options)) {
        capturing_ = false;
        SetWindowTextW(capture_button_, TrW(StringId::kXiaomiKeymapCapture, language).c_str());
        EnableWindow(manual_edit_, TRUE);
        EnableWindow(apply_button_, TRUE);
        EnableWindow(clear_button_, TRUE);
    }
}

void XiaomiKeymapDialog::ApplyManualInput() {
    if (selected_button_ == "mic") return;
    const std::string text = ToLowerAscii(Utf8(GetEditText(manual_edit_)));
    if (text.empty()) return;
    const auto spec = ParseKeySpec(text);
    if (!spec) {
        MessageBoxW(hwnd_, TrW(StringId::kXiaomiKeymapInvalid,
                               EffectiveUiLanguage(language_)).c_str(),
                    L"VoiceStick", MB_OK | MB_ICONWARNING);
        return;
    }
    ApplyMapping(selected_button_, ToLowerAscii(spec->display_text));
    SetWindowTextW(manual_edit_, L"");
}

void XiaomiKeymapDialog::ApplyMapping(const std::string& button_id, const std::string& spec_text) {
    if (!IsXiaomiMappableButton(button_id)) return;

    XiaomiSettings edited = current_;
    // 与全局默认相同 → 从设备覆盖中删除（回落全局）；否则写入（空串 = 显式取消）。
    const auto default_it = defaults_.key_map.find(button_id);
    const bool same_as_default = default_it != defaults_.key_map.end() &&
                                 default_it->second == spec_text;
    if (same_as_default) {
        edited.key_map.erase(button_id);
    } else {
        edited.key_map[button_id] = spec_text;
    }
    current_ = edited;
    UpdateRightPanel();
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    FireChanged();
}

void XiaomiKeymapDialog::FireChanged() {
    if (!on_settings_changed) return;
    if (current_ == defaults_) {
        on_settings_changed(device_id_, std::nullopt);
    } else {
        on_settings_changed(device_id_, current_);
    }
}

int XiaomiKeymapDialog::Dp(int px) const {
    return ScalePx(px, dpi_);
}

} // namespace voicestick
