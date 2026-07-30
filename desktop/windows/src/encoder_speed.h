#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// 编码器旋转快慢分档测速。
//
// 判速依据：固件按 10ms 轮询窗口聚合旋转事件（{"event":"encoder_rotate","steps":N}），
// steps 即该窗口内的累计格数，单窗口格速 = steps * 100（格/秒）。
//
// 为什么不能直接用单窗口 steps*100 判速：测量量子是 100 格/秒（1 步/窗），阈值落在
// 100~200 之间时判定结果完全相同（单窗口 ≥2 步即判快），滑杆失去线性；且正常连转
// （1~3 步/窗）中偶发的 2 步窗口会越阈触发快速档 + 停转锁定，手感在阈值附近跳变。
// BLE 投递还可能把多个固件窗口合并到同一连接事件，事件到达间隔不可靠，不能用
// 到达时间差测速。
//
// 因此这里对单窗口格速做指数滑动平均（EWMA，α=0.5，按事件更新、与墙钟无关）：
//   estimate = 0.5 * (steps*100) + 0.5 * estimate
// 每次新手势（静默超过停转窗口 250ms）估计值从零冷启动。效果：
//   - 持续 1 步/窗 → 估计渐近 100 格/秒，永不越阈；
//   - 偶发 2 步窗口（起步量化抖动）→ 估计只抬到 ~100，不误判快；
//   - 持续 2 步/窗（真实 200 格/秒）→ 估计 2~3 窗后越过 150 区间，阈值 100~300
//     全程可获得近线性、单调的手感；
//   - 真快甩（单窗 4+ 步）→ 首窗估计即 ≥200，默认阈值下立即判快，响应不变慢。
// 冷启动从零起步是有意的：它给阈值附近的手势起步段一个宽容区，消除"稍微快一点
// 就触发快速档"的非线性跳变。
//
// threshold_sps <= 0 视为关闭快慢分档（永不判快）。
class EncoderRotateSpeedEstimator {
public:
    // 停转窗口：与协调器停转锁定同一时长；静默超过该值视为新手势，估计值清零冷启动。
    static constexpr auto kGestureGap = std::chrono::milliseconds(250);

    // 喂入一个旋转事件（固件一个 10ms 窗口的步数），返回平滑后的格速估计（格/秒）。
    double AddSample(std::chrono::steady_clock::time_point now, std::uint32_t steps) {
        if (!last_sample_at_.has_value() || now - *last_sample_at_ > kGestureGap) {
            estimate_sps_ = 0.0;
        }
        last_sample_at_ = now;
        estimate_sps_ = kAlpha * (static_cast<double>(steps) * 100.0) +
                        (1.0 - kAlpha) * estimate_sps_;
        return estimate_sps_;
    }

    void Reset() {
        estimate_sps_ = 0.0;
        last_sample_at_.reset();
    }

private:
    static constexpr double kAlpha = 0.5;
    double estimate_sps_ = 0.0;
    std::optional<std::chrono::steady_clock::time_point> last_sample_at_;
};

// 判快：平滑估计值达到阈值即判快。threshold_sps <= 0 永不判快。
inline bool EncoderRotateIsFast(double smoothed_speed_sps, int threshold_sps) {
    return threshold_sps > 0 && smoothed_speed_sps >= static_cast<double>(threshold_sps);
}
