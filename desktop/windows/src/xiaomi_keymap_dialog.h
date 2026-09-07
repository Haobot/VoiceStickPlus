#pragma once

#include "app_config.h"
#include "localization.h"
#include "shortcut_capture.h"

#include <Windows.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Gdiplus {
class Bitmap;
}

namespace voicestick {

class XiaomiRemoteCanvas;

// 小米遥控器按键映射对话框：从托盘设备子菜单「按键映射…」打开（仅小米遥控器显示）。
// 左侧为遥控器照片画布（点击选中按键），右侧编辑选中键的 key_map 映射
//（真实键盘录入 / 手动输入 / 清除）。传入的 current 为已用全局默认填平的有效值；
// 保存时与 defaults 相同则回调 std::nullopt（调用方 erase 覆盖，回落全局默认）。
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
    // 画布按键命中回调：切换选中键，刷新右侧面板。
    void OnCanvasButtonClicked(int hotspot_index);
    // 按当前选中键与 working_key_map_ 刷新右侧全部控件（含 mic 键特判）。
    void RefreshSidePanel();
    // 「按真实键盘录入」：启动 ShortcutCapture（require_modifier=false）。
    void StartCapture();
    // 捕获结束/取消/切换按键后恢复录入按钮文案。
    void RestoreCaptureButtonText();
    // 录入超时收尾：停提示定时器（幂等）。
    void StopCaptureHintTimer();
    // 「应用」：ParseKeySpec 校验手动输入，非法弹提示且不写入。
    void ApplyManualInput();
    // 「清除映射」：显式取消当前键映射（写空串，含取消全局默认）。
    void ClearMapping();
    // 「恢复默认」：工作副本重置为 defaults_.key_map 并刷新。
    void RestoreDefaults();
    void SaveSettings();
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    std::string device_id_;
    XiaomiSettings current_;     // 当前有效值（key_map 加载进工作副本；保存时回写）
    XiaomiSettings defaults_;    // 全局默认（「恢复默认」按钮与保存比较基准）
    UiLanguage language_;
    UINT dpi_ = 96;
    std::wstring title_;         // 「按键映射 - RC-{0}」，兼作 MessageBox 标题

    // 工作副本：button_id → key_spec 串（空串=显式取消）。初始为 current_.key_map。
    std::map<std::string, std::string> working_key_map_;
    int selected_hotspot_ = -1;  // 画布热区表索引（-1 = 未选中）

    std::unique_ptr<XiaomiRemoteCanvas> canvas_;
    std::unique_ptr<Gdiplus::Bitmap> image_;  // 从 RCDATA 资源解码的遥控器照片
    ULONG_PTR gdiplus_token_ = 0;             // 0 = GDI+ 未初始化（降级无图）

    HWND key_name_label_ = nullptr;
    HWND mapping_label_ = nullptr;
    HWND mapping_value_ = nullptr;
    HWND capture_button_ = nullptr;
    HWND manual_hint_label_ = nullptr;
    HWND manual_edit_ = nullptr;
    HWND apply_button_ = nullptr;
    HWND clear_button_ = nullptr;
    // mic 键选中时显示的说明文案（其余按键隐藏）。
    HWND mic_note_label_ = nullptr;
    HWND restore_defaults_button_ = nullptr;
    HWND save_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HFONT ui_font_ = nullptr;
    HFONT name_font_ = nullptr;  // 选中键名称：粗体大字

    ShortcutCapture capture_;

    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;
    std::vector<HWND> label_controls_;

    static constexpr int kClientWidth = 620;
    static constexpr int kClientHeight = 660;

    static constexpr UINT kIdCapture = 2800;
    static constexpr UINT kIdManualEdit = 2801;
    static constexpr UINT kIdApply = 2802;
    static constexpr UINT kIdClear = 2803;
    static constexpr UINT kIdSave = 2804;
    static constexpr UINT kIdCancel = 2805;
    static constexpr UINT kIdRestoreDefaults = 2806;
    // 画布子窗口控件 ID：命中通知走 WM_COMMAND，LOWORD=kIdCanvas、HIWORD=热区索引。
    static constexpr UINT kIdCanvas = 2807;

    // 录入超时提示定时器：录入启动后 kCaptureHintTimeoutMs 内无任何键盘事件到
    // 达（UIPI 前台提权隔离等）时弹一次引导，不中断进行中的捕获。
    static constexpr UINT_PTR kCaptureHintTimerId = 0x5343;
    static constexpr UINT kCaptureHintTimeoutMs = 3000;
};

} // namespace voicestick
