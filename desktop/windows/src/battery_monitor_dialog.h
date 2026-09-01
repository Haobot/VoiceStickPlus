#pragma once

#include "app_config.h"
#include "ble_protocol.h"
#include "localization.h"
#include "power_log_monitor.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

namespace voicestick {

// 电池电压监测窗口：托盘设备子菜单「电池电压监测…」打开的单设备非模态窗口。
// 开始监测后经 control_rx 的 power_log 命令族执行：clear → time_anchor →
// 基线探测（越界 offset 空 dump 拿 total）→ 每 60s 增量 dump，共 60 个周期；
// 过程可停止，结束后可导出 CSV（PowerLogAccumulator::FormatCsv）与 PNG 曲线。
// 窗口右上角另有供电态（USB）10min 自动关机开关。
// 全部设备回调（OnPowerLogFragment / OnPowerMgmtState / NotifyAllDisconnected）
// 由 win32_app 在 UI 线程派发。
class BatteryMonitorDialog {
public:
    BatteryMonitorDialog(HINSTANCE instance, HWND parent, UiLanguage language,
                         std::string device_id);
    ~BatteryMonitorDialog();

    void Show();
    bool IsSameDevice(const std::string& device_id) const;

    // power_log 导出分片（设备→主机）；非本设备或窗口未在监测时忽略。
    void OnPowerLogFragment(const std::string& device_id, const PowerLogFragment& fragment);
    // 供电态（USB）自动关机开关状态推送，刷新勾选框。
    void OnPowerMgmtState(const std::string& device_id, bool usb_auto_off);
    // 所有设备已断连（含本设备）：监测中立即按断连错误中止，避免干等导出超时。
    void NotifyAllDisconnected();

    // 发送 control_rx 命令到本设备（win32_app 桥接到 VoiceStickCoordinator::SendPowerLogCommand）。
    std::function<void(const std::string& device_id, ByteVector payload)> on_send_command;
    // 窗口销毁回调（win32_app 在此释放本对象；析构路径不触发）。
    std::function<void()> on_closed;

private:
    enum class MonitorState {
        kIdle,        // 空闲（未开始/已停止）
        kAnchoring,   // 已发 clear，正在写 time_anchor
        kProbing,     // 已发越界探测 dump，等待 eof 空片拿 total
        kMonitoring,  // 周期增量导出中
        kFinished,    // 60 个周期跑完
        kError,       // 中止（原因见 error_reason_）
    };

    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildUi();
    void DestroyControls();
    void CenterWindow();
    void SetText(HWND control, const std::wstring& text);
    int Dp(int px) const;

    void StartMonitoring();
    void StopMonitoring();  // 用户停止：回到 Idle，已采集数据保留可导出
    void AbortWithError(StringId reason);
    void FinishMonitoring();
    void BeginProbing();
    void ScheduleNextCycle();
    void StartDumpAttempt();
    void HandleDumpFailure();  // dump 超时/乱序：周期内重试，超限计失败周期
    void FinishCycle(std::uint32_t total);
    void KillSessionTimers();
    void SendCommand(ByteVector payload);

    void RefreshStatus();
    void UpdateButtons();
    void UpdateUsbWarning();
    std::wstring FormatCountdownText() const;

    void ExportCsv();
    void ExportPng();
    bool SavePng(const std::wstring& path, std::wstring* error);
    void ShowSaveResult(bool success, const std::wstring& detail);

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    UiLanguage language_;
    std::string device_id_;
    UINT dpi_ = 96;
    HFONT ui_font_ = nullptr;
    bool closing_ = false;  // 析构路径置位，WM_DESTROY 不再回调 on_closed

    HWND status_label_ = nullptr;
    HWND warn_label_ = nullptr;
    HWND usb_auto_off_check_ = nullptr;
    HWND start_button_ = nullptr;
    HWND stop_button_ = nullptr;
    HWND export_csv_button_ = nullptr;
    HWND export_png_button_ = nullptr;
    HWND close_button_ = nullptr;
    std::vector<HWND> all_controls_;
    std::vector<BYTE> dialog_template_;

    MonitorState state_ = MonitorState::kIdle;
    // state_ == kError 时的原因（kBatteryMonitorErr* 之一）。
    StringId error_reason_ = StringId::kBatteryMonitorErrDisconnected;
    PowerLogAccumulator accumulator_;
    std::uint32_t log_size_ = 0;  // 已确认的设备日志字节数（增量 dump 起点）
    int anchor_step_ = 0;         // kAnchoring 子步：1=已发 clear 待锚点；2=已发锚点待探测
    int cycle_ = 0;               // 当前周期序号（1..kTotalCycles）
    int attempt_ = 0;             // 当前周期内的 dump 尝试序号（1..kMaxAttemptsPerCycle）
    int failed_cycles_ = 0;       // 连续失败周期数
    bool dump_active_ = false;    // 有未完成的增量 dump 会话
    ByteVector pending_blob_;     // 当前 dump 会话已拼出的字节
    std::uint32_t expected_offset_ = 0;  // 下一分片期望 offset
    ULONGLONG next_cycle_tick_ = 0;      // 下次采集的 GetTickCount64 时刻（倒计时显示用）
    bool usb_auto_off_state_ = false;    // 最近一次设备上报的供电态自动关机开关状态
    std::wstring status_override_;       // 保存结果等瞬态文案（非监测态优先于状态文案显示）

    static constexpr int kClientWidth = 600;
    static constexpr int kClientHeight = 160;

    static constexpr UINT kIdStart = 2110;
    static constexpr UINT kIdStop = 2111;
    static constexpr UINT kIdExportCsv = 2112;
    static constexpr UINT kIdExportPng = 2113;
    static constexpr UINT kIdClose = 2114;
    static constexpr UINT kIdUsbAutoOff = 2115;

    static constexpr UINT_PTR kTimerAnchorStep = 1;
    static constexpr UINT_PTR kTimerProbeTimeout = 2;
    static constexpr UINT_PTR kTimerCycle = 3;
    static constexpr UINT_PTR kTimerDumpTimeout = 4;
    static constexpr UINT_PTR kTimerStatusTick = 5;

    static constexpr std::uint32_t kProbeOffset = 1000000000;  // 越界 offset：只探测 total 不传数据
    static constexpr std::uint32_t kDumpChunkMax = 160;        // 单片原始字节上限（协议 ≤160）
    static constexpr int kTotalCycles = 60;                    // 60 分钟 × 每分钟 1 个周期
    static constexpr UINT kCycleIntervalMs = 60000;
    static constexpr UINT kAnchorStepMs = 400;                 // clear → 锚点 → 探测的间隔
    static constexpr UINT kProbeTimeoutMs = 20000;
    static constexpr UINT kDumpTimeoutMs = 20000;
    static constexpr int kMaxAttemptsPerCycle = 3;
    static constexpr int kMaxFailedCycles = 3;
};

} // namespace voicestick
