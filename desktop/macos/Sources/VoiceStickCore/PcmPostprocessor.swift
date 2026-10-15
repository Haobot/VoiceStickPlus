import Foundation

/// PCM 后处理链：三点平滑（首尾样本不动）→ HPF(120Hz) → 固定 dB 增益（钳位 ±24 dB）
/// → 软件 AGC（含 0.8FS 瞬时限幅）。算法逐字移植固件 audio_pipeline.c:55-153
///（HPF 系数与 AGC 常量），级间量化回 int16，与 Windows C++ / Python 镜像
/// bit 级对齐（golden fixture 对拍）。HPF 置于固定增益之前：先削掉爆破音的
/// 20–150Hz 能量，避免 +12dB 把它推进 int16 饱和区。
/// HPF/AGC 状态跨 process 调用保持（按 640 样本帧流式处理），每次录音会话开始
/// 由 XiaomiAtvvSession 调 reset() 清零。
public final class PcmPostprocessor {
    /// 增益钳位范围（dB）。
    public static let maxGainDb = 24.0

    // 二阶 Butterworth 高通（transposed DF-II），fc≈120Hz @16kHz，Q=0.707：
    // 120Hz -3.0dB / 50Hz -15.3dB / 30Hz -24.1dB / ≥300Hz ~0dB（逐字拷贝固件）。
    // （2026-10-14 由 90Hz 上调至 120Hz：喷麦爆点能量集中在 100-150Hz。）
    private static let hpfB0 = 0.967227
    private static let hpfB1 = -1.934455
    private static let hpfB2 = 0.967227
    private static let hpfA1 = -1.933380
    private static let hpfA2 = 0.935529

    // 软件 AGC v2 常量（逐字拷贝固件；固件为 float，桌面端统一 double 保证
    // C++↔Python 对拍确定）。2026-10-15 耳语电平修复，方案 Doc/Plan/whisper-level-agc-v2.md：
    // 旧版「env < -45dBFS ⇒ desired = 1.0」把耳语前级包络（-42~-52dBFS）整段挡在门外
    // （真机耳语会话生效增益 ≈0dB），门限之上又被 +12dB 上限截断在 -33dBFS。
    // 新版把 -45dBFS 从「门限」改为「噪声预算」：噪声段只允许把本底抬到 -45dBFS，
    // 语音段放开到 +36dB；本底用冷启动窗估计，门限 = 本底 +12dB。
    private static let agcTarget = 8192.0            // -12 dBFS
    private static let agcMaxGain = 63.0             // +36 dB（由 +12dB 放开）
    private static let agcMinGain = 0.1              // -20 dB
    private static let agcNoiseBudget = 184.0        // -45 dBFS：噪声段被放大后的上限
    private static let agcEnvAttack = 0.98758        // 快攻 5ms
    private static let agcEnvRelease = 0.99979       // 慢释 300ms
    private static let agcGainUp = 0.99988           // 小误差慢上 500ms
    private static let agcGainUpFast = 0.9993752     // 大误差快上 100ms
    private static let agcGainDown = 0.73162         // 快下（τ≈0.2ms）
    private static let agcFastRatio = 4.0            // desired > 4×gain 走快攻
    private static let agcCeil = 26214.0             // 0.8FS 瞬时限幅
    private static let agcNfAbs = 16.0               // -66 dBFS：门限绝对下限
    private static let agcNfRatio = 4.0              // 门限 = 本底 +12 dB
    private static let agcNfNear = 2.0               // 仅 env < 2×nf 时缓升本底
    private static let agcNfRise = 6.25e-5           // 本底缓升 τ=1s
    private static let agcNfTrustMin = 1.0           // 本底低于此视为数字静音
    private static let agcNfMaxTrust = 184.0         // -45 dBFS：本底估计上限
    private static let agcNfWarmSamples = 1920       // 冷启动窗 120ms
    private static let agcNfSkipSamples = 80         // 跳过包络冷启动 5ms
    private static let agcNfWarmMaxSamples = 7680    // 冷启动窗封顶 480ms
    private static let agcNfSentinel = 1e30          // nf 初值（尚未观测）

