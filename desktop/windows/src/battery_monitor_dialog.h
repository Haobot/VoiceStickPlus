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
// 中部图区实时 GDI 曲线（WM_PAINT 双缓冲自绘），过程可停止，结束后可导出
// CSV（PowerLogAccumulator::FormatCsv）与 PNG 曲线。
// 窗口右上角另有供电态（USB）10min 自动关机开关与开发者模式开关；左上角
// 供电指示器（开发者模式开关左侧）按设备实时上报的电池状态显示：充电中
// 「充电中: XX分钟充满」/ 电池供电「预计可用: XX」。
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
    // 实时电池状态推送（设备每 10s 检查、变化即上报），刷新左上角供电指示器：
    // 充电中显示预计充满时间，电池供电显示预计可用时长，USB 供电未充电时隐藏。
    void OnBatteryStatus(const std::string& device_id, int level_percent, bool charging,
                        bool usb_powered);
    // 所有设备已断连（含本设备）：监测中转入等待重连（设备端 power_log 持续
    // 记录，重连后从 log_size_ 增量续传，数据不丢）；启动阶段仍按断连错误中止。
    void NotifyAllDisconnected();
    // 本设备重新出现在连接列表（win32_app 在 GATT 就绪后才派发）：等待重连态
    // 立即拉一次增量 dump 补回断连期间数据，随后恢复 60s 周期节律。
    void NotifyDeviceReconnected();

    // 发送 control_rx 命令到本设备（win32_app 桥接到 VoiceStickCoordinator::SendPowerLogCommand）。
    std::function<void(const std::string& device_id, ByteVector payload)> on_send_command;
    // 窗口销毁回调（win32_app 在此释放本对象；析构路径不触发）。
    std::function<void()> on_closed;

private:
    enum class MonitorState {
        kIdle,               // 空闲（未开始/已停止）
        kAnchoring,          // 已发 clear，正在写 time_anchor
        kProbing,            // 已发越界探测 dump，等待 eof 空片拿 total
        kMonitoring,         // 周期增量导出中
        kWaitingReconnect,   // 监测中链路断开：等待设备重连后续传
        kFinished,           // total_cycles_ 个周期跑完
        kError,              // 中止（原因见 error_reason_）
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
    void UpdateChargeIndicator();
    std::wstring FormatCountdownText() const;

    // 图区自绘（GDI，双缓冲）：横轴 0..total_cycles_ 分钟，纵轴数据自适应。
    RECT ChartRect() const;
    void InvalidateChart();
    void DrawChart(HDC hdc, const RECT& rect);

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
    HFONT chart_font_ = nullptr;        // 图区刻度/标签字体
    HFONT chart_title_font_ = nullptr;  // 图区标题字体
    bool closing_ = false;  // 析构路径置位，WM_DESTROY 不再回调 on_closed

    HWND status_label_ = nullptr;
    HWND warn_label_ = nullptr;
    HWND charge_label_ = nullptr;  // 左上角供电指示器（充电=充满预估，电池=续航预估）
    HWND usb_auto_off_check_ = nullptr;
    HWND dev_mode_check_ = nullptr;
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
    bool dev_mode_ = false;              // 开发者模式（会话级）：总时长 60 → 10 分钟
    bool battery_charging_ = false;      // 最近一次设备上报的充电状态（实时，独立于监测会话）
    bool battery_usb_powered_ = false;   // 最近一次设备上报的 USB 供电状态
    int battery_level_percent_ = 0;      // 最近一次设备上报的电量百分比（充/放电预估的输入）
    int total_cycles_ = kTotalCycles;    // 本轮监测总周期数（分钟），StartMonitoring 时锁定
    std::wstring status_override_;       // 保存结果等瞬态文案（非监测态优先于状态文案显示）
    std::int64_t session_start_epoch_s_ = -1;  // 会话起点（time_anchor 下发的 epoch），图区横轴基准

    static constexpr int kClientWidth = 680;
    static constexpr int kClientHeight = 480;

    static constexpr UINT kIdStart = 2110;
    static constexpr UINT kIdStop = 2111;
    static constexpr UINT kIdExportCsv = 2112;
    static constexpr UINT kIdExportPng = 2113;
    static constexpr UINT kIdClose = 2114;
    static constexpr UINT kIdUsbAutoOff = 2115;
    static constexpr UINT kIdDevMode = 2116;

    static constexpr UINT_PTR kTimerAnchorStep = 1;
    static constexpr UINT_PTR kTimerProbeTimeout = 2;
    static constexpr UINT_PTR kTimerCycle = 3;
    static constexpr UINT_PTR kTimerDumpTimeout = 4;
    static constexpr UINT_PTR kTimerStatusTick = 5;

    static constexpr std::uint32_t kProbeOffset = 1000000000;  // 越界 offset：只探测 total 不传数据
    static constexpr std::uint32_t kDumpChunkMax = 160;        // 单片原始字节上限（协议 ≤160）
    static constexpr int kTotalCycles = 60;                    // 默认 60 分钟 × 每分钟 1 个周期
    static constexpr int kDevModeTotalCycles = 10;              // 开发者模式：10 分钟快速验证
    static constexpr UINT kCycleIntervalMs = 60000;
    static constexpr UINT kAnchorStepMs = 400;                 // clear → 锚点 → 探测的间隔
    static constexpr UINT kProbeTimeoutMs = 20000;
    static constexpr UINT kDumpTimeoutMs = 20000;
    static constexpr int kMaxAttemptsPerCycle = 3;
    static constexpr int kMaxFailedCycles = 3;
    // 预计充满时长模型（分钟）：StickS3 电池约 200mAh（power_model.json），PMIC
    // 恒流充电约 200mA（1C），0→100% 约 1 小时。设备端无充电电流上报，电量
    // 百分比由电压线性映射（3350–4150mV → 0–100%）且充电中端电压偏高，估算
    // 仅作量级参考；电量接近充满（CV 涓流段）时改显示「即将充满」。
    static constexpr int kFullChargeMinutes = 60;
    static constexpr int kChargeAlmostFullPercent = 98;
    // 预计可用时长模型（分钟）：满电 200mAh 按混合典型电流 ~80mA（S0 亮屏 90 /
    // S1 暗屏 70，power_model.json）持续使用约 2.5 小时。设备电池供电下空闲
    // 10 分钟会自动关机，纯放置的实际可用时间远超此估计。
    static constexpr int kFullRuntimeMinutes = 150;
};

} // namespace voicestick
