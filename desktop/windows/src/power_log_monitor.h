#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voicestick {

// power_log 12 字节条目的 flags 位定义（Doc/Ref/protocol.md「Power Log Export」）。
inline constexpr std::uint8_t kPowerLogFlagCharging = 0x01;         // bit0 充电中
inline constexpr std::uint8_t kPowerLogFlagUsbPowered = 0x02;       // bit1 USB 供电
inline constexpr std::uint8_t kPowerLogFlagPeriodic = 0x04;         // bit2 周期采样（非模式切换事件）
inline constexpr std::uint8_t kPowerLogFlagPoweroffSegment = 0x08;  // bit3 关机段恢复记录
inline constexpr std::uint8_t kPowerLogFlagTimeAnchor = 0x10;       // bit4 时间锚点（mode=0xFF，reserved 存 epoch）

// 时间锚点条目的 mode 值。
inline constexpr std::uint8_t kPowerLogModeTimeAnchor = 0xFF;

// 单条 12 字节 power_log 条目的解析结果（多字节字段均为小端）。
struct PowerLogEntryData {
    std::uint32_t uptime_s = 0;      // esp_timer 相对秒
    std::uint16_t vbat_mv = 0;       // 记录时电池电压（0 = 设备 ADC 读失败）
    std::uint8_t mode = 0;           // 电源模式；锚点条目为 0xFF
    std::uint8_t flags = 0;
    bool is_time_anchor = false;     // flags 含 kPowerLogFlagTimeAnchor
    std::uint32_t anchor_epoch = 0;  // 仅锚点条目有效（reserved[0..3] 的墙钟 epoch 秒）
};

// 解析单条 12 字节 power_log 条目；data 为空、size 不足 12 或 out 为空返回 false。
bool ParsePowerLogEntry(const std::uint8_t* data, std::size_t size, PowerLogEntryData* out);

// 一个电池电压采样点（由 60s 周期采样条目产生）。epoch_s 为经时间锚点对齐的
// 墙钟秒，无可用锚点时为 -1。
struct PowerLogSample {
    std::int64_t epoch_s = -1;
    std::uint32_t uptime_s = 0;
    std::uint16_t vbat_mv = 0;
    std::uint8_t mode = 0;
    bool charging = false;
    bool usb_powered = false;
};

// power_log 增量累积器：喂入 dump 会话拼出的裸条目流（不含 16 字节头，长度
// 为 12 的倍数），产出按墙钟对齐的电压采样序列。语义与
// scripts/e2e_test/battery_voltage_monitor.py 的 live 模式一致：
// - 时间锚点条目只更新对齐基准（取 uptime 最大者），不产生采样；
// - 仅带 kPowerLogFlagPeriodic 的条目产生采样，且跳过 uptime 不超过已见值的旧条目；
// - 任何非锚点条目的 uptime 回退视为设备重启，返回 false 且内部状态不变。
class PowerLogAccumulator {
public:
    // 消费一段增量裸条目流。成功返回 true，本次新产生的采样追加到 new_samples
    // （可为 nullptr 表示不回取）。失败（长度非 12 倍数 / uptime 回退）返回
    // false，samples、锚点与 last_uptime_s 均保持不变。
    bool ConsumeIncrementalBlob(const std::uint8_t* data, std::size_t size,
                                std::vector<PowerLogSample>* new_samples);

    const std::vector<PowerLogSample>& samples() const { return samples_; }
    // 已见最大周期采样 uptime（尚无采样时为 0）。
    std::uint32_t last_uptime_s() const { return last_uptime_s_; }
    // 导出 CSV，列与 battery_voltage_monitor.py 相同：
    // seq,timestamp_iso,epoch_s,uptime_s,vbat_mv,vbat_v,charging,usb_powered,mode_name,valid
    std::string FormatCsv() const;

private:
    std::vector<PowerLogSample> samples_;
    std::uint32_t last_uptime_s_ = 0;
    bool has_anchor_ = false;
    std::uint32_t anchor_uptime_s_ = 0;
    std::uint32_t anchor_epoch_s_ = 0;
};

} // namespace voicestick