    public private(set) var gainDb: Double
    /// HPF/AGC 开关（默认开）；关闭时该级直通（不动状态），用于逃生与测试隔离。
    public let hpfEnabled: Bool
    public let agcEnabled: Bool

    // HPF/AGC 滤波状态（会话开始时 reset() 清零，防跨会话泄漏）。
    private var hpfZ1 = 0.0
    private var hpfZ2 = 0.0
    private var agcEnv = 0.0
    private var agcGain = 1.0
    private var agcNf = PcmPostprocessor.agcNfSentinel
    private var agcIndex = 0
    private var agcNfReady = false
    private var agcNfSeen = false

    public init(gainDb: Double = 0.0, hpf: Bool = true, agc: Bool = true) {
        self.gainDb = Self.clampGainDb(gainDb)
        self.hpfEnabled = hpf
        self.agcEnabled = agc
    }

    public func setGainDb(_ gainDb: Double) {
        self.gainDb = Self.clampGainDb(gainDb)
    }

    /// 清零 HPF/AGC 状态（新录音会话开始时调用；幂等）。
    public func reset() {
        hpfZ1 = 0.0
        hpfZ2 = 0.0
        agcEnv = 0.0
        agcGain = 1.0
        agcNf = Self.agcNfSentinel
        agcIndex = 0
        agcNfReady = false
        agcNfSeen = false
    }

    public static func clampGainDb(_ gainDb: Double) -> Double {
        min(max(gainDb, -maxGainDb), maxGainDb)
    }

    /// 返回处理后的新数组；少于 3 个样本时跳过平滑只做增益。
    public func process(_ pcm: [Int16]) -> [Int16] {
        let gain = pow(10.0, gainDb / 20.0)
        var out = [Int16](repeating: 0, count: pcm.count)
        for i in 0..<pcm.count {
            // 三点平滑（首尾样本不动）：out[i]=(in[i-1]+2*in[i]+in[i+1])>>2。
            var smoothed = Int32(pcm[i])
            if pcm.count >= 3, i > 0, i + 1 < pcm.count {
                smoothed = (Int32(pcm[i - 1]) + 2 * Int32(pcm[i]) + Int32(pcm[i + 1])) >> 2
            }
            var v = Int(smoothed)
            if hpfEnabled { v = hpfProcess(v) }
            // lround 语义：rounded() 默认 .toNearestOrAwayFromZero。
            v = Int((Double(v) * gain).rounded())
            v = min(max(v, -32768), 32767)
            if agcEnabled { v = agcProcess(v) }
            out[i] = Int16(v)
        }
        return out
    }

    /// 二阶 Butterworth 高通（transposed DF-II），逐样本，double 状态。
    private func hpfProcess(_ x: Int) -> Int {
        let input = Double(x)
        var y = Self.hpfB0 * input + hpfZ1
        hpfZ1 = Self.hpfB1 * input - Self.hpfA1 * y + hpfZ2
        hpfZ2 = Self.hpfB2 * input - Self.hpfA2 * y
        // 软限幅防数值漂移越界（biquad 稳定但极端输入下保险）。
        if y > 32767.0 { y = 32767.0 } else if y < -32768.0 { y = -32768.0 }
        return Int(y.rounded()) // .toNearestOrAwayFromZero，对齐 C lround
    }

