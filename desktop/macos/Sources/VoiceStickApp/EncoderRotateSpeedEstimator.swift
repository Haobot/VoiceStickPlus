import Foundation

/// 编码器旋转快慢分档测速（对齐 Windows encoder_speed.h 的 EncoderRotateSpeedEstimator）。
///
/// 判速依据：固件按 10ms 轮询窗口聚合旋转事件（{"event":"encoder_rotate","steps":N}），
/// steps 即该窗口内的累计格数，单窗口格速 = steps * 100（格/秒）。
///
/// 为什么不能直接用单窗口 steps*100 判速：测量量子是 100 格/秒（1 步/窗），阈值落在
/// 100~200 之间时判定结果完全相同（单窗口 ≥2 步即判快），滑杆失去线性；且正常连转
/// （1~3 步/窗）中偶发的 2 步窗口会越阈触发快速档 + 停转锁定，手感在阈值附近跳变。
/// BLE 投递还可能把多个固件窗口合并到同一连接事件，事件到达间隔不可靠，不能用
/// 到达时间差测速。
///
/// 因此这里对单窗口格速做指数滑动平均（EWMA，α=0.5，按事件更新、与墙钟无关）：
///   estimate = 0.5 * (steps*100) + 0.5 * estimate
/// 每次新手势（静默超过停转窗口 250ms）估计值从零冷启动。冷启动从零起步是有意的：
/// 它给阈值附近的手势起步段一个宽容区，消除"稍微快一点就触发快速档"的非线性跳变。
struct EncoderRotateSpeedEstimator {
    /// 停转窗口：与协调器停转锁定同一时长；静默超过该值视为新手势，估计值清零冷启动。
    static let gestureGap: TimeInterval = 0.25

    private static let alpha = 0.5
    private var estimateSps = 0.0
    private var lastSampleAt: Date?

    /// 喂入一个旋转事件（固件一个 10ms 窗口的步数），返回平滑后的格速估计（格/秒）。
    mutating func addSample(now: Date, steps: UInt32) -> Double {
        if let last = lastSampleAt, now.timeIntervalSince(last) <= Self.gestureGap {
            // 同一手势延续
        } else {
            estimateSps = 0.0
        }
        lastSampleAt = now
        estimateSps = Self.alpha * (Double(steps) * 100.0) + (1.0 - Self.alpha) * estimateSps
        return estimateSps
    }

    mutating func reset() {
        estimateSps = 0.0
        lastSampleAt = nil
    }

    /// 判快：平滑估计值达到阈值即判快。thresholdSps <= 0 永不判快（关闭分档）。
    static func isFast(smoothedSpeedSps: Double, thresholdSps: Int) -> Bool {
        thresholdSps > 0 && smoothedSpeedSps >= Double(thresholdSps)
    }
}
