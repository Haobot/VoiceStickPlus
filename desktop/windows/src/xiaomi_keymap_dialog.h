#pragma once

#include "app_config.h"
#include "localization.h"
#include "shortcut_capture.h"

#include <Windows.h>

#include <functional>
#include <optional>
#include <string>

namespace voicestick {

// 设备级按键映射对话框：从托盘设备子菜单「按键映射…」打开（仅小米遥控器显示）。
// 左侧为遥控器产品图画布，点击图中按键选中；右侧对该键录入（真实键盘捕获或
// 手动输入语法）或清除映射，每次修改立即经 on_settings_changed 持久化。
// 传入的 current 为已用全局默认填平的有效值；保存时与 defaults 相同则回调
// std::nullopt（调用方 erase 覆盖，回落全局默认）。mic 语音键不可映射。
class XiaomiKeymapDialog {
public:
    XiaomiKeymapDialog(HINSTANCE instance, HWND parent,
                       std::string device_id,
                       XiaomiSettings current,
                       XiaomiSettings defaults,
                       UiLanguage language);
    ~XiaomiKeymapDialog();

    void Show();

    // (device_id, override)：override 为 nullopt 表示与全局默认一致（清除覆盖）。
    std::function<void(const std::string& device_id,
                       std::optional<XiaomiSettings> override)> on_settings_changed;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildControls();
    void Relayout();
    void PaintCanvas(HDC dc);
    void SelectButton(const std::string& button_id);
    void UpdateRightPanel();
    void StartCapture();
    void ApplyManualInput();
    void ApplyMapping(const std::string& button_id, const std::string& spec_text);
    void FireChanged();
    bool HitTestCanvas(POINT point, std::string* button_id) const;
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    std::string device_id_;
    XiaomiSettings current_;     // 编辑中的设备有效值（每次修改立即回调持久化）
    XiaomiSettings defaults_;    // 全局默认（回调 nullopt 的比较基准）
    UiLanguage language_;
    UINT dpi_ = 96;

    ShortcutCapture capture_;
    bool capturing_ = false;

    // GDI+（图片解码/绘制）。token 在 WM_INITDIALOG 启动、WM_DESTROY 关闭。
    ULONG_PTR gdiplus_token_ = 0;
    void* remote_image_ = nullptr;  // Gdiplus::Image*（void* 避免头文件泄漏进公共头）

    std::string selected_button_;
    RECT image_rect_{};  // 画布内图片显示矩形（客户区像素，Relayout 更新）

    HWND button_name_label_ = nullptr;
    HWND mapping_value_label_ = nullptr;
    HWND mic_note_label_ = nullptr;
    HWND capture_button_ = nullptr;
    HWND manual_hint_label_ = nullptr;
    HWND manual_edit_ = nullptr;
    HWND apply_button_ = nullptr;
    HWND clear_button_ = nullptr;
    HWND click_hint_label_ = nullptr;
    HWND close_button_ = nullptr;
    HFONT ui_font_ = nullptr;
    HFONT title_font_ = nullptr;

    std::vector<BYTE> dialog_template_;

    static constexpr int kClientWidth = 560;
    static constexpr int kClientHeight = 430;

    static constexpr UINT kIdApply = 2800;
    static constexpr UINT kIdClear = 2801;
    static constexpr UINT kIdCapture = 2802;
    static constexpr UINT kIdClose = 2803;
};

} // namespace voicestick