    /// 软件 AGC v2（逐样本，double 状态，跨 process 调用保持）：
    /// 噪声预算 + 本底自适应门限 + 大误差快攻 + 0.8FS 瞬时限幅（无记忆）。
    private func agcProcess(_ x: Int) -> Int {
        let ax = abs(Double(x))
        if agcIndex == 0 {
            // 包络冷启动：首样本直接取 |x|，避免爬升污染本底估计。
            agcEnv = ax
        } else {
            let ec = ax > agcEnv ? Self.agcEnvAttack : Self.agcEnvRelease
            agcEnv = ec * agcEnv + (1.0 - ec) * ax
        }

        if !agcNfReady {
            // 冷启动窗：只测本底；本底可信前直通（仅保留 0.8FS 限幅）。
            if agcIndex >= Self.agcNfSkipSamples {
                if agcEnv < agcNf { agcNf = agcEnv }
                if agcEnv >= Self.agcNfTrustMin { agcNfSeen = true }
            }
            if (agcIndex >= Self.agcNfWarmSamples && agcNfSeen && agcNf <= Self.agcNfMaxTrust)
                || agcIndex >= Self.agcNfWarmMaxSamples {
                var nf = agcNfSeen ? agcNf : Self.agcNfAbs
                if nf < Self.agcNfTrustMin { nf = Self.agcNfAbs }
                else if nf > Self.agcNfMaxTrust { nf = Self.agcNfMaxTrust }
                agcNf = nf
                agcNfReady = true
            }
            agcIndex += 1
            var g0 = 1.0
            if ax > 1.0 { g0 = min(g0, Self.agcCeil / ax) }
            var y0 = Double(x) * g0
            if y0 > 32767.0 { y0 = 32767.0 } else if y0 < -32768.0 { y0 = -32768.0 }
            return Int(y0.rounded())
        }

        if agcEnv < agcNf {
            agcNf = agcEnv
        } else if agcEnv < agcNf * Self.agcNfNear {
            agcNf += (agcEnv - agcNf) * Self.agcNfRise
        }
        if agcNf > Self.agcNfMaxTrust { agcNf = Self.agcNfMaxTrust }

        let gated = agcEnv < max(Self.agcNfAbs, agcNf * Self.agcNfRatio)
        var desired = min(max(Self.agcTarget / max(agcEnv, 1.0), Self.agcMinGain), Self.agcMaxGain)
        if gated {
            desired = min(desired, Self.agcNoiseBudget / max(agcEnv, 1.0))
        }
        let gc: Double
        if desired < agcGain { gc = Self.agcGainDown }
        else if desired > Self.agcFastRatio * agcGain { gc = Self.agcGainUpFast }
        else { gc = Self.agcGainUp }
        agcGain = gc * agcGain + (1.0 - gc) * desired
        var g = agcGain
        if ax > 1.0 { g = min(g, Self.agcCeil / ax) }
        var y = Double(x) * g
        if y > 32767.0 { y = 32767.0 } else if y < -32768.0 { y = -32768.0 }
        return Int(y.rounded())
    }
}

/// ADPCM 裸字节流跨包累积切帧器：ATVV audio notify 无帧头无序号，
/// 按 CAPS 协商帧长（默认 120 字节）累积，攒满一帧吐出一帧。
public final class FrameAccumulator {
    public private(set) var frameBytes: Int
    // 用 [UInt8] 而非 Data：Data.removeFirst 后 startIndex 偏移不归零，
    // 绝对下标 0..<n 会越界；Array 恒为 0 基索引（对齐 Windows ByteVector）。
    private var buffer: [UInt8] = []

    public init(frameBytes: Int = XiaomiAtvvProtocol.defaultFrameBytes) {
        self.frameBytes = frameBytes
        buffer.reserveCapacity(frameBytes)
    }

    /// 调整协商帧长并清空已累积字节（CAPS 到达时调用）。
    public func setFrameBytes(_ frameBytes: Int) {
        self.frameBytes = frameBytes
        reset()
    }

    /// 已累积、尚未凑满一帧的字节数。
    public var pendingBytes: Int { buffer.count }

    public func reset() {
        buffer.removeAll(keepingCapacity: true)
    }

    /// 追加字节，返回本次凑满的所有完整帧（可能为零或多帧）。
    public func append(_ data: Data) -> [Data] {
        var frames: [Data] = []
        guard frameBytes > 0 else { return frames }
        buffer.append(contentsOf: data)
        while buffer.count >= frameBytes {
            frames.append(Data(buffer[0..<frameBytes]))
            buffer.removeFirst(frameBytes)
        }
        return frames
    }
}
