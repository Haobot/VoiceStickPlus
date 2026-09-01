#include "power_log_monitor.h"

#include "byte_utils.h"

#include <cstdio>
#include <ctime>
#include <span>

namespace voicestick {

namespace {

// 电源模式 → 名字（与 scripts/e2e_test/power_log_dump.py 的 MODE_NAMES 一致）。
std::string PowerLogModeName(std::uint8_t mode) {
    switch (mode) {
    case 0: return "S0_ACTIVE";
    case 1: return "S1_RESTING";
    case 2: return "S2_SCREEN_OFF";
    case 3: return "S3_POWER_OFF";
    case 4: return "RECORDING";
    case 5: return "ADVERTISING";
    case 6: return "OTA";
    case kPowerLogModeTimeAnchor: return "TIME_ANCHOR";
    default: break;
    }
    return "UNKNOWN_" + std::to_string(mode);
}

// epoch 秒 → 本地 ISO 时间串（YYYY-MM-DDTHH:MM:SS）；转换失败返回空串。
std::string FormatIsoTimestamp(std::int64_t epoch_s) {
    std::tm tm_value{};
    const std::time_t t = static_cast<std::time_t>(epoch_s);
    if (localtime_s(&tm_value, &t) != 0) return {};
    char buf[32]{};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_value) == 0) return {};
    return buf;
}

} // namespace

bool ParsePowerLogEntry(const std::uint8_t* data, std::size_t size, PowerLogEntryData* out) {
    if (!data || size < 12 || !out) return false;
    const std::span<const std::uint8_t> bytes(data, size);
    out->uptime_s = ReadLe32(bytes.subspan(0));
    out->vbat_mv = ReadLe16(bytes.subspan(4));
    out->mode = bytes[6];
    out->flags = bytes[7];
    out->is_time_anchor = (out->flags & kPowerLogFlagTimeAnchor) != 0;
    out->anchor_epoch = ReadLe32(bytes.subspan(8));
    return true;
}

bool PowerLogAccumulator::ConsumeIncrementalBlob(
    const std::uint8_t* data, std::size_t size, std::vector<PowerLogSample>* new_samples) {
    if (!data && size > 0) return false;
    if (size % 12 != 0) return false;  // 流损坏：非整条目

    // 先完整解析并做重启检测（非锚点条目 uptime 回退 = 设备重启），
    // 全部通过前不改任何内部状态。
    std::vector<PowerLogEntryData> entries;
    entries.reserve(size / 12);
    for (std::size_t off = 0; off < size; off += 12) {
        PowerLogEntryData entry{};
        if (!ParsePowerLogEntry(data + off, 12, &entry)) return false;
        if (!entry.is_time_anchor && entry.uptime_s < last_uptime_s_) return false;
        entries.push_back(entry);
    }

    std::uint32_t max_uptime = last_uptime_s_;
    for (const auto& entry : entries) {
        if (entry.is_time_anchor) {
            // 多锚点取 uptime 最大者（流按时间序，后到的锚点 uptime 更大）。
            if (!has_anchor_ || entry.uptime_s > anchor_uptime_s_) {
                has_anchor_ = true;
                anchor_uptime_s_ = entry.uptime_s;
                anchor_epoch_s_ = entry.anchor_epoch;
            }
            continue;
        }
        if ((entry.flags & kPowerLogFlagPeriodic) == 0) continue;  // 模式切换事件不采样
        if (entry.uptime_s <= last_uptime_s_) continue;  // 旧条目（重叠/重复导出）
        PowerLogSample sample;
        sample.uptime_s = entry.uptime_s;
        sample.vbat_mv = entry.vbat_mv;
        sample.mode = entry.mode;
        sample.charging = (entry.flags & kPowerLogFlagCharging) != 0;
        sample.usb_powered = (entry.flags & kPowerLogFlagUsbPowered) != 0;
        // epoch 对齐：epoch = anchor_epoch + (uptime - anchor_uptime)。
        if (has_anchor_ && anchor_uptime_s_ <= entry.uptime_s) {
            sample.epoch_s = static_cast<std::int64_t>(anchor_epoch_s_) +
                             static_cast<std::int64_t>(entry.uptime_s - anchor_uptime_s_);
        }
        samples_.push_back(sample);
        if (new_samples) new_samples->push_back(sample);
        if (entry.uptime_s > max_uptime) max_uptime = entry.uptime_s;
    }
    last_uptime_s_ = max_uptime;
    return true;
}

std::string PowerLogAccumulator::FormatCsv() const {
    std::string csv = "seq,timestamp_iso,epoch_s,uptime_s,vbat_mv,vbat_v,"
                      "charging,usb_powered,mode_name,valid\r\n";
    std::size_t seq = 0;
    for (const auto& sample : samples_) {
        ++seq;
        char vbat_v[16]{};
        std::snprintf(vbat_v, sizeof(vbat_v), "%.3f", sample.vbat_mv / 1000.0);
        csv += std::to_string(seq);
        csv += ',';
        if (sample.epoch_s >= 0) csv += FormatIsoTimestamp(sample.epoch_s);
        csv += ',';
        if (sample.epoch_s >= 0) csv += std::to_string(sample.epoch_s);
        csv += ',';
        csv += std::to_string(sample.uptime_s);
        csv += ',';
        csv += std::to_string(sample.vbat_mv);
        csv += ',';
        csv += vbat_v;
        csv += ',';
        csv += sample.charging ? '1' : '0';
        csv += ',';
        csv += sample.usb_powered ? '1' : '0';
        csv += ',';
        csv += PowerLogModeName(sample.mode);
        csv += ',';
        csv += sample.vbat_mv > 0 ? '1' : '0';
        csv += "\r\n";
    }
    return csv;
}

} // namespace voicestick
