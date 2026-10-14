#pragma once

#include "byte_utils.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace voicestick {

// PCM 后处理链：三点平滑（首尾样本不动）→ HPF(120Hz) → 固定 dB 增益（钳位
// ±24 dB）→ 软件 AGC（含 0.8FS 瞬时限幅）。算法逐字移植固件
// audio_pipeline.c:55-153（HPF 系数与 AGC 常量），级间量化回 int16，
// 与 macOS Swift / Python 镜像 bit 级对齐（golden fixture 对拍）。
// HPF 置于固定增益之前：先削掉爆破音的 20–150Hz 能量，避免 +12dB 把它
// 推进 int16 饱和区。HPF/AGC 状态跨 Process 调用保持（按 640 样本帧流式
// 处理），每次录音会话开始由 XiaomiAtvvSession 调 Reset() 清零。
class PcmPostprocessor {
public:
    // 增益钳位范围（dB）。
    static constexpr double kMaxGainDb = 24.0;

    explicit PcmPostprocessor(double gain_db = 0.0, bool hpf = true, bool agc = true);

    void set_gain_db(double gain_db) { gain_db_ = ClampGainDb(gain_db); }
    double gain_db() const { return gain_db_; }
    bool hpf_enabled() const { return hpf_enabled_; }
    bool agc_enabled() const { return agc_enabled_; }

    // 清零 HPF/AGC 状态（新录音会话开始时调用；幂等）。
    void Reset();

    // 返回处理后的新缓冲；少于 3 个样本时跳过平滑只做增益。
    std::vector<std::int16_t> Process(std::span<const std::int16_t> pcm);

    static double ClampGainDb(double gain_db);

private:
    // 二阶 Butterworth 高通（transposed DF-II），fc≈120Hz @16kHz，Q=0.707
    // （逐字拷贝固件）：120Hz -3.0dB / 50Hz -15.3dB / 30Hz -24.1dB / ≥300Hz ~0dB。
    // （2026-10-14 由 90Hz 上调至 120Hz：喷麦爆点能量集中在 100-150Hz。）
    static constexpr double kHpfB0 = 0.967227;
    static constexpr double kHpfB1 = -1.934455;
    static constexpr double kHpfB2 = 0.967227;
    static constexpr double kHpfA1 = -1.933380;
    static constexpr double kHpfA2 = 0.935529;

    // 软件 AGC 常量（逐字拷贝固件；固件为 float，桌面端统一 double 保证
    // C++↔Python 对拍确定）。
    // 2026-10-14 收紧：目标 -6→-12dBFS、上限 +20→+12dB——旧参数把语音推到贴近
    // 满幅致爆点撞限幅，且 +20dB 上限在语音间隙把环境底噪抬高 20dB。
    static constexpr double kAgcTarget = 8192.0;       // -12 dBFS
    static constexpr double kAgcMaxGain = 4.0;         // +12 dB
    static constexpr double kAgcMinGain = 0.1;         // -20 dB
    static constexpr double kAgcNoiseFloor = 184.0;    // -45 dBFS
    static constexpr double kAgcEnvAttack = 0.98758;   // 快攻 5ms
    static constexpr double kAgcEnvRelease = 0.99979;  // 慢释 300ms
    static constexpr double kAgcGainUp = 0.99988;      // 慢上 500ms
    static constexpr double kAgcGainDown = 0.73162;    // 快下 2ms
    static constexpr double kAgcGainGate = 0.9999961;  // 噪声门缓回 ~1s
    static constexpr double kAgcCeil = 26214.0;        // 0.8FS 瞬时限幅

    int hpf_process(int x);
    int agc_process(int x);

    double gain_db_ = 0.0;
    bool hpf_enabled_ = true;
    bool agc_enabled_ = true;
    // HPF/AGC 滤波状态（会话开始时 Reset() 清零，防跨会话泄漏）。
    double hpf_z1_ = 0.0;
    double hpf_z2_ = 0.0;
    double agc_env_ = 0.0;
    double agc_gain_ = 1.0;
};

// ADPCM 裸字节流跨包累积切帧器：ATVV audio notify 无帧头无序号，
// 按 CAPS 协商帧长（默认 120 字节）累积，攒满一帧吐出一帧。
class FrameAccumulator {
public:
    explicit FrameAccumulator(std::size_t frame_bytes = 120);

    // 调整协商帧长并清空已累积字节（CAPS 到达时调用）。
    void set_frame_bytes(std::size_t frame_bytes);
    std::size_t frame_bytes() const { return frame_bytes_; }
    // 已累积、尚未凑满一帧的字节数。
    std::size_t pending_bytes() const { return buffer_.size(); }

    void Reset();
    // 追加字节，返回本次凑满的所有完整帧（可能为零或多帧）。
    std::vector<ByteVector> Append(std::span<const std::uint8_t> data);

private:
    std::size_t frame_bytes_;
    ByteVector buffer_;
};

} // namespace voicestick
