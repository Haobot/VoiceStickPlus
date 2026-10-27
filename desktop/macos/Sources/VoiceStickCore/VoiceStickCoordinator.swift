import Foundation

public final class VoiceStickCoordinator {
    private enum PendingPasteState {
        case idle
        case waitingToPaste(text: String)
        case paused(text: String)

        var isIdle: Bool {
            if case .idle = self {
                return true
            }
            return false
        }
    }

    private enum MainInputState {
        case ready
        case recording(sessionID: UInt32, peripheralID: UUID, startedAt: Date)
        case finalizing(sessionID: UInt32, peripheralID: UUID, startedAt: Date?)
        case pendingConfirmation(peripheralID: UUID)
        case pausedConfirmation(peripheralID: UUID)
        case error(peripheralID: UUID?)

        var sessionID: UInt32? {
            switch self {
            case .recording(let sessionID, _, _), .finalizing(let sessionID, _, _):
                return sessionID
            case .ready, .pendingConfirmation, .pausedConfirmation, .error:
                return nil
            }
        }

        var peripheralID: UUID? {
            switch self {
            case .recording(_, let peripheralID, _),
                 .finalizing(_, let peripheralID, _),
                 .pendingConfirmation(let peripheralID),
                 .pausedConfirmation(let peripheralID):
                return peripheralID
            case .error(let peripheralID):
                return peripheralID
            case .ready:
                return nil
            }
        }

        var startedAt: Date? {
            switch self {
            case .recording(_, _, let startedAt):
                return startedAt
            case .finalizing(_, _, let startedAt):
                return startedAt
            case .ready, .pendingConfirmation, .pausedConfirmation, .error:
                return nil
            }
        }

        var isRecording: Bool {
            if case .recording = self {
                return true
            }
            return false
        }

        var isFinalizing: Bool {
            if case .finalizing = self {
                return true
            }
            return false
        }

        var isBusy: Bool {
            if case .ready = self {
                return false
            }
            return true
        }
    }

    private final class SubtitleCycle {
        let peripheralID: UUID
        let deviceID: String?
        let sessionID: UInt32
        let startedAt: Date
        let oggMuxer = OggOpusMuxer(sampleRate: 16_000, channels: 1)
        let debugAudioRecorder: any VoiceStickDebugRecordServing
        var asr: any ASRClient
        var receivedAudioFrames = 0
        var bufferedOggChunks: [Data] = []
        var asrStarted = false
        var sentFinalAudioChunk = false
        var finishedFinalText = false
        var waitingForAudioEnd = false
        var audioEndTimeoutTimer: Timer?

        init(peripheralID: UUID, deviceID: String?, sessionID: UInt32,
             config: AppConfig, asr: any ASRClient,
             makeDebugRecorder: @escaping (AppConfig) -> any VoiceStickDebugRecordServing) {
            self.peripheralID = peripheralID
            self.deviceID = deviceID
            self.sessionID = sessionID
            self.startedAt = Date()
            self.asr = asr
            self.debugAudioRecorder = makeDebugRecorder(config)
        }

        var duration: TimeInterval {
            Date().timeIntervalSince(startedAt)
        }

        deinit {
            audioEndTimeoutTimer?.invalidate()
        }
    }

    private struct SubtitleCycleKey: Hashable {
        let peripheralID: UUID
        let sessionID: UInt32
    }

    private var config: AppConfig
    // N1 切5：成员类型换协议（VoiceStickCore/StatusSink.swift），类向 Core 迁移第一步。
    private let statusController: VoiceStickStatusSink
    // N1 切5 第二步：BLE 成员换协议（VoiceStickCore/BleServing.swift），具体类外置注入。
    private let ble: any VoiceStickBleServing
    // N1 切5：ASR 工厂外置注入（工厂构造 App 实现留 App）。
    private let makeAsr: (AppConfig) -> any ASRClient
    // N1 切5 闸3/4：精修与翻译同样闭包注入（具体客户端留 App 合规）。
    private let makeRefiner: (AppConfig) -> any RefinerServing
    private let makeDebugRecorder: (AppConfig) -> any VoiceStickDebugRecordServing
    private let makeTranslator: (AppConfig) -> any TranslatorServing
    private var asr: any ASRClient
    private var translator: any TranslatorServing
    private var refiner: any RefinerServing
    /// 前台应用追踪（三期按键映射按前台应用切换的注入点；AppDelegate 启动时装配，
    /// v1 协调器暂不消费，app 级覆盖落地后由按键映射 resolve 使用）。
    // N1 切5 闸5：AppKit 原语闭包注入——协调器入 Core 前置（Core 不得 import AppKit）。
    public var openExternalURL: ((URL) -> Void)?
    public var presentUpgradeAlertUI: ((URL, String) -> Void)?
    public var isProcessTrusted: (() -> Bool)?
    // N1 闸6b：单点本地化键闭包（L10nKey 为 App 枚举，原样键串入闭包）。
    public var trText: ((String) -> String)?
    // ---- 会话历史（用户数据飞轮 P0-3，Typeless 方案 P0-1）----
    /// 会话历史存储工厂：AppDelegate 装配（系统 SQLite 打开闭包）；nil（默认，
    /// 测试不落盘）= 关闭记录。打开/迁移失败由工厂返回 nil，记录降级停用，
    /// 绝不阻塞识别主链路。
    public var makeSessionHistoryStore: ((URL) -> SessionHistoryStore?)?
    /// 用户数据文件目录覆盖（纠错对/热词候选两个 JSON 的落盘位置）：生产为 nil
    ///（真实 config 目录），FSM/预算类真实协调器测试必须注入会话级临时目录——
    /// 否则测试挖掘会污染真实用户数据，甚至让后续测试被精确命中改写对误拦截
    ///（2026-10-15 教训：FSM 精修用例把「长长长…」→「精修结果」写进真实
    /// correction_pairs.json，预算边界用例随后被精确命中跳过精修而转红）。
    public var userDataDirectoryOverride: URL?
    /// 焦点字段文本读取闭包（编辑追踪用，App 侧 AX 实现；nil=编辑追踪停用）。
    /// 隐私约束：仅用于与本次注入文本做区间 diff，读取内容不落库、不打日志。
    public var readFocusedFieldText: (() -> String?)?
    /// 焦点字段状态读取闭包（语音编辑 L1 校验用：字段全文 + 光标 UTF-16 偏移 +
    /// 选区长度；App 侧 AX 实现，nil=不可验证）。隐私约束同 readFocusedFieldText：
    /// 字段全文只在内存参与校验，不落库、不上传、不写日志内容。
    public var readFocusedFieldState: (() -> FocusedFieldState?)?
    /// targeted 编辑平台出口（App 侧 AX 实现）：设置焦点字段选区（UTF-16
    /// location+length，只写选区不写 Value），成功返回 true；nil/失败 = 平台能力
    /// 不可用时按不可验证拒绝（targeted 恒 verified，严禁盲改）。
    public var writeTargetedSelection: ((Int, Int) -> Bool)?
    /// targeted 编辑平台出口（App 侧 AX 实现）：读回当前实际选中文本；nil = 不可验证。
    /// 隐私约束：读回内容只与目标词逐字比对，不落库、不写日志。
    public var readTargetedSelectedText: (() -> String?)?
    /// 前台应用 bundle id 闭包（App 场景分域用，App 侧 NSWorkspace 实现；nil=不分工
    /// 桶，全部落全局桶）。只取 bundle id，不读窗口内容。
    public var frontmostAppBundleID: (() -> String?)?
    private var sessionHistoryStore: SessionHistoryStore?
    /// 最近一次落库的会话历史行 ID（编辑追踪回填锚点）。
    private var lastSessionHistoryID: String?
    /// 进行中会话的历史草稿（finishWithFinalText 建立、finishWithRefinedText 落库）。
    private struct PendingSessionHistory {
        let id: String
        let createdAt: Int64
        let deviceID: String?
        let asrProvider: String?
        let mode: String
        let rawAsr: String
        let duration: Double?
        let hotwordsSent: String?
    }
    private var pendingSessionHistory: PendingSessionHistory?
    /// 本会话精确命中应用的改写对（P3 反馈环锚点：确认保留 → recall+1，取消清空）。
    private var appliedCorrectionPair: CorrectionPair?
    /// 编辑追踪进行态（Typeless P0-2：注入后追踪用户修改；generation 防迟到回调）。
    private struct EditTrackingSession {
        let historyID: String
        let injectedText: String
        var attempts: Int
    }
    private var editTrackingSession: EditTrackingSession?
    private var editTrackingGeneration = 0
    /// 追踪窗口参数：8s 首读（给粘贴/应用切换留时间），之后每 10s 一读，最多 5 读（~48s）。
    private let kEditTrackingFirstDelay: TimeInterval = 8
    private let kEditTrackingRetryDelay: TimeInterval = 10
    private let kEditTrackingMaxAttempts = 5
    /// 语音编辑：最近一次文本注入记录（方案 §6.1）。只保留最近一次（新注入覆盖），
    /// 文本仅内存保存、不落库；remainingText 每成功删除一次后更新。
    private struct LastInjection {
        let text: String
        var remainingText: String
        let injectedAt: Date
        var pressedEnter: Bool
        let appID: String?
    }
    private var lastInjection: LastInjection?
    /// 语音编辑代际：新注入覆盖记录时 +1；targeted 异步收尾只认同代（防旧回调覆盖
    /// 新注入的记录）。
    private var voiceEditGeneration = 0
    /// targeted 编辑终验等待时长：给目标应用消化 一次 Backspace / 一次粘贴 的时间
    /// （InputInjector.paste 剪贴板恢复窗口同为 0.2s 量级）。
    private let kVoiceTargetedVerifyDelay: TimeInterval = 0.2
    /// 语音编辑退格注入串行队列（方案 §7.1：后台注入 + 约 12ms 节拍，不阻塞主线程）。
    private let voiceEditInjectionQueue = DispatchQueue(label: "com.voicestick.voice-edit.backspace")
    // N1 闸9：FSM 只读快照（runner 断言用；状态枚举保持 private 不外泄）。
    public var fsmSnapshot: String {
        "main=\(mainInputState) pendingPaste=\(pendingPasteState) "
            + "subtitleCycles=\(subtitleCycles.count)"
    }
    private let subtitleController: any VoiceStickSubtitleServing
    private let oggMuxer = OggOpusMuxer(sampleRate: 16_000, channels: 1)
    /// ASR 前 RNNoise 人声增强（config.audio_noise_suppression 开启时按会话创建；
    /// 仅主输入路径，字幕路径 PoC 阶段不覆盖）。
    private var audioEnhancer: AudioEnhancer?
    /// 增强失败回退透传的节流日志标记（每会话最多打一条）。
    private var audioEnhancerFallbackLogged = false
    private let inputInjector: any VoiceStickInputServing
    private let firmwareManifestClient = FirmwareManifestClient()
    private var debugAudioRecorder: any VoiceStickDebugRecordServing
    private let minimumRecordingDuration: TimeInterval = 0.5
    private let audioEndTimeout: TimeInterval = 1.0
    // ---- 精修预算与短文本跳过（B2/B3；常量先硬编码，是否提 config 字段留作后续）----
    /// 精修首 token 预算：到点仍无 token 判端点卡死，取消精修回退原文。
    private let kRefineFirstTokenBudget: TimeInterval = 2.5
    /// 精修总预算地板：短文本封顶 5s。
    private let kRefineTotalBudgetFloor: TimeInterval = 5.0
    /// 精修总预算按输入 utf8 字节伸缩的单价（25ms/字节，351B ≈ 8.8s）。
    private let kRefineTotalBudgetPerByte: TimeInterval = 0.025
    /// 短文本跳过精修的去空白字符阈值（字符数非字节；实测 12B 短句精修也要 1.2s+，
    /// 收益极低，直接粘贴原文）。
    private let kRefineMinChars = 12
    /// 录音中音频帧静默看门狗：录音态下超过该时长没有任何音频帧，视为设备侧
    /// 音频链路已死（2026-10-11 实证：tx 通道卡住后桌面端永远等不到 audio_end/
    /// button_up，会话悬挂 77s 直到 ASR 服务端报错）。到点即按 audio_end 收尾，
    /// 无帧时走 "No audio frames from device" 快速失败，不再无限等待。
    private let recordingInactivityTimeout: TimeInterval = 3.0
    private let firmwareManifestCacheDuration: TimeInterval = 24 * 60 * 60

    private var mainInputState = MainInputState.ready
    private var receivedAudioFrames = 0
    private var bufferedOggChunks: [Data] = []
    private var asrStarted = false
    private var sentFinalAudioChunk = false
    private var pastedFinalText = false
    private var waitingForAudioEnd = false
    private var audioEndTimeoutTimer: Timer?
    private var recordingInactivityTimer: Timer?
    private var firstAudioFrameAt: Date?
    private var lastAudioFrameAt: Date?
    private var pendingPasteState = PendingPasteState.idle
    private var lastRecoverableText: String?
    private var lastRecoverablePeripheralID: UUID?
    private var pairedDeviceIDs: [String]
    private var firmwareInfoByDeviceID: [String: DeviceFirmwareInfo] = [:]
    private var latestFirmwareManifest: FirmwareManifest?
    private var lastFirmwareManifestCheckAt: Date?
    private var firmwareManifestCheckInFlight = false
    private var firmwareManifestRefreshTimer: Timer?
    private var pendingFirmwareUpdatePromptDeviceIDs: Set<String> = []
    // A15：gateway_keymap 分片回执的未完成累计（seq0 起攒，more=false 收口）。
    private var keymapReportPending: [GatewayKeymapRoute] = []
    // 网关键长按连发（对齐 Windows gateway_repeater_）：gateway_key 按下沿首发 +
    // 登记，抬起沿清除，定时器 poll 到点按 ButtonsSettings 节拍重复注入。
    private var gatewayRepeater = RemoteButtonRepeater(timing: .init(
        delayMs: ButtonsSettings.defaultRepeatDelayMs,
        intervalMs: ButtonsSettings.defaultRepeatIntervalMs
    ))
    private var gatewayRepeatTimer: Timer?
    private var errorRecoveryToken = 0
    private var isShowingASRError = false
    private var subtitleCycles: [SubtitleCycleKey: SubtitleCycle] = [:]
    private var activeSubtitleSessions: [UUID: UInt32] = [:]

    // MARK: 编码器/敲击交互状态（对齐 Windows 协调器同名字段组）

    /// tap 注入节流锚点（两次方向键注入最短间隔 500ms）。
    private var lastTapInjectAt: Date?
    /// 编码器旋转快慢分档测速估计器（EWMA，全局单例；换设备时重置冷启动）。
    private var encoderSpeedEstimator = EncoderRotateSpeedEstimator()
    private var lastEncoderRotateDeviceID: String?
    /// 快速甩动注入一次后进入停转锁定，直到静默 250ms 停稳才恢复识别。
    private var encoderRotateLockout = false
    private var lastEncoderRotateEventAt: Date?
    /// 慢速旋转延迟判定挂起段（decide_window 内累计，到期/换向/加速冲刷）。
    private var encoderPendingActive = false
    private var encoderPendingCCW = false
    private var encoderPendingSteps: UInt32 = 0
    private var encoderPendingStartedAt = Date.distantPast
    private var encoderPendingPeripheralID: UUID?
    /// decide_window 到期冲刷定时器（pending 激活时 30ms 周期运行）。
    private var encoderRotateTimer: Timer?

    /// 进行中的流式精修取消令牌（finishWithFinalText 建立新流前先 cancel 旧的）。
    private var refinementCancelToken: RefineCancelToken?
    /// B2 精修预算定时器：首 token 预算（首个 onToken 到达即失效）+ 总预算（全程 armed）。
    /// 仅主线程创建/失效；触发回调二次校验令牌身份，悬挂定时器误触发为无害 no-op。
    private var refineFirstTokenTimer: Timer?
    private var refineTotalTimer: Timer?
    /// 热词使用统计（hotword_usage.json，懒加载；仅主线程读写）。
    private var hotwordUsageStore: HotwordUsageStore?

    // ---- 体感鼠标状态（对齐 Windows air_mouse_active_devices_/air_mouse_states_）----
    /// 每设备运动学状态（速度 v + 相对角度 theta + 最近 omega 采样与时间戳）。
    private struct AirMouseDeviceState {
        var kin = AirMouseKinState()
        var lastOmegaX: Double = 0
        var lastOmegaY: Double = 0
        var lastOmegaAt = Date()
        var thetaX: Double = 0
        var thetaY: Double = 0
    }
    private var airMouseActiveDevices: Set<String> = []
    private var airMouseStates: [String: AirMouseDeviceState] = [:]
    private var airMouseTimer: Timer?
    /// tick 周期 ~60Hz（对齐 Windows kAirMouseTickInterval=16ms，WM_TIMER 驱动）。
    private static let airMouseTickInterval: TimeInterval = 16.0 / 1000.0
    /// omega 超龄视为静止（固件 ~50Hz 上报；≥3× 帧周期容忍抖动与偶发丢帧，绝不误触发归零）。
    private static let airMouseOmegaStaleAge: TimeInterval = 0.080
    /// theta 积分上限（防异常累积）与 angle/omega 归零死区（对齐 Windows 常量）。
    private static let airMouseMaxTheta: Double = 100.0
    private static let airMouseAngleDeadzone: Double = 0.5
    private static let airMouseOmegaDeadzone: Double = 2.0

    public var onFirmwareUpdatePrompt: ((String, String, String, Bool) -> Void)?
    /// 注入路径发现无辅助功能权限（AppDelegate 接此回调弹引导窗；悬浮窗提示由协调器自带节流）。
    public var onAccessibilityPermissionMissing: (() -> Void)?
    /// power_log 分片 / power_mgmt 事件（deviceID 寻址；AppDelegate 转发到电量监测窗口）。
    public var onPowerLogFragment: ((String, PowerLogFragment) -> Void)?
    public var onPowerMgmtEvent: ((String, PowerMgmtEvent) -> Void)?

    public init(config: AppConfig, statusController: VoiceStickStatusSink,
         ble: any VoiceStickBleServing,
         makeAsr: @escaping (AppConfig) -> any ASRClient,
         makeTranslator: @escaping (AppConfig) -> any TranslatorServing,
         makeRefiner: @escaping (AppConfig) -> any RefinerServing,
         inputInjector: any VoiceStickInputServing,
         subtitleController: any VoiceStickSubtitleServing,
         makeDebugRecorder: @escaping (AppConfig) -> any VoiceStickDebugRecordServing) {
        self.config = config
        self.statusController = statusController
        self.pairedDeviceIDs = config.pairedDeviceIDs
        self.ble = ble
        self.makeAsr = makeAsr
        self.makeTranslator = makeTranslator
        self.makeRefiner = makeRefiner
        self.inputInjector = inputInjector
        self.subtitleController = subtitleController
        self.makeDebugRecorder = makeDebugRecorder
        self.asr = makeAsr(config)
        self.translator = makeTranslator(config)
        self.refiner = makeRefiner(config)
        self.debugAudioRecorder = makeDebugRecorder(config)
        // 小米遥控器接入：paired_device 条目表注入（实时读最新 config，配对/遗忘
        // 即时生效）；同款注入方式见 BleCentral.pairedDevicesProvider 注释。
        ble.pairedDevicesProvider = { [weak self] in self?.config.pairedDevices ?? [] }
        // 注入权限兜底：无辅助功能权限时 CGEvent 被静默丢弃，转悬浮窗提示 + 引导弹窗。
        inputInjector.onAccessibilityPermissionMissing = { [weak self] in
            self?.handleAccessibilityPermissionMissing()
        }
    }

    /// 透传：按 RC deviceID 解析 ATVV 会话参数（见 BleCentral.xiaomiOptionsResolver）。
    public func setXiaomiOptionsResolver(_ resolver: @escaping (String) -> XiaomiAtvvSession.Options) {
        ble.xiaomiOptionsResolver = resolver
    }

    /// 透传：F5 抑制锚点（AppDelegate 事件钩子读取；写入在 BleCentral 回调线程）。
    public var xiaomiMicOpenAnchor: XiaomiMicOpenAnchor {
        ble.micOpenAnchor
    }

    /// 透传：BLE 连接集变化（AppDelegate 的 F5 suppressor 门控挂在上面）。
    public var onConnectionChange: (([ConnectedVoiceStickDevice]) -> Void)?

    public func start() {
        ble.onConnectionChange = { [weak self] connectedDevices in
            guard let self else { return }
            self.statusController.setConnectedDevices(connectedDevices)
            self.cancelActiveCycleIfDeviceDisconnected()
            self.refreshFirmwareAvailability()
            if !connectedDevices.isEmpty {
                self.statusController.setStatus("Ready")
                self.ble.sendInteractionMode(self.config.interactionMode)
                self.ble.sendShowIMUDebug(self.config.showIMUDebug)
                // 设备交互/编码器设置按设备覆盖：逐台取其有效配置单播（无覆盖设备收到
                // 全局默认值，与旧广播行为等价）。小米遥控器无 IMU/敲击/编码器硬件，跳过
                // （对齐 Windows 连接回调；BLE 层另有按类门控兜底）。
                for device in connectedDevices where device.deviceClass == .stickS3 {
                    self.sendDeviceInteractionSettings(deviceID: device.deviceID)
                    // 网关模式（P2）：就绪即上报主机名（P1 目标表命名）+ 推送按键路由
                    //（幂等重发，配置漂移自愈；对齐 Windows PublishConnections 路径）。
                    self.pushGatewayConfig(deviceID: device.deviceID)
                }
            } else {
                self.statusController.setStatus(self.pairedDeviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
            }
            self.onConnectionChange?(connectedDevices)
        }

        // 控制特征就绪补推（网关 P1 闭环的送达保证）：didConnect 时刻 onConnectionChange
        // 里的交互设置/网关下发会因 controlCharacteristics 未就绪全部被丢弃（真机日志
        // gateway_keymap_set 182/182 skipped、就绪后无重发 ⇒ 固件永远收不到本机路由表
        // 与目标名）。就绪回调只在 StickS3 控制特征分支触发，幂等重发交互设置 + 网关
        // 目标名/路由表；与 onConnectionChange 路径重叠时多发一轮亦无害。
        ble.onControlChannelReady = { [weak self] deviceID in
            guard let self else { return }
            self.sendDeviceInteractionSettings(deviceID: deviceID)
            self.pushGatewayConfig(deviceID: deviceID)
        }

        ble.onStateEvent = { [weak self] peripheralID, event in
            self?.handleStateEvent(event, peripheralID: peripheralID)
        }

        // power_log 分片与 power_mgmt 事件透传（电量监测窗口经 AppDelegate 消费；
        // 窗口未开时丢弃，对齐 Windows）。
        ble.onPowerLogFragment = { [weak self] peripheralID, fragment in
            guard let self, let deviceID = self.deviceID(for: peripheralID) else { return }
            self.onPowerLogFragment?(deviceID, fragment)
        }
        ble.onPowerMgmtEvent = { [weak self] peripheralID, event in
            guard let self, let deviceID = self.deviceID(for: peripheralID) else { return }
            self.onPowerMgmtEvent?(deviceID, event)
        }

        ble.onAudioFrame = { [weak self] peripheralID, frame in
            self?.handleAudioFrame(frame, peripheralID: peripheralID)
        }

        // 体感鼠标 motion 帧（state_tx 0x11，~50Hz）：仅更新 omega/积分 theta，
        // 光标位移由 airMouseTick 统一驱动（对齐 Windows HandleMotionEvent/AirMouseTick 分工）。
        ble.onMotionFrame = { [weak self] peripheralID, frame in
            self?.handleMotionFrame(frame, peripheralID: peripheralID)
        }

        configureASRCallbacks()
        ble.start()
        checkFirmwareUpdatesIfNeeded(force: false, showErrors: false)
        startFirmwareManifestRefreshTimer()
    }

    deinit {
        audioEndTimeoutTimer?.invalidate()
        firmwareManifestRefreshTimer?.invalidate()
        encoderRotateTimer?.invalidate()
        airMouseTimer?.invalidate()
        cancelRefineBudgetTimers()
    }

    public func updateConfig(_ config: AppConfig) {
        let wasRecognizing = asrStarted || mainInputState.isBusy || isWaitingForFinalText || !subtitleCycles.isEmpty
        if wasRecognizing {
            asr.onPartial = nil
            asr.onSegment = nil
            asr.onFinal = nil
            asr.onError = nil
            asr.onUpgradeURL = nil
            asr.cancel()
            cancelStreamingRefinement()
            for cycle in subtitleCycles.values {
                cycle.asr.cancel()
                cycle.debugAudioRecorder.discard()
            }
            subtitleCycles.removeAll()
            activeSubtitleSessions.removeAll()
            mainInputState = .ready
            pendingPasteState = .idle
            debugAudioRecorder.discard()
            statusController.hideOverlay()
            subtitleController.hideAll()
            sendUIStateForActiveDevice("ready")
            finishRecognitionCycle()
        }

        self.config = config
        // 语音编辑记录清理（方案 §6.1）：关闭功能或当前输出目标不是 focused_app 时
        // 旧注入记录不再有安全作用域，清掉避免误删。清理同时递增代际：在途 targeted
        // 终验回调（0.2s 窗口）只认同代，避免对已被配置清掉的记录弹旧成功 toast。
        if !config.voiceEdit.enabled || outputProfile(for: activeDeviceID).target != .focusedApp {
            lastInjection = nil
            voiceEditGeneration += 1
        }
        // 网关键连发节拍热更（按键映射对话框保存路径）。
        gatewayRepeater.setTiming(.init(
            delayMs: config.buttonsSettings(for: activeXiaomiDeviceID).repeatDelayMs,
            intervalMs: config.buttonsSettings(for: activeXiaomiDeviceID).repeatIntervalMs
        ))
        ble.sendInteractionMode(config.interactionMode)
        ble.sendShowIMUDebug(config.showIMUDebug)
        // 编码器/交互设置按设备覆盖：对已连接 StickS3 逐台取其有效配置单播
        // （对齐 Windows UpdateConfig；小米遥控器跳过）。
        for deviceID in ble.connectedStickDeviceIDs() {
            sendDeviceInteractionSettings(deviceID: deviceID)
            // 按键映射配置变化 → 重发网关路由（主机名不变不重报；对齐 Windows
            // OnSettingsChanged 重发路径）。
            pushGatewayKeymapRoutes(deviceID: deviceID)
        }
        debugAudioRecorder = makeDebugRecorder(config)
        asr = makeAsr(config)
        translator = makeTranslator(config)
        refiner = makeRefiner(config)
        configureASRCallbacks()

        if pairedDeviceIDs != config.pairedDeviceIDs {
            updatePairedDeviceIDs(config.pairedDeviceIDs)
        } else if isShowingASRError {
            recoverFromASRError()
        } else if wasRecognizing {
            statusController.setStatus("Ready")
        }
    }

    private func configureASRCallbacks() {
        asr.onPartial = { [weak self] text in
            DispatchQueue.main.async {
                guard let self else { return }
                self.statusController.showPartial(text, deviceID: self.activeDeviceID)
                if self.shouldSendPartialToDevice() {
                    self.sendUIStateForActiveDevice("thinking", text: text)
                }
            }
        }

        asr.onSegment = { [weak self] segment in
            DispatchQueue.main.async {
                self?.handleDefiniteSegment(segment)
            }
        }

        asr.onFinal = { [weak self] text in
            DispatchQueue.main.async {
                self?.finishWithFinalText(text)
            }
        }

        asr.onError = { [weak self] message in
            DispatchQueue.main.async {
                self?.finishWithASRError(message)
            }
        }

        asr.onUpgradeURL = { [weak self] url, message in
            DispatchQueue.main.async {
                self?.presentASRUpgradeAlert(url: url, message: message)
            }
        }
    }

    private func configureSubtitleASRCallbacks(for cycle: SubtitleCycle) {
        let peripheralID = cycle.peripheralID
        let sessionID = cycle.sessionID
        cycle.asr.onPartial = { [weak self] text in
            DispatchQueue.main.async {
                guard
                    let self,
                    let cycle = self.subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
                    self.canUpdateOverlayForSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID)
                else { return }
                self.statusController.showPartial(text, deviceID: cycle.deviceID)
                if self.shouldSendSubtitlePartialToDevice(cycle) {
                    self.ble.sendUIState("thinking", text: text, to: peripheralID)
                }
            }
        }
        cycle.asr.onSegment = { [weak self] segment in
            DispatchQueue.main.async {
                guard self?.isActiveSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID) == true else {
                    return
                }
                self?.handleSubtitleDefiniteSegment(segment, peripheralID: peripheralID)
            }
        }
        cycle.asr.onFinal = { [weak self] text in
            DispatchQueue.main.async {
                self?.finishSubtitleCycleWithFinalText(
                    peripheralID: peripheralID,
                    sessionID: sessionID,
                    text: text
                )
            }
        }
        cycle.asr.onError = { [weak self] message in
            DispatchQueue.main.async {
                self?.finishSubtitleCycleWithError(
                    peripheralID: peripheralID,
                    sessionID: sessionID,
                    message: message
                )
            }
        }
        cycle.asr.onUpgradeURL = { [weak self] url, _ in
            DispatchQueue.main.async {
                self?.openExternalURL?(url)
            }
        }
    }

    public func updatePairedDeviceIDs(_ deviceIDs: [String]) {
        pairedDeviceIDs = deviceIDs
        // 配对/遗忘由 AppDelegate 先落盘；此处从磁盘刷新配对条目，
        // 否则 ble.pairedDevicesProvider 读到的是启动时的旧快照，
        // RC 设备（靠 paired_device 条目反查 UUID 重连）在重启前永远连不上。
        config.pairedDevices = AppConfig.load().pairedDevices
        // forget 后设备会断开重连：清理残留体感态，避免拦截重连后的主键录音（对齐 Windows）。
        clearAirMouseForInactiveDevices(reason: "forget")
        statusController.setPairedDeviceIDs(deviceIDs)
        statusController.setConnectedDevices([])
        statusController.setStatus(deviceIDs.isEmpty ? "Pair a VoiceStick" : "Ready")
        ble.updatePairedDeviceIDs(deviceIDs)
    }

    public func updateFirmware(from url: URL, for deviceID: String,
                        progress: @escaping (FirmwareUpdateProgress) -> Void,
                        completion: @escaping (Result<Void, Error>) -> Void) {
        do {
            let image = try Data(contentsOf: url)
            ble.updateFirmware(image: image, for: deviceID, progress: progress) { result in
                DispatchQueue.main.async {
                    completion(result)
                }
            }
        } catch {
            completion(.failure(error))
        }
    }

    public func cancelFirmwareUpdate() {
        ble.cancelFirmwareUpdate()
    }

    /// power_log 命令下发（电量监测窗口；对齐 Windows VoiceStickCoordinator::SendPowerLogCommand）。
    public func sendPowerLogCommand(deviceID: String, payload: Data) {
        ble.sendPowerLogCommand(payload, to: deviceID)
    }

    public func checkFirmwareUpdatesNow() {
        checkFirmwareUpdatesIfNeeded(force: true, showErrors: true)
    }

    public func checkFirmwareAfterPairing(deviceID: String) {
        pendingFirmwareUpdatePromptDeviceIDs.insert(deviceID)
        checkFirmwareUpdatesIfNeeded(force: true, showErrors: false)
        refreshFirmwareAvailability()
    }

    public func updateFirmwareFromLatest(for deviceID: String,
                                  progress: @escaping (FirmwareUpdateProgress) -> Void,
                                  completion: @escaping (Result<Void, Error>) -> Void) {
        guard let manifest = latestFirmwareManifest else {
            completion(.failure(FirmwareManifestClient.FirmwareManifestError.invalidResponse))
            return
        }

        firmwareManifestClient.downloadOTA(from: manifest) { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                switch result {
                case .success(let image):
                    self.ble.updateFirmware(image: image, for: deviceID, progress: progress) { result in
                        DispatchQueue.main.async {
                            completion(result)
                        }
                    }
                case .failure(let error):
                    completion(.failure(error))
                }
            }
        }
    }

    private func handleStateEvent(_ event: StateEvent, peripheralID: UUID) {
        switch event.event {
        case "device_info":
            if let hardware = event.hardware, let firmwareVersion = event.firmwareVersion {
                NSLog("Connected VoiceStick hardware=\(hardware) firmware=\(firmwareVersion)")
            }
            updateDeviceFirmwareInfo(event: event, peripheralID: peripheralID)
        case "encoder_status":
            if let present = event.encoderPresent, let deviceID = deviceID(for: peripheralID) {
                statusController.setDeviceEncoderPresent(deviceID, present: present)
            }
        case "battery_status":
            if let level = event.batteryLevel, let deviceID = deviceID(for: peripheralID) {
                statusController.setDeviceBattery(
                    deviceID,
                    level: level,
                    charging: event.batteryCharging ?? false,
                    usbPowered: event.batteryUsbPowered ?? false
                )
            }
        case "button_down":
            if event.button == "primary" && event.source == "encoder" {
                handleEncoderButtonDown(event, peripheralID: peripheralID)
            } else {
                handleButtonDown(event, peripheralID: peripheralID)
            }
        case "button_up":
            if event.button == "primary" && event.source == "encoder" {
                handleEncoderButtonUp(event, peripheralID: peripheralID)
            } else {
                handleButtonUp(event, peripheralID: peripheralID)
            }
        case "button_click":
            if event.button == "primary" && event.source == "encoder" {
                handleEncoderButtonClick(event, peripheralID: peripheralID)
            } else {
                handleButtonClick(event, peripheralID: peripheralID)
            }
        case "button_double_click":
            if event.button == "primary" && event.source == "encoder" {
                handleEncoderButtonDoubleClick(event, peripheralID: peripheralID)
            } else {
                handleButtonDoubleClick(event, peripheralID: peripheralID)
            }
        case "tap":
            handleTapEvent(event, peripheralID: peripheralID)
        case "encoder_rotate":
            handleEncoderRotate(event, peripheralID: peripheralID)
        case "gateway_status":
            // 网关模式小帧（信息性）。Windows 用它抑制「直连遥控器 ATVV」；macOS
            // 无直连 ATVV 路径，仅记日志。老固件不发此事件，缺席即未知。
            if let mode = event.gatewayMode {
                NSLog("gateway status dev=\(deviceID(for: peripheralID) ?? "unknown") mode=\(mode)")
            }
        case "gateway_key":
            handleGatewayKey(event)
        case "proto_info":
            // D9：设备协议版本——出界告警（把「未来升级静默不兼容」变成日志可见）。
            let proto = event.proto ?? -1
            let minProto = event.minProto ?? -1
            if proto < BleProtocol.kProtocolMinVersion || proto > BleProtocol.kProtocolVersion {
                NSLog("WARNING: device proto=\(proto) min=\(minProto) out of supported range [\(BleProtocol.kProtocolMinVersion),\(BleProtocol.kProtocolVersion)]")
            } else {
                NSLog("device proto=\(proto) min=\(minProto) (supported)")
            }
        case "gateway_keymap":
            // A15：分片回执（seq/more，protocol.md）——攒到 more=false 才当完整表。
            if event.accumulateKeymap(into: &keymapReportPending) {
                let routes = keymapReportPending.map { "\($0.key)=\($0.route)" }
                    .joined(separator: ", ")
                NSLog("gateway keymap report (\(keymapReportPending.count) keys): " + routes)
                keymapReportPending.removeAll()
            }
        default:
            break
        }
    }

    private func handleButtonDoubleClick(_ event: StateEvent, peripheralID: UUID) {
        guard event.button == "primary" else { return }
        NSLog("Double-click detected on VS-\(deviceID(for: peripheralID) ?? "unknown"), sending Enter")

        cancelActiveSessionsForDoubleClick(peripheralID: peripheralID)

        injectDoubleClickAction(peripheralID: peripheralID)

        // 回到就绪状态。
        ble.sendUIState("ready", to: peripheralID)
        mainInputState = .ready
        statusController.setStatus("Ready")
    }

    /// 双击注入动作：StickS3 主键双击恒为 Enter（核心交互）；小米语音键双击（A 级）
    /// 按按键映射处置——原生=Enter / 禁用=不注入 / 按键=KeySpec 注入，解析失败回落
    /// Enter（对齐 HID 拦截层语义）。
    private func injectDoubleClickAction(peripheralID: UUID) {
        guard ble.deviceClass(for: peripheralID) == .xiaomiRemote2Pro,
              let deviceID = deviceID(for: peripheralID) else {
            inputInjector.sendEnter()
            return
        }
        let mapping = config.buttonsSettings(for: deviceID).mapping(for: .voiceDoubleClick)
        switch mapping.action {
        case .native:
            inputInjector.sendEnter()
        case .disabled:
            NSLog("xiaomi voice double-click disabled by mapping on \(deviceID)")
        case .key:
            guard let spec = KeySpec.parse(mapping.key) else {
                NSLog("xiaomi voice double-click key invalid: \"\(mapping.key)\" on \(deviceID), fallback to Enter")
                inputInjector.sendEnter()
                return
            }
            inputInjector.sendKeyCombo(spec)
        }
    }

    // ---- 网关模式（P2，Doc/Plan/xiaomi-gateway-p2-macos.md）----

    /// 本机「电脑名称」（对齐 Windows ComputerName 语义）：localizedName 优先，
    /// 回退 hostname。规整与 23 字节截断在 GatewaySupport.targetInfoName。
    private static var localHostName: String {
        Host.current().localizedName ?? ProcessInfo.processInfo.hostName
    }

    /// 活跃小米遥控器（首个配对 RC 条目，对齐 Windows active_rc 取法）：路由下发与
    /// gateway_key 消费共用其按键映射；无配对 RC 时回落全局默认。
    private var activeXiaomiDeviceID: String? {
        config.pairedDevices.first {
            $0.hardware == PairedDeviceEntry.hardwareXiaomiRemote2Pro
        }?.deviceID
    }

    /// 连接就绪即上报主机名 + 推送按键路由（幂等）。
    private func pushGatewayConfig(deviceID: String) {
        if let name = GatewaySupport.targetInfoName(Self.localHostName) {
            ble.sendStickControlPayload(
                BleProtocol.gatewayTargetInfoPayload(name: name),
                label: "gateway_target_info '\(name)'", deviceID: deviceID
            )
        } else {
            NSLog("gateway target info skipped (empty host name) dev=VS-\(deviceID)")
        }
        pushGatewayKeymapRoutes(deviceID: deviceID)
    }

    /// 按本机按键映射逐键下发路由（macOS 语义见 GatewaySupport.routes：可映射键
    /// 一律 software——原生=桌面自然键注入；power native → passthrough、有映射 →
    /// software；volume_mute → passthrough。key_map 是固件 NVS 全局态，两端各自
    /// 连接时重发 = 当前活跃目标的配置生效）。
    private func pushGatewayKeymapRoutes(deviceID: String) {
        let settings = config.buttonsSettings(for: activeXiaomiDeviceID)
        for route in GatewaySupport.routes(for: settings) {
            ble.sendStickControlPayload(
                BleProtocol.gatewayKeymapSetPayload(key: route.key, route: route.route),
                label: "gateway_keymap_set \(route.key)=\(route.route)", deviceID: deviceID
            )
        }
    }

    /// 网关软件路由键消费（对齐 Windows OnGatewayKey）：按下沿首发注入 + 登记连发，
    /// 抬起沿清除连发；重复注入由连发定时器按 ButtonsSettings 节拍驱动（对齐 Windows
    /// gateway_repeater_）。映射取活跃 RC 的配置——与路由下发同源，保证「路由为
    /// software 的键必能在此被消费」。禁用键吞掉；native 按 naturalSpec 注入等价自然键
    /// （2026-10-12：macOS 系统级 HID 会话不可用，原生语义由桌面实现，见
    /// GatewaySupport.routes 语义修正）；power 已可映射（2026-10-19），native 的
    /// power 路由为 passthrough 不会到达这里；仅 volume_mute（桌面无映射面）忽略防误注入。
    private func handleGatewayKey(_ event: StateEvent) {
        guard let keyName = event.gatewayKey else { return }
        guard let button = RemoteButton(rawValue: keyName) else {
            NSLog("gateway key \(keyName) not mappable on macOS, ignored")
            return
        }
        if event.gatewayPressed == true {
            handleGatewayKeyDown(button)
        } else {
            if gatewayRepeater.release(button) {
                NSLog("gateway key \(keyName) repeat stop")
            }
            syncGatewayRepeatTimer()
        }
    }

    /// 网关键按下沿：解析映射（key=KeySpec / native=naturalSpec），首发注入并登记
    /// 连发 hold；禁用或解析失败不登记（无连发）。
    private func handleGatewayKeyDown(_ button: RemoteButton) {
        let mapping = config.buttonsSettings(for: activeXiaomiDeviceID).mapping(for: button)
        guard let spec = gatewaySpec(for: button, mapping: mapping) else {
            if mapping.action == .disabled {
                NSLog("gateway key \(button.rawValue) disabled by mapping, swallowed")
            } else {
                NSLog("gateway key \(button.rawValue) invalid key spec \"\(mapping.key)\", ignored")
            }
            return
        }
        NSLog("gateway key \(button.rawValue) -> inject \(spec.displayText)")
        inputInjector.sendKeyCombo(spec)
        if gatewayRepeater.press(button, atMs: XiaomiMicOpenAnchor.nowMs()) {
            NSLog("gateway key \(button.rawValue) repeat armed " +
                  "delay=\(config.buttonsSettings(for: activeXiaomiDeviceID).repeatDelayMs)ms " +
                  "interval=\(config.buttonsSettings(for: activeXiaomiDeviceID).repeatIntervalMs)ms")
            syncGatewayRepeatTimer()
        }
    }

    /// 映射 → 注入规格：key 动作解析 KeySpec（失败 nil），native 动作取桌面等价
    /// 自然键（无等价语义的键如 menu/tv/power 返回 nil），disabled 一律 nil。
    private func gatewaySpec(for button: RemoteButton, mapping: ButtonMapping) -> KeySpec? {
        switch mapping.action {
        case .key:
            return KeySpec.parse(mapping.key)
        case .native:
            return GatewaySupport.naturalSpec(for: button)
        case .disabled:
            return nil
        }
    }

    // MARK: - 网关键长按连发定时器

    /// hold 存在性 ↔ 连发定时器开关联动（30ms 粒度；到期判定走绝对 deadline）。
    private func syncGatewayRepeatTimer() {
        if gatewayRepeater.hasHolds {
            guard gatewayRepeatTimer == nil else { return }
            let timer = Timer(timeInterval: 0.03, repeats: true) { [weak self] _ in
                self?.fireGatewayRepeat()
            }
            RunLoop.main.add(timer, forMode: .common)
            gatewayRepeatTimer = timer
        } else if gatewayRepeatTimer != nil {
            gatewayRepeatTimer?.invalidate()
            gatewayRepeatTimer = nil
        }
    }

    /// 连发轮询：到期按钮按当前映射重新解析注入（热改映射即刻生效；改失配则
    /// 撤销该 hold 不再连发）。
    private func fireGatewayRepeat() {
        for button in gatewayRepeater.poll(atMs: XiaomiMicOpenAnchor.nowMs()) {
            let mapping = config.buttonsSettings(for: activeXiaomiDeviceID).mapping(for: button)
            guard let spec = gatewaySpec(for: button, mapping: mapping) else {
                NSLog("gateway key \(button.rawValue) repeat drop (mapping changed)")
                gatewayRepeater.release(button)
                continue
            }
            NSLog("gateway key \(button.rawValue) repeat fire -> \(spec.displayText)")
            inputInjector.sendKeyCombo(spec)
        }
        syncGatewayRepeatTimer()
    }

    /// 双击取消结构（对齐 Windows CancelActiveSessionsForDoubleClick）：取消当前活跃
    /// 录音 + 该设备的字幕会话。物理主键双击与编码器双击（key 动作）共用。
    private func cancelActiveSessionsForDoubleClick(peripheralID: UUID) {
        // 取消当前活跃录音（如果有）。
        if case .recording(_, let recordingPeripheralID, _) = mainInputState,
           recordingPeripheralID == peripheralID {
            cancelRecognitionInProgress()
        }
        // 取消字幕会话。
        cancelSubtitleCycles(peripheralID: peripheralID, reason: "double_click")
    }

    private func handleButtonDown(_ event: StateEvent, peripheralID: UUID) {
        NSLog("Button down button=\(event.button ?? "nil") dev=VS-\(deviceID(for: peripheralID) ?? "unknown") session=\(event.sessionID.map(String.init) ?? "nil")")
        switch event.button {
        case "primary":
            handlePrimaryButtonDown(sessionID: event.sessionID, peripheralID: peripheralID)
        case "secondary":
            break
        default:
            break
        }
    }

    private func handleButtonUp(_ event: StateEvent, peripheralID: UUID) {
        NSLog("Button up button=\(event.button ?? "nil") dev=VS-\(deviceID(for: peripheralID) ?? "unknown") session=\(event.sessionID.map(String.init) ?? "nil") duration_ms=\(event.durationMs.map(String.init) ?? "nil")")
        switch event.button {
        case "primary":
            handlePrimaryButtonUp(peripheralID: peripheralID)
        case "secondary":
            handleSecondaryButtonClick(peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handleButtonClick(_ event: StateEvent, peripheralID: UUID) {
        NSLog("Button click button=\(event.button ?? "nil") dev=VS-\(deviceID(for: peripheralID) ?? "unknown") session=\(event.sessionID.map(String.init) ?? "nil") duration_ms=\(event.durationMs.map(String.init) ?? "nil")")
        switch event.button {
        case "primary":
            // 体感态：主键单击映射为鼠标左键，不走录音/字幕逻辑（对齐 Windows）。
            if isAirMouseActive(deviceID: deviceID(for: peripheralID)) {
                NSLog("air mouse primary click on VS-\(deviceID(for: peripheralID) ?? "unknown"), left button")
                inputInjector.clickLeftButton()
                return
            }
            if handleFrontButtonDuringPendingPaste(peripheralID: peripheralID) {
                return
            }
            guard config.interactionMode == .clickToTalk else {
                ble.sendUIState("ready", to: peripheralID)
                return
            }
            // click_to_talk 的启动 click 发 duration_ms=0、停止 click 发 >0（均带
            // session_id）。仅靠本地状态消歧会在失步后（识别中启动 click 被忽略、
            // 固件仍在录音）把停止 click 误判为启动，产生永远收不到音频的幽灵会话。
            // 缺失 duration_ms（旧固件）时回退原状态判定（对齐 Windows HandleButtonClick）。
            if let durationMs = event.durationMs {
                if durationMs > 0 {
                    // 停止 click：仅在匹配的活跃录音会话上生效，否则按失步残留忽略。
                    if case .recording(let sessionID, let recordingPeripheralID, _) = mainInputState,
                       recordingPeripheralID == peripheralID,
                       let eventSessionID = event.sessionID,
                       eventSessionID == sessionID {
                        handlePrimaryButtonUp(peripheralID: peripheralID)
                    } else {
                        NSLog("click_to_talk stale stop click ignored dev=VS-\(deviceID(for: peripheralID) ?? "unknown")")
                    }
                    return
                }
                // 启动 click（duration_ms==0）：finalizing 期间维持忽略，其余交
                // handlePrimaryButtonDown（含 recording 残留自愈）。
                if case .finalizing(_, let finalizingPeripheralID, _) = mainInputState,
                   finalizingPeripheralID == peripheralID {
                    NSLog("Ignoring primary button click while recording is finalizing")
                    ble.sendUIState("thinking", to: peripheralID)
                    return
                }
                handlePrimaryButtonDown(sessionID: event.sessionID, peripheralID: peripheralID)
                return
            }
            // 旧固件无 duration_ms：回退本地状态判定。
            if case .recording(_, let recordingPeripheralID, _) = mainInputState,
               recordingPeripheralID == peripheralID {
                handlePrimaryButtonUp(peripheralID: peripheralID)
            } else if case .finalizing(_, let finalizingPeripheralID, _) = mainInputState,
                      finalizingPeripheralID == peripheralID {
                NSLog("Ignoring primary button click while recording is finalizing")
                ble.sendUIState("thinking", to: peripheralID)
            } else {
                handlePrimaryButtonDown(sessionID: event.sessionID, peripheralID: peripheralID)
            }
        case "secondary":
            handleSecondaryButtonClick(peripheralID: peripheralID)
        default:
            break
        }
    }

    private func handleSecondaryButtonClick(peripheralID: UUID) {
        // 体感态已开：侧键单击退出体感（优先于其它语义，对齐 Windows）。
        if let deviceID = deviceID(for: peripheralID), isAirMouseActive(deviceID: deviceID) {
            _ = toggleAirMouse(deviceID: deviceID)
            return
        }
        if activeSubtitleSessions[peripheralID] != nil {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "secondary_cancel")
            return
        }
        if subtitleCycles.values.contains(where: { $0.peripheralID == peripheralID }) {
            cancelSubtitleCycles(peripheralID: peripheralID, reason: "secondary_cancel")
            return
        }
        cancelPendingPaste(peripheralID: peripheralID)
    }

    // MARK: - 设备交互/编码器设置下发（对齐 Windows 连接回调与 UpdateConfig 的逐台单播）

    /// 对单台 StickS3 下发交互 + 编码器设置（5 项）。调用方保证 deviceID 为 StickS3；
    /// BLE 层 sendStickControlPayload 另有按类门控兜底。
    private func sendDeviceInteractionSettings(deviceID: String) {
        let inter = config.interactionSettings(for: deviceID)
        ble.sendStickControlPayload(
            BleProtocol.tapEnabledPayload(enabled: inter.tapToArrow),
            label: "tap_enabled", deviceID: deviceID
        )
        ble.sendStickControlPayload(
            BleProtocol.tapSensitivityPayload(level: inter.tapSensitivity),
            label: "tap_sensitivity", deviceID: deviceID
        )
        ble.sendStickControlPayload(
            BleProtocol.imuWakeSensitivityPayload(thresholdLsb: inter.imuWakeSensitivity.thresholdLsb),
            label: "imu_wake_sensitivity", deviceID: deviceID
        )
        let enc = config.encoderSettings(for: deviceID)
        ble.sendStickControlPayload(
            BleProtocol.encoderLedColorPayload(color: enc.ledColor.rawValue),
            label: "encoder_led_color", deviceID: deviceID
        )
        ble.sendStickControlPayload(
            BleProtocol.encoderRecordingGatePayload(enabled: enc.pressAction == .recording),
            label: "encoder_recording_gate", deviceID: deviceID
        )
    }

    // MARK: - 体感鼠标（对齐 Windows ToggleAirMouse/HandleMotionEvent/AirMouseTick）

    public func isAirMouseActive(deviceID: String?) -> Bool {
        guard let deviceID else { return false }
        return airMouseActiveDevices.contains(deviceID)
    }

    /// 切换指定设备的体感态。进入：通知固件校准零偏并上报 motion + 设备显示体感态提示
    /// （主键变鼠标左键，避免不知情误判「按下没反应」）；退出：停表 + 恢复 ready。
    @discardableResult
    public func toggleAirMouse(deviceID: String) -> Bool {
        if isAirMouseActive(deviceID: deviceID) {
            airMouseActiveDevices.remove(deviceID)
            airMouseStates.removeValue(forKey: deviceID)
            ble.sendStickControlPayload(
                BleProtocol.airMouseEnabledPayload(enabled: false),
                label: "air_mouse_enabled(false)", deviceID: deviceID
            )
            if let peripheralID = ble.peripheralID(forDeviceID: deviceID) {
                ble.sendUIState("ready", to: peripheralID)
            }
            statusController.setAirMouseActive(false, deviceID: deviceID)
            NSLog("air mouse disabled on VS-\(deviceID)")
            stopAirMouseTimerIfIdle()
            return false
        }
        airMouseActiveDevices.insert(deviceID)
        // lastOmegaAt 初始化为当下，防止首次积分 dt 爆炸（对齐 Windows）。
        airMouseStates[deviceID] = AirMouseDeviceState(lastOmegaAt: Date())
        ble.sendStickControlPayload(
            BleProtocol.airMouseEnabledPayload(enabled: true),
            label: "air_mouse_enabled(true)", deviceID: deviceID
        )
        if let peripheralID = ble.peripheralID(forDeviceID: deviceID) {
            ble.sendUIState("air_mouse", to: peripheralID)
        }
        statusController.setAirMouseActive(true, deviceID: deviceID)
        NSLog("air mouse enabled on VS-\(deviceID)")
        startAirMouseTimerIfNeeded()
        return true
    }

    private func startAirMouseTimerIfNeeded() {
        guard airMouseTimer == nil else { return }
        airMouseTimer = Timer.scheduledTimer(
            withTimeInterval: Self.airMouseTickInterval, repeats: true
        ) { [weak self] _ in
            self?.airMouseTick()
        }
    }

    private func stopAirMouseTimerIfIdle() {
        if airMouseStates.isEmpty {
            airMouseTimer?.invalidate()
            airMouseTimer = nil
        }
    }

    /// 运行期参数组装（对齐 Windows AirMouseParamsForDevice）：灵敏度按设备取
    /// （gain = sensitivity × 48，P1 去双重缩放标定），其余进阶参数走全局配置。
    private func airMouseParams(forDevice deviceID: String) -> AirMouseParams {
        var params = AirMouseParams()
        let inter = config.interactionSettings(for: deviceID)
        params.gainX = Double(inter.airMouseSensitivityX) * 48.0
        params.gainY = Double(inter.airMouseSensitivityY) * 48.0
        params.tau = config.airMouse.tau
        params.invertY = config.airMouse.invertY
        params.curve = AirMouseKin.curveClamp(AirMouseCurveParams(
            lowThresh: config.airMouse.curveLowThresh,
            highThresh: config.airMouse.curveHighThresh,
            lowFactor: config.airMouse.curveLowFactor,
            highFactor: config.airMouse.curveHighFactor
        ))
        params.controlMode = AirMouseControlMode.fromName(config.airMouse.controlMode)
        params.neutralDeadzone = config.airMouse.neutralDeadzone
        params.rateGain = config.airMouse.rateGain
        params.rateFriction = config.airMouse.rateFriction
        params.rateMaxSpeed = config.airMouse.rateMaxSpeed
        return params
    }

    /// motion 帧入口：仅在该设备处于体感态时更新 omega 并积分 theta；光标位移由
    /// airMouseTick 统一驱动（~50Hz 固件上报与 60Hz 桌面 tick 分工，对齐 Windows）。
    private func handleMotionFrame(_ frame: MotionFrame, peripheralID: UUID) {
        guard let deviceID = deviceID(for: peripheralID),
              isAirMouseActive(deviceID: deviceID),
              var state = airMouseStates[deviceID] else { return }

        // theta = 对 omega 积分（dt 用 tick 周期估计，避免 BLE 帧率抖动致 dt≈0），
        // 供 rate 飞行摇杆模式；angle 模式用瞬时 omega，不依赖 theta。
        let dt = Self.airMouseTickInterval
        state.thetaX += Double(frame.dx) * dt
        state.thetaY += Double(frame.dy) * dt
        state.thetaX = min(max(state.thetaX, -Self.airMouseMaxTheta), Self.airMouseMaxTheta)
        state.thetaY = min(max(state.thetaY, -Self.airMouseMaxTheta), Self.airMouseMaxTheta)

        state.lastOmegaX = Double(frame.dx)
        state.lastOmegaY = Double(frame.dy)
        state.lastOmegaAt = Date()
        airMouseStates[deviceID] = state
    }

    /// 60Hz tick：速度环驱动光标（对齐 Windows AirMouseTick）。固定 dt = tick 周期；
    /// omega 超龄（stale）归零输入，速度环经 tau 滑行停止（防断帧仍持续移动）。
    private func airMouseTick() {
        guard !airMouseStates.isEmpty else { return }
        let now = Date()
        for (deviceID, state) in airMouseStates {
            var state = state
            let params = airMouseParams(forDevice: deviceID)
            let omegaAge = now.timeIntervalSince(state.lastOmegaAt)
            let stale = omegaAge > Self.airMouseOmegaStaleAge
            if stale {
                state.lastOmegaX = 0
                state.lastOmegaY = 0
            }
            // 回到中立区（theta/omega 都很小）或 stale 时归零 theta，实现「回正即停」。
            if stale ||
                (abs(state.thetaX) < Self.airMouseAngleDeadzone &&
                 abs(state.thetaY) < Self.airMouseAngleDeadzone &&
                 abs(state.lastOmegaX) < Self.airMouseOmegaDeadzone &&
                 abs(state.lastOmegaY) < Self.airMouseOmegaDeadzone) {
                state.thetaX = 0
                state.thetaY = 0
            }

            var input = AirMouseInput()
            if params.controlMode == .angle {
                // angle 模式速度命令用瞬时 omega（theta 无限增长会正反馈失控，P0 修复）。
                input.valueX = Int(state.lastOmegaX)
                input.valueY = Int(state.lastOmegaY)
            } else {
                // rate 模式 theta 控制光标速度变化率（回中后速度保持 + 摩擦衰减）。
                input.valueX = Int(state.thetaX)
                input.valueY = Int(state.thetaY)
                input.isAngle = true
            }

            let result = AirMouseKin.step(
                &state.kin,
                input: input,
                dtSeconds: Self.airMouseTickInterval,
                inputIsStale: stale,
                params: params
            )
            airMouseStates[deviceID] = state
            if result.dx != 0 || result.dy != 0 {
                inputInjector.moveMouse(dx: result.dx, dy: result.dy)
            }
        }
    }

    /// 断连/遗忘设备的体感态必须清理，否则残留激活会拦截重连后的主键录音（对齐 Windows）。
    private func clearAirMouseForInactiveDevices(reason: String) {
        for deviceID in airMouseActiveDevices where !ble.isConnected(deviceID: deviceID) {
            airMouseActiveDevices.remove(deviceID)
            airMouseStates.removeValue(forKey: deviceID)
            ble.sendStickControlPayload(
                BleProtocol.airMouseEnabledPayload(enabled: false),
                label: "air_mouse_enabled(false)", deviceID: deviceID
            )
            statusController.setAirMouseActive(false, deviceID: deviceID)
            NSLog("air mouse disabled on VS-\(deviceID) (\(reason))")
        }
        stopAirMouseTimerIfIdle()
    }

    // MARK: - 编码器按键事件（对齐 Windows HandleEncoderButton{Down,Up,Click,DoubleClick}）

    private func handleEncoderButtonDown(_ event: StateEvent, peripheralID: UUID) {
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        if enc.pressAction == .recording {
            handleButtonDown(event, peripheralID: peripheralID)
            return
        }
        // key 动作：down/up 不注入（在 click 成对确认时注入一次），仅记日志。
        NSLog("encoder button down on VS-\(deviceID(for: peripheralID) ?? "unknown") (press_action=key, ignored)")
    }

    private func handleEncoderButtonUp(_ event: StateEvent, peripheralID: UUID) {
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        if enc.pressAction == .recording {
            handleButtonUp(event, peripheralID: peripheralID)
            return
        }
        NSLog("encoder button up on VS-\(deviceID(for: peripheralID) ?? "unknown") (press_action=key, ignored)")
    }

    private func handleEncoderButtonClick(_ event: StateEvent, peripheralID: UUID) {
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        if enc.pressAction == .recording {
            handleButtonClick(event, peripheralID: peripheralID)
            return
        }
        guard let spec = KeySpec.parse(enc.pressKey) else {
            NSLog("encoder press key invalid: \"\(enc.pressKey)\" on VS-\(deviceID(for: peripheralID) ?? "unknown"), click ignored")
            return
        }
        // 编码器单击在录音/识别中仍注入是有意设计：该键是用户显式配置的快捷键（如撤销），
        // 与会话状态正交；不同于 rotate 的录音中抑制。
        NSLog("encoder click on VS-\(deviceID(for: peripheralID) ?? "unknown"), injecting \(spec.displayText)")
        inputInjector.sendKeyCombo(spec)
    }

    private func handleEncoderButtonDoubleClick(_ event: StateEvent, peripheralID: UUID) {
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        let deviceID = deviceID(for: peripheralID) ?? ""
        if enc.doubleClickAction == .recording {
            // 切换录音起停：复用固件 remote_button 通道（固件侧等价一次远程按下/松开，
            // 音频链路真实完整，等同 click_to_talk 点按起停）。remote_button 走
            // APP_INPUT_SOURCE_REMOTE，不受 encoder_recording_gate 门控约束——
            // press_action=key（门控关）时双击起停录音仍可用，属有意设计。
            let hasActive = activeSessionID != nil || activeSubtitleSessions[peripheralID] != nil
            NSLog("encoder double-click on VS-\(deviceID)\(hasActive ? ", remote stop recording" : ", remote start recording")")
            let requestID = nextHotkeyRequestID
            nextHotkeyRequestID &+= 1
            ble.sendRemoteButton(
                action: hasActive ? "up" : "down",
                deviceID: deviceID,
                requestID: requestID
            )
            return
        }
        // key 动作：沿用物理主键双击的取消结构，注入配置的按键（默认 enter=现行为）。
        cancelActiveSessionsForDoubleClick(peripheralID: peripheralID)
        if let spec = KeySpec.parse(enc.doubleClickKey) {
            NSLog("encoder double-click on VS-\(deviceID), injecting \(spec.displayText)")
            inputInjector.sendKeyCombo(spec)
        } else {
            NSLog("encoder double-click key invalid: \"\(enc.doubleClickKey)\" on VS-\(deviceID), fallback to Enter")
            inputInjector.sendEnter()
        }
        ble.sendUIState("ready", to: peripheralID)
        mainInputState = .ready
        statusController.setStatus("Ready")
    }

    // MARK: - 敲击事件（对齐 Windows HandleTapEvent）

    private func handleTapEvent(_ event: StateEvent, peripheralID: UUID) {
        // 总开关关闭则忽略（按设备取有效配置）。
        guard config.interactionSettings(for: deviceID(for: peripheralID)).tapToArrow else { return }
        // 体感态忽略敲击，避免与体感移动/点击冲突（对齐 Windows）。
        if isAirMouseActive(deviceID: deviceID(for: peripheralID)) { return }
        // 录音中或识别中忽略敲击，避免震动干扰当前语音周期。
        // 与双击主键不同：tap 不取消录音/识别，仅在不冲突时注入方向键。
        if mainInputState.isRecording || mainInputState.isFinalizing {
            return
        }
        // 节流：两次方向键注入最短间隔 500ms，防止快速连击导致光标连续下移。
        let now = Date()
        if let last = lastTapInjectAt, now.timeIntervalSince(last) < 0.5 {
            NSLog("tap detected on VS-\(deviceID(for: peripheralID) ?? "unknown"), throttled (<500ms since last)")
            return
        }
        lastTapInjectAt = now
        NSLog("tap detected on VS-\(deviceID(for: peripheralID) ?? "unknown"), sending ArrowDown")
        inputInjector.sendArrowDown()
        ble.sendUIState("ready", to: peripheralID)
    }

    // MARK: - 编码器旋转事件（对齐 Windows HandleEncoderRotate/InjectEncoderRotateSteps/
    // FlushEncoderRotatePending/EncoderRotateTick）

    /// steps 上限钳制：固件侧已截断到 uint8（255），桌面端再钳到物理合理值，
    /// 防伪造/异常 BLE 帧让注入循环放大挂死线程。真实 10ms 窗口内旋转 1~3 步。
    private static let maxEncoderRotateSteps: UInt32 = 64
    /// 停转窗口：连续旋转时事件间隔 <=10ms（加 BLE 抖动亦远小于此值），静默超过
    /// 250ms 无旋转事件即可靠判定停稳。
    private static let encoderRotateStopGap: TimeInterval = 0.25

    private func handleEncoderRotate(_ event: StateEvent, peripheralID: UUID) {
        let deviceID = deviceID(for: peripheralID) ?? ""
        let enc = config.encoderSettings(for: deviceID)
        // 总开关关闭则忽略。
        guard enc.toArrow else { return }
        // 体感态忽略旋转，避免与体感移动/点击冲突（固件侧也有体感门控，双保险）。
        if isAirMouseActive(deviceID: deviceID) { return }
        // 录音中或识别中忽略旋转，避免干扰当前语音周期（门控与 tap 一致）。
        if mainInputState.isRecording || mainInputState.isFinalizing {
            return
        }
        let rawSteps = event.steps ?? 0
        guard rawSteps > 0 else { return }
        let steps = min(rawSteps, Self.maxEncoderRotateSteps)
        // 多设备交替旋转：测速估计器是全局单例，换设备时重置冷启动，避免跨设备手势
        // 互相污染 EWMA 估计（阈值/方向可能按设备不同）。
        if deviceID != lastEncoderRotateDeviceID {
            encoderSpeedEstimator.reset()
            lastEncoderRotateDeviceID = deviceID
        }
        // 方向映射：默认 cw→rotate_cw_key / ccw→rotate_ccw_key；
        // rotation_invert=true 时翻转。direction 非 "ccw"（含空串/未知值）按 cw 处理，
        // 与固件只发 cw|ccw 的约定一致。
        let effectiveCCW = (event.direction == "ccw") != enc.rotationInvert
        let now = Date()
        if encoderRotateLockout {
            let stopped = lastEncoderRotateEventAt
                .map { now.timeIntervalSince($0) > Self.encoderRotateStopGap } ?? true
            if !stopped {
                // 锁定中：屏蔽一切旋转输出（含快甩减速段的慢速事件与换向事件）。
                // 不喂测速估计器：减速段样本与新手势无关，静默 250ms 后估计器自动冷启动。
                lastEncoderRotateEventAt = now
                NSLog("encoder rotate suppressed (lockout) on VS-\(deviceID) direction=\(event.direction ?? "") steps=\(steps)")
                return
            }
            // 已停稳：退出锁定，本事件走正常识别。
            encoderRotateLockout = false
        }
        // 快慢分档：EWMA 平滑估计测速（见 EncoderRotateSpeedEstimator）；估计值
        // >= rotate_fast_threshold 走快速档按键，否则走普通按键。
        let speedSps = encoderSpeedEstimator.addSample(now: now, steps: steps)
        let fast = EncoderRotateSpeedEstimator.isFast(
            smoothedSpeedSps: speedSps, thresholdSps: enc.rotateFastThreshold
        )
        let normalKey = effectiveCCW ? enc.rotateCcwKey : enc.rotateCwKey
        let fastKey = effectiveCCW ? enc.rotateCcwFastKey : enc.rotateCwFastKey
        if fast {
            // 快速甩动视为一次手势：注入一次快速键后进入停转锁定，直到停稳才恢复识别。
            encoderRotateLockout = true
            lastEncoderRotateEventAt = now
            if encoderPendingActive {
                // 加速段识别：挂起的慢速事件是本次快甩的起步，整段丢弃不注入。
                encoderPendingActive = false
                encoderPendingSteps = 0
                encoderPendingPeripheralID = nil
                stopEncoderRotateTimer()
                NSLog("encoder rotate pending discarded (acceleration) on VS-\(deviceID)")
            }
            // 选键：快速档 → 普通档 → 方向键兜底；一次手势只注入一次。
            var fastSpec = KeySpec.parse(fastKey)
            if fastSpec == nil {
                // 快速档按键非法（绕过加载校验直改内存/未来新键名）回退普通按键。
                NSLog("encoder rotate fast key invalid: \"\(fastKey)\" on VS-\(deviceID), fallback to normal key")
                fastSpec = KeySpec.parse(normalKey)
            }
            guard let spec = fastSpec else {
                // 非法配置回退方向键，保持可用。
                NSLog("encoder rotate key invalid: \"\(normalKey)\" on VS-\(deviceID), fallback to arrows")
                if effectiveCCW {
                    inputInjector.sendArrowUp()
                } else {
                    inputInjector.sendArrowDown()
                }
                return
            }
            // 与 tap 不同，旋转不回写 ui_state（无屏幕状态变化）。
            NSLog("encoder rotate on VS-\(deviceID) direction=\(event.direction ?? "") steps=\(steps)\(rawSteps > steps ? " (clamped from \(rawSteps))" : "") speed=\(Int(speedSps))sps [fast] -> \(spec.displayText)")
            inputInjector.sendKeyCombo(spec)
            return
        }
        // 慢速路径：延迟判定——先挂起累计，判定窗内判快则整段丢弃（见 fast 分支），
        // 到期由 encoderRotateTick 或此处新事件检查冲刷。window<=0 时立即注入（旧行为）。
        if enc.rotateDecideWindowMs <= 0 {
            injectEncoderRotateSteps(ccw: effectiveCCW, steps: steps, keyText: normalKey, deviceID: deviceID)
            return
        }
        if encoderPendingActive {
            let expired = now.timeIntervalSince(encoderPendingStartedAt) >=
                Double(enc.rotateDecideWindowMs) / 1000.0
            if expired || encoderPendingCCW != effectiveCCW {
                // 旧 pending 到期或换向：不是本次快甩的加速段，立即冲刷。
                flushEncoderRotatePending()
            }
        }
        if !encoderPendingActive {
            encoderPendingActive = true
            encoderPendingCCW = effectiveCCW
            encoderPendingSteps = 0
            encoderPendingStartedAt = now
            encoderPendingPeripheralID = peripheralID
            startEncoderRotateTimer()
        }
        encoderPendingSteps = min(encoderPendingSteps + steps, Self.maxEncoderRotateSteps)
        NSLog("encoder rotate pending on VS-\(deviceID) direction=\(event.direction ?? "") steps=\(steps) total=\(encoderPendingSteps)")
    }

    private func injectEncoderRotateSteps(ccw: Bool, steps: UInt32, keyText: String, deviceID: String) {
        guard let spec = KeySpec.parse(keyText) else {
            // 非法配置（绕过加载校验直改内存/未来新键名）回退方向键，保持可用。
            NSLog("encoder rotate key invalid: \"\(keyText)\" on VS-\(deviceID), fallback to arrows")
            for _ in 0..<steps {
                if ccw {
                    inputInjector.sendArrowUp()
                } else {
                    inputInjector.sendArrowDown()
                }
            }
            return
        }
        // 与 tap 不同，旋转不回写 ui_state（无屏幕状态变化）。
        NSLog("encoder rotate inject on VS-\(deviceID) steps=\(steps) -> \(spec.displayText)")
        for _ in 0..<steps {
            inputInjector.sendKeyCombo(spec)
        }
    }

    private func flushEncoderRotatePending() {
        guard encoderPendingActive else { return }
        let ccw = encoderPendingCCW
        let steps = encoderPendingSteps
        let peripheralID = encoderPendingPeripheralID
        encoderPendingActive = false
        encoderPendingSteps = 0
        encoderPendingPeripheralID = nil
        stopEncoderRotateTimer()
        // 按挂起 pending 来源设备取覆盖配置；设备中途断开则回全局默认（可接受）。
        let deviceID = peripheralID.flatMap { self.deviceID(for: $0) } ?? ""
        let enc = config.encoderSettings(for: deviceID)
        let key = ccw ? enc.rotateCcwKey : enc.rotateCwKey
        injectEncoderRotateSteps(ccw: ccw, steps: steps, keyText: key, deviceID: deviceID)
    }

    private func encoderRotateTick() {
        guard encoderPendingActive, let peripheralID = encoderPendingPeripheralID else { return }
        // 到期判定用挂起 pending 来源设备的覆盖配置（decide_window_ms 可按设备不同）。
        let enc = config.encoderSettings(for: deviceID(for: peripheralID))
        guard enc.rotateDecideWindowMs > 0 else { return }
        if Date().timeIntervalSince(encoderPendingStartedAt) >=
            Double(enc.rotateDecideWindowMs) / 1000.0 {
            flushEncoderRotatePending()
        }
    }

    private func startEncoderRotateTimer() {
        guard encoderRotateTimer == nil else { return }
        // 30ms 周期检查 pending 到期（对齐 Windows UI 层 30ms 定时器）。
        let timer = Timer(timeInterval: 0.03, repeats: true) { [weak self] _ in
            self?.encoderRotateTick()
        }
        RunLoop.main.add(timer, forMode: .common)
        encoderRotateTimer = timer
    }

    private func stopEncoderRotateTimer() {
        encoderRotateTimer?.invalidate()
        encoderRotateTimer = nil
    }

    private func handlePrimaryButtonDown(sessionID: UInt32?, peripheralID: UUID) {
        // 体感态：主键长按/按下不启动录音（点击由 button_click 映射为左键，对齐 Windows）。
        if isAirMouseActive(deviceID: deviceID(for: peripheralID)) { return }
        if config.defaultOutputProfile.target == .subtitle {
            handleSubtitlePrimaryButtonDown(sessionID: sessionID, peripheralID: peripheralID)
            return
        }
        if handleFrontButtonDuringPendingPaste(peripheralID: peripheralID) {
            return
        }
        if mainInputState.isBusy {
            if activePeripheralID != peripheralID {
                ble.sendUIState("ready", to: peripheralID)
            } else if mainInputState.isFinalizing || isWaitingForFinalText {
                ble.sendUIState("thinking", to: peripheralID)
            }
            NSLog("Ignoring primary button while main input is busy")
            return
        }
        guard let sessionID, sessionID != 0 else {
            NSLog("Ignoring primary button down with missing/zero session; sending ready")
            ble.sendUIState("ready", to: peripheralID)
            return
        }

        mainInputState = .recording(sessionID: sessionID, peripheralID: peripheralID, startedAt: Date())
        receivedAudioFrames = 0
        firstAudioFrameAt = nil
        lastAudioFrameAt = nil
        armRecordingInactivityWatchdog(sessionID: sessionID, peripheralID: peripheralID)
        bufferedOggChunks.removeAll(keepingCapacity: true)
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        pendingPasteState = .idle
        isShowingASRError = false
        oggMuxer.reset()
        if config.audioNoiseSuppression {
            do {
                audioEnhancer = try AudioEnhancer()
            } catch {
                NSLog("AudioEnhancer init failed: \(error.localizedDescription); audio passthrough")
                audioEnhancer = nil
            }
        } else {
            audioEnhancer = nil
        }
        audioEnhancerFallbackLogged = false
        debugAudioRecorder.start(deviceID: deviceID(for: peripheralID), sessionID: sessionID,
                                 devicePrefix: deviceNamePrefix(for: peripheralID))
        statusController.showListening(deviceID: deviceID(for: peripheralID))
        sendUIStateForActiveDevice("recording")
    }

    private func handlePrimaryButtonUp(peripheralID: UUID) {
        if activeSubtitleSessions[peripheralID] != nil {
            handleSubtitlePrimaryButtonUp(peripheralID: peripheralID)
            return
        }
        guard case .recording(_, let recordingPeripheralID, _) = mainInputState,
              recordingPeripheralID == peripheralID
        else {
            return
        }
        let recordingDuration = currentRecordingDuration
        let isValidRecording = recordingDuration >= minimumRecordingDuration
        if !isValidRecording {
            cancelShortRecording()
        } else {
            beginWaitingForAudioEnd(reason: "button_up")
        }
    }

    private func handleAudioFrame(_ frame: AudioFrame, peripheralID: UUID) {
        if subtitleCycle(peripheralID: peripheralID, sessionID: frame.sessionID) != nil {
            handleSubtitleAudioFrame(frame, peripheralID: peripheralID)
            return
        }
        guard frame.sessionID == activeSessionID, activePeripheralID == peripheralID else { return }
        guard !sentFinalAudioChunk || (frame.isEnd && frame.payload.isEmpty) else { return }

        if frame.isEnd && frame.payload.isEmpty {
            cancelAudioEndTimeout()
            sendFinalOggChunkIfNeeded(recordingDuration: currentRecordingDuration)
            return
        }

        guard !frame.payload.isEmpty else { return }
        receivedAudioFrames += 1
        let frameArrivedAt = Date()
        if firstAudioFrameAt == nil { firstAudioFrameAt = frameArrivedAt }
        lastAudioFrameAt = frameArrivedAt
        armRecordingInactivityWatchdog(sessionID: frame.sessionID, peripheralID: peripheralID)
        var payload = frame.payload
        if let audioEnhancer {
            if let enhanced = audioEnhancer.processFrame(frame.payload) {
                payload = enhanced
            } else if !audioEnhancerFallbackLogged {
                audioEnhancerFallbackLogged = true
                NSLog("AudioEnhancer processFrame failed; passthrough for rest of session")
            }
        }
        let oggChunk = oggMuxer.append(opusPayload: payload, isLast: frame.isEnd)
        debugAudioRecorder.append(oggChunk)
        sendOrBufferOggChunk(
            oggChunk,
            isLast: frame.isEnd,
            canStartASR: currentRecordingDuration >= minimumRecordingDuration
        )
        if frame.isEnd {
            cancelAudioEndTimeout()
            let recordingDuration = currentRecordingDuration
            sentFinalAudioChunk = true
            enterFinalizingState(reason: "audio_end")
            if !asrStarted && recordingDuration < minimumRecordingDuration {
                cancelShortRecording()
            } else if asrStarted {
                debugAudioRecorder.finish()
                statusController.setStatus("Processing")
                sendUIStateForActiveDevice("thinking")
            } else {
                debugAudioRecorder.finish()
                if !startASRAndFlushBufferedChunks(lastChunkIsFinal: true) {
                    finishWithASRError("Failed to start ASR")
                    return
                }
                statusController.setStatus("Processing")
                sendUIStateForActiveDevice("thinking")
            }
        }
    }

    private func beginWaitingForAudioEnd(reason: String) {
        guard !waitingForAudioEnd else { return }
        waitingForAudioEnd = true
        NSLog("Waiting for audio END frame reason=\(reason)")
        enterFinalizingState(reason: reason)
        scheduleAudioEndTimeout()
    }

    private func enterFinalizingState(reason: String) {
        guard case .recording(let sessionID, let peripheralID, let startedAt) = mainInputState else {
            return
        }
        recordingInactivityTimer?.invalidate()
        NSLog("Main input finalizing reason=\(reason)")
        mainInputState = .finalizing(sessionID: sessionID, peripheralID: peripheralID, startedAt: startedAt)
        statusController.setStatus("Processing")
        sendUIStateForActiveDevice("thinking")
    }

    private func scheduleAudioEndTimeout() {
        audioEndTimeoutTimer?.invalidate()
        let sessionID = activeSessionID
        let peripheralID = activePeripheralID
        audioEndTimeoutTimer = Timer.scheduledTimer(withTimeInterval: audioEndTimeout, repeats: false) { [weak self] _ in
            guard let self else { return }
            guard self.waitingForAudioEnd,
                  self.activeSessionID == sessionID,
                  self.activePeripheralID == peripheralID
            else { return }
            NSLog("Audio END timeout; finalizing buffered audio")
            self.sendFinalOggChunkIfNeeded(recordingDuration: self.currentRecordingDuration)
        }
    }

    private func cancelAudioEndTimeout() {
        waitingForAudioEnd = false
        audioEndTimeoutTimer?.invalidate()
        audioEndTimeoutTimer = nil
    }

    /// 录音中音频静默看门狗：每个音频帧到达时重新计时；到点仍无帧即按 audio_end 收尾。
    /// 回调内二次校验会话身份，过期定时器误触发为无害 no-op。
    private func armRecordingInactivityWatchdog(sessionID: UInt32, peripheralID: UUID) {
        recordingInactivityTimer?.invalidate()
        recordingInactivityTimer = Timer.scheduledTimer(
            withTimeInterval: recordingInactivityTimeout,
            repeats: false
        ) { [weak self] _ in
            guard let self else { return }
            guard case .recording(let activeSessionID, let activePeripheralID, _) = self.mainInputState,
                  activeSessionID == sessionID,
                  activePeripheralID == peripheralID
            else { return }
            NSLog("Recording inactivity timeout (no audio frames for %.1fs); finalizing buffered audio",
                  self.recordingInactivityTimeout)
            self.sendFinalOggChunkIfNeeded(recordingDuration: self.currentRecordingDuration)
        }
    }

    private func handleSubtitlePrimaryButtonDown(sessionID: UInt32?, peripheralID: UUID) {
        guard let sessionID, sessionID != 0 else {
            NSLog("Ignoring subtitle button_down with missing/zero session dev=VS-\(deviceID(for: peripheralID) ?? "unknown"); sending ready")
            ble.sendUIState("ready", to: peripheralID)
            return
        }
        let deviceID = deviceID(for: peripheralID)
        NSLog("Subtitle button_down dev=VS-\(deviceID ?? "unknown") session=\(sessionID) active=\(activeSubtitleSessions[peripheralID].map(String.init) ?? "nil") existing=\(subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) != nil)")
        if activeSubtitleSessions[peripheralID] == sessionID ||
            subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) != nil {
            NSLog("Subtitle button_down ignored dev=VS-\(deviceID ?? "unknown") session=\(sessionID)")
            return
        }
        if let previousSessionID = activeSubtitleSessions[peripheralID] {
            NSLog("Subtitle button_down preempt active dev=VS-\(deviceID ?? "unknown") previous=\(previousSessionID) next=\(sessionID)")
            clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: previousSessionID)
        }
        let cycle = SubtitleCycle(
            peripheralID: peripheralID,
            deviceID: deviceID,
            sessionID: sessionID,
            config: config,
            asr: makeAsr(config),
            makeDebugRecorder: makeDebugRecorder
        )
        configureSubtitleASRCallbacks(for: cycle)
        subtitleCycles[SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID)] = cycle
        activeSubtitleSessions[peripheralID] = sessionID
        cycle.debugAudioRecorder.start(deviceID: deviceID, sessionID: sessionID,
                                       devicePrefix: deviceNamePrefix(for: peripheralID))
        NSLog("Subtitle cycle start dev=VS-\(deviceID ?? "unknown") session=\(sessionID)")
        statusController.showListening(deviceID: deviceID)
        ble.sendUIState("recording", to: peripheralID)
    }

    private func handleSubtitlePrimaryButtonUp(peripheralID: UUID) {
        guard let cycle = activeSubtitleCycle(peripheralID: peripheralID) else {
            NSLog("Subtitle button_up ignored no active cycle dev=VS-\(deviceID(for: peripheralID) ?? "unknown")")
            return
        }
        let sessionID = cycle.sessionID
        NSLog("Subtitle button_up dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) frames=\(cycle.receivedAudioFrames) duration=\(String(format: "%.3f", cycle.duration))")
        if cycle.duration < minimumRecordingDuration {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
        } else if cycle.receivedAudioFrames == 0 {
            finishSubtitleCycleWithError(
                peripheralID: peripheralID,
                sessionID: sessionID,
                message: "No audio frames from device"
            )
        } else {
            beginWaitingForSubtitleAudioEnd(cycle, reason: "button_up")
            finishSubtitleAudioInput(cycle)
        }
    }

    private func handleSubtitleAudioFrame(_ frame: AudioFrame, peripheralID: UUID) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: frame.sessionID) else { return }
        if frame.isEnd && frame.payload.isEmpty {
            cancelSubtitleAudioEndTimeout(cycle)
            sendSubtitleFinalOggChunkIfNeeded(peripheralID: peripheralID, sessionID: frame.sessionID)
            return
        }
        guard !frame.payload.isEmpty else { return }
        cycle.receivedAudioFrames += 1
        let oggChunk = cycle.oggMuxer.append(opusPayload: frame.payload, isLast: frame.isEnd)
        cycle.debugAudioRecorder.append(oggChunk)
        sendOrBufferSubtitleOggChunk(
            oggChunk,
            isLast: frame.isEnd,
            canStartASR: cycle.duration >= minimumRecordingDuration,
            cycle: cycle
        )
        if frame.isEnd {
            cancelSubtitleAudioEndTimeout(cycle)
            cycle.sentFinalAudioChunk = true
            if !cycle.asrStarted && cycle.duration < minimumRecordingDuration {
                cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
            } else if cycle.asrStarted {
                cycle.debugAudioRecorder.finish()
                finishSubtitleAudioInput(cycle)
            } else {
                cycle.debugAudioRecorder.finish()
                if !startSubtitleASRAndFlushBufferedChunks(cycle, lastChunkIsFinal: true) {
                    finishSubtitleCycleWithError(
                        peripheralID: peripheralID,
                        sessionID: cycle.sessionID,
                        message: "Failed to start ASR"
                    )
                    return
                }
                finishSubtitleAudioInput(cycle)
            }
        }
    }

    private func beginWaitingForSubtitleAudioEnd(_ cycle: SubtitleCycle, reason: String) {
        guard !cycle.waitingForAudioEnd else { return }
        cycle.waitingForAudioEnd = true
        NSLog("Waiting for subtitle audio END frame VS-\(cycle.deviceID ?? "unknown") reason=\(reason)")
        if config.interactionMode != .holdToTalk {
            statusController.setStatus("Processing")
            ble.sendUIState("thinking", to: cycle.peripheralID)
        }
        scheduleSubtitleAudioEndTimeout(cycle)
    }

    private func scheduleSubtitleAudioEndTimeout(_ cycle: SubtitleCycle) {
        cycle.audioEndTimeoutTimer?.invalidate()
        let peripheralID = cycle.peripheralID
        let sessionID = cycle.sessionID
        cycle.audioEndTimeoutTimer = Timer.scheduledTimer(withTimeInterval: audioEndTimeout, repeats: false) { [weak self] _ in
            guard let self,
                  let cycle = self.subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
                  cycle.waitingForAudioEnd
            else { return }
            NSLog("Subtitle audio END timeout VS-\(cycle.deviceID ?? "unknown"); finalizing buffered audio")
            self.sendSubtitleFinalOggChunkIfNeeded(peripheralID: peripheralID, sessionID: sessionID)
        }
    }

    private func cancelSubtitleAudioEndTimeout(_ cycle: SubtitleCycle) {
        cycle.waitingForAudioEnd = false
        cycle.audioEndTimeoutTimer?.invalidate()
        cycle.audioEndTimeoutTimer = nil
    }

    private func sendSubtitleFinalOggChunkIfNeeded(peripheralID: UUID, sessionID: UInt32) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
              !cycle.sentFinalAudioChunk
        else { return }
        cycle.sentFinalAudioChunk = true
        NSLog("Subtitle audio final dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) frames=\(cycle.receivedAudioFrames) asr_started=\(cycle.asrStarted)")
        cancelSubtitleAudioEndTimeout(cycle)
        if !cycle.asrStarted && cycle.duration < minimumRecordingDuration {
            cancelSubtitleCycle(peripheralID: peripheralID, reason: "short_recording")
            return
        }
        let finalChunk = cycle.oggMuxer.finish()
        cycle.debugAudioRecorder.append(finalChunk)
        cycle.debugAudioRecorder.finish()
        sendOrBufferSubtitleOggChunk(finalChunk, isLast: true, canStartASR: true, cycle: cycle)
    }

    private func finishSubtitleAudioInput(_ cycle: SubtitleCycle) {
        if config.interactionMode == .holdToTalk {
            NSLog("Subtitle audio input finished dev=VS-\(cycle.deviceID ?? "unknown") session=\(cycle.sessionID) -> device ready")
            clearActiveSubtitleSession(peripheralID: cycle.peripheralID, sessionID: cycle.sessionID)
            ble.sendUIState("ready", to: cycle.peripheralID)
        } else {
            statusController.setStatus("Processing")
            ble.sendUIState("thinking", to: cycle.peripheralID)
        }
    }

    private func sendOrBufferSubtitleOggChunk(_ chunk: Data, isLast: Bool, canStartASR: Bool, cycle: SubtitleCycle) {
        if cycle.asrStarted {
            cycle.asr.sendOggOpusChunk(chunk, isLast: isLast)
            return
        }
        cycle.bufferedOggChunks.append(chunk)
        guard canStartASR else { return }
        if !startSubtitleASRAndFlushBufferedChunks(cycle, lastChunkIsFinal: isLast) {
            finishSubtitleCycleWithError(
                peripheralID: cycle.peripheralID,
                sessionID: cycle.sessionID,
                message: "Failed to start ASR"
            )
        }
    }

    private func startSubtitleASRAndFlushBufferedChunks(_ cycle: SubtitleCycle, lastChunkIsFinal: Bool) -> Bool {
        guard !cycle.asrStarted else { return true }
        let useDefiniteSegments = shouldUseDefiniteSegments(for: outputProfile(for: cycle.deviceID))
        guard cycle.asr.start(options: ASRSessionOptions(
            hotwords: config.asrHotwords,
            dialogContext: recentDialogContext(),
            resultType: useDefiniteSegments ? .single : .full,
            showUtterances: useDefiniteSegments
        )) else {
            cycle.bufferedOggChunks.removeAll(keepingCapacity: true)
            return false
        }
        cycle.asrStarted = true
        for bufferedChunk in cycle.bufferedOggChunks.dropLast() {
            cycle.asr.sendOggOpusChunk(bufferedChunk, isLast: false)
        }
        if let lastChunk = cycle.bufferedOggChunks.last {
            cycle.asr.sendOggOpusChunk(lastChunk, isLast: lastChunkIsFinal)
        }
        cycle.bufferedOggChunks.removeAll(keepingCapacity: true)
        return true
    }

    private func sendFinalOggChunkIfNeeded(recordingDuration: TimeInterval) {
        guard !sentFinalAudioChunk else { return }
        sentFinalAudioChunk = true
        cancelAudioEndTimeout()
        recordingInactivityTimer?.invalidate()
        // 诊断锚点：音频流到达跨度 vs 录音时长。跨度远大于录音时长 = BLE 侧积压拖尾；
        // 跨度≈0 且 frames 少 = 设备侧没发帧。2026-10-11 排查识别延迟时加。
        let streamSpan = (firstAudioFrameAt != nil && lastAudioFrameAt != nil)
            ? lastAudioFrameAt!.timeIntervalSince(firstAudioFrameAt!)
            : 0
        NSLog("Audio stream summary frames=%d span=%.3fs recording=%.3fs",
              receivedAudioFrames, streamSpan, recordingDuration)
        enterFinalizingState(reason: "final_audio_sent")
        if !asrStarted && recordingDuration < minimumRecordingDuration {
            cancelShortRecording()
            return
        }
        if receivedAudioFrames == 0 {
            finishWithASRError("No audio frames from device")
            return
        }

        let finalChunk = oggMuxer.finish()
        debugAudioRecorder.append(finalChunk)
        debugAudioRecorder.finish()
        sendOrBufferOggChunk(finalChunk, isLast: true, canStartASR: true)
        statusController.setStatus("Processing")
        sendUIStateForActiveDevice("thinking")
    }

    private func shouldSendPartialToDevice() -> Bool {
        sentFinalAudioChunk
    }

    private var currentRecordingDuration: TimeInterval {
        guard let activeSessionStartedAt else { return 0 }
        return Date().timeIntervalSince(activeSessionStartedAt)
    }

    private func sendOrBufferOggChunk(_ chunk: Data, isLast: Bool, canStartASR: Bool) {
        if asrStarted {
            asr.sendOggOpusChunk(chunk, isLast: isLast)
            return
        }

        bufferedOggChunks.append(chunk)
        guard canStartASR else { return }
        if !startASRAndFlushBufferedChunks(lastChunkIsFinal: isLast) {
            finishWithASRError("Failed to start ASR")
        }
    }

    private func startASRAndFlushBufferedChunks(lastChunkIsFinal: Bool) -> Bool {
        guard !asrStarted else { return true }
        let profile = outputProfile(for: activeDeviceID)
        let useDefiniteSegments = shouldUseDefiniteSegments(for: profile)
        guard asr.start(options: ASRSessionOptions(
            hotwords: config.asrHotwords,
            dialogContext: recentDialogContext(),
            resultType: useDefiniteSegments ? .single : .full,
            showUtterances: useDefiniteSegments
        )) else {
            bufferedOggChunks.removeAll(keepingCapacity: true)
            return false
        }

        asrStarted = true
        for bufferedChunk in bufferedOggChunks.dropLast() {
            asr.sendOggOpusChunk(bufferedChunk, isLast: false)
        }
        if let lastChunk = bufferedOggChunks.last {
            asr.sendOggOpusChunk(lastChunk, isLast: lastChunkIsFinal)
        }
        bufferedOggChunks.removeAll(keepingCapacity: true)
        return true
    }

    private func cancelShortRecording() {
        NSLog("Ignoring short recording under \(minimumRecordingDuration)s")
        cancelAudioEndTimeout()
        recordingInactivityTimer?.invalidate()
        bufferedOggChunks.removeAll(keepingCapacity: true)
        asr.cancel()
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        debugAudioRecorder.discard()
        statusController.setStatus("Ready")
        statusController.hideOverlay()
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
    }

    private func finishWithFinalText(_ text: String) {
        guard !pastedFinalText else { return }
        // 语音编辑指令拦截（方案 §5.6/§4 关键原则 1）：必须在 beginSessionHistory /
        // 精修 / 翻译 / 粘贴之前判定；命中即整轮按命令处理并 return（不粘贴、不写历史）。
        if handleVoiceEditCommandIfMatched(text) { return }
        // targeted 编辑（删除/替换最近一次输入中的指定词）：旧解析器优先、targeted
        // 其次（互不重叠）；同样命中即整轮接管并 return。
        if handleVoiceTargetedEditIfMatched(text) { return }
        // 会话历史草稿（用户数据飞轮 P0-3）：final 文本产出即建立，落库在
        // finishWithRefinedText（所有收口路径统一经过）。
        beginSessionHistory(rawAsr: text)
        // 新会话开始 = 上一轮编辑追踪窗口提前结束（Typeless track 边界同款）。
        finishEditTracking(status: EditTrackingStatus.skipped, editedSpan: nil)
        // 改写对应用闭环（用户数据飞轮，doubao 方案 §3.1 应用位置 1）：全句精确命中
        // 改写对 → 直接替换（毫秒级零 LLM，同一错误第二次不犯），use_count 落盘。
        if let applied = applyCorrectionPairExact(rawAsr: text) {
            finishWithRefinedText(applied)
            return
        }
        // LLM 流式精修（对齐 Windows RefineStream 语义）：final 原文先经 LLM 改写，
        // overlay 进 refining 态后 token 增量追加（~60ms 节流）；失败/热词丢失/预算
        // 超时回退原文，SSE 失败客户端内部自动回退非流式。
        if config.refineEnabled, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            // B3：短文本跳过精修，直接粘贴原文。
            guard text.trimmingCharacters(in: .whitespacesAndNewlines).count >= kRefineMinChars else {
                NSLog("refine skipped (short text) len=%d", text.utf8.count)
                cancelStreamingRefinement()
                finishWithRefinedText(text)
                return
            }
            statusController.showRefining(text, deviceID: activeDeviceID)
            cancelStreamingRefinement()
            let token = RefineCancelToken()
            refinementCancelToken = token
            let overlayDeviceID = activeDeviceID
            // B1 打点：begin 时刻，首 token/complete 分段耗时全靠它。
            let refineBeginAt = Date()
            NSLog("refine begin text_len=%d", text.utf8.count)
            // 节流状态（onToken 串行到达于 URLSession 线程，仅此处读写）。
            var accumulated = ""
            var lastUpdateAt = Date.distantPast
            var didLogFirstToken = false
            refiner.refineStream(
                text,
                promptOverride: config.refinePrompt,
                hotwords: hotwordsForLlmPrompts(),
                corrections: correctionPairsForLlmPrompts(),
                cancel: token,
                onToken: { [weak self] delta in
                    guard let self, !token.isCancelled, !self.pastedFinalText else { return }
                    if !didLogFirstToken {
                        didLogFirstToken = true
                        let ms = Int(Date().timeIntervalSince(refineBeginAt) * 1000)
                        NSLog("refine first token ms=%d", ms)
                        // onToken 在 URLSession 回调线程，定时器失效切回主线程。
                        DispatchQueue.main.async {
                            self.refineFirstTokenTimer?.invalidate()
                            self.refineFirstTokenTimer = nil
                        }
                    }
                    accumulated += delta
                    let now = Date()
                    guard now.timeIntervalSince(lastUpdateAt) >= 0.06 else { return }
                    lastUpdateAt = now
                    let snapshot = accumulated
                    DispatchQueue.main.async {
                        guard !token.isCancelled, !self.pastedFinalText else { return }
                        self.statusController.appendPartial(snapshot, deviceID: overlayDeviceID)
                    }
                },
                // 思考链活动同样证明端点存活：失效首 token 预算，防默认开思考的
                // 模型被 2.5s 预算误杀（总预算仍全程 armed 兜底思考过长的长尾）。
                onActivity: { [weak self] in
                    DispatchQueue.main.async {
                        self?.refineFirstTokenTimer?.invalidate()
                        self?.refineFirstTokenTimer = nil
                    }
                },
                onComplete: { [weak self] ok, result, fallback in
                    DispatchQueue.main.async {
                        guard let self, !token.isCancelled, !self.pastedFinalText else { return }
                        self.refinementCancelToken = nil
                        self.cancelRefineBudgetTimers()
                        var reason = fallback
                        if ok, !result.isEmpty,
                           !resultKeepsHotwords(
                               original: text, refined: result, hotwords: self.config.asrHotwords
                           ) {
                            reason = .hotwordGuard
                        }
                        NSLog("refine complete ok=%d ms=%d out_len=%d fallback=%@",
                              ok ? 1 : 0, Int(Date().timeIntervalSince(refineBeginAt) * 1000),
                              result.utf8.count, reason.rawValue)
                        if ok, !result.isEmpty, reason != .hotwordGuard {
                            // 纠错对挖掘（用户数据飞轮 P0-1）：original→refined 提取最小
                            // 替换片段对落盘，供后续精修 prompt few-shot 注入。
                            self.recordCorrectionPairs(original: text, refined: result)
                            // 热词候选挖掘（用户数据飞轮 P0-2，对齐 Windows
                            // MineHotwordCandidatesFromRefinement）：refined 新出现的
                            // 标识符样式词计数入池，达阈值提示用户确认。
                            self.mineHotwordCandidates(original: text, refined: result)
                            // 用最终累积文本做最后一次 UI 刷新。
                            self.statusController.showPartial(result, deviceID: overlayDeviceID)
                            self.finishWithRefinedText(result)
                        } else {
                            self.finishWithRefinedText(text)
                        }
                    }
                }
            )
            // B2：预算定时器在启动精修后 arm——放协调器层是为了覆盖
            // 「SSE 失败→非流式重发」的内部回退总时长（client 层看不到全程）。
            armRefineBudgetTimers(text: text, token: token, beginAt: refineBeginAt)
            return
        }
        finishWithRefinedText(text)
    }

    private func finishWithRefinedText(_ text: String) {
        guard !pastedFinalText else { return }
        // 热词使用统计：最终文本（精修或原文）中命中的热词计数 + 刷新时间戳
        //（对齐 Windows RecordHotwordUsageFromText；仅主线程调用，无锁）。
        recordHotwordUsage(fromText: text)
        // 会话历史落库（用户数据飞轮 P0-3）：refined/injected 先按 final 文本记，
        // translate 成功后回填 injected（见下方 transform 分支）。
        let sessionHistoryID = completeSessionHistory(finalText: text)
        let profile = outputProfile(for: activeDeviceID)
        if profile.target == .subtitle {
            pastedFinalText = true
            pendingPasteState = .idle
            let deviceID = activeDeviceID
            if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                showSubtitleText(text, profile: profile, deviceID: deviceID)
            }
            finishRecognitionCycle()
            statusController.hideOverlay()
            statusController.setStatus("Ready")
            sendUIStateForActiveDevice("ready")
            mainInputState = .ready
            return
        }
        if text.isEmpty {
            pastedFinalText = true
            pendingPasteState = .idle
            finishRecognitionCycle()
            statusController.hideOverlay()
            statusController.setStatus("Ready")
            sendUIStateForActiveDevice("ready")
            mainInputState = .ready
            return
        }

        if profile.transform == .translate {
            pastedFinalText = true
            statusController.setStatus("Translating")
            transformText(text, profile: profile, deviceID: activeDeviceID) { [weak self] result in
                guard let self else { return }
                switch result {
                case .success(let translatedText):
                    // 会话历史回填实际注入文本（与 refined 不同：翻译产物）。
                    if let sessionHistoryID {
                        self.sessionHistoryStore?.updateInjectedText(
                            id: sessionHistoryID, text: translatedText
                        )
                    }
                    self.enterPendingConfirmation(text: translatedText)
                case .failure(let error):
                    self.finishWithASRError(error.localizedDescription)
                }
            }
            return
        }

        enterPendingConfirmation(text: text)
    }

    private func enterPendingConfirmation(text: String) {
        pastedFinalText = true
        lastRecoverableText = text
        lastRecoverablePeripheralID = activePeripheralID
        statusController.setHasRecoverableInput(true)
        pendingPasteState = .waitingToPaste(text: text)
        pendingPasteEnteredAt = Date()
        if let peripheralID = activePeripheralID {
            mainInputState = .pendingConfirmation(peripheralID: peripheralID)
        }
        NSLog("Enter pending confirmation dev=VS-%@ text_len=%d (overlay showFinal)",
              activeDeviceID ?? "unknown", text.utf8.count)
        statusController.showFinal(text, deviceID: activeDeviceID) { [weak self] in
            NSLog("Pending confirmation hidden; committing paste text_len=%d", text.utf8.count)
            self?.commitPendingPaste(text: text, manual: false)
        }
        sendUIStateForActiveDevice("pending_confirmation", text: text)
    }

    private func handleDefiniteSegment(_ segment: ASRSegment) {
        guard segment.definite, let deviceID = activeDeviceID else { return }
        let profile = outputProfile(for: deviceID)
        guard shouldUseDefiniteSegments(for: profile) else { return }
        statusController.hideOverlay(deviceID: deviceID)
        showSubtitleText(segment.text, profile: profile, deviceID: deviceID)
    }

    private func handleSubtitleDefiniteSegment(_ segment: ASRSegment, peripheralID: UUID) {
        guard segment.definite, let cycle = activeSubtitleCycle(peripheralID: peripheralID) else { return }
        let profile = outputProfile(for: cycle.deviceID)
        guard shouldUseDefiniteSegments(for: profile) else { return }
        statusController.hideOverlay(deviceID: cycle.deviceID)
        showSubtitleText(segment.text, profile: profile, deviceID: cycle.deviceID)
    }

    private func finishSubtitleCycleWithFinalText(peripheralID: UUID, sessionID: UInt32, text: String) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID),
              !cycle.finishedFinalText
        else { return }
        cycle.finishedFinalText = true
        NSLog("Subtitle final text dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) text_len=\(text.count)")
        let profile = outputProfile(for: cycle.deviceID)
        if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            showSubtitleText(text, profile: profile, deviceID: cycle.deviceID) { [weak self] didShowSubtitle in
                guard let self else { return }
                self.finishSubtitleCycle(
                    peripheralID: peripheralID,
                    sessionID: sessionID,
                    hideOverlay: didShowSubtitle &&
                        self.shouldHideOverlayForFinishedSubtitleCycle(
                            peripheralID: peripheralID,
                            sessionID: sessionID
                        )
                )
            }
            return
        }
        finishSubtitleCycle(
            peripheralID: peripheralID,
            sessionID: sessionID,
            hideOverlay: shouldHideOverlayForFinishedSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID)
        )
    }

    private func finishSubtitleCycleWithError(peripheralID: UUID, sessionID: UInt32, message: String) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) else { return }
        NSLog("ASR error VS-\(cycle.deviceID ?? "unknown"): \(message)")
        cycle.asr.cancel()
        cycle.debugAudioRecorder.discard()
        cancelSubtitleAudioEndTimeout(cycle)
        clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: sessionID)
        if !hasActiveSubtitleSession(peripheralID: peripheralID) {
            statusController.showError(message, deviceID: cycle.deviceID) { [weak self] in
                self?.ble.sendUIState("ready", to: peripheralID)
            }
        }
        subtitleCycles.removeValue(forKey: SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID))
    }

    private func cancelSubtitleCycle(peripheralID: UUID, reason: String) {
        guard let cycle = activeSubtitleCycle(peripheralID: peripheralID) else { return }
        NSLog("Cancel subtitle cycle VS-\(cycle.deviceID ?? "unknown") reason=\(reason)")
        cycle.asr.cancel()
        cycle.debugAudioRecorder.discard()
        cancelSubtitleAudioEndTimeout(cycle)
        statusController.hideOverlay(deviceID: cycle.deviceID)
        ble.sendUIState("ready", to: peripheralID)
        clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: cycle.sessionID)
        subtitleCycles.removeValue(
            forKey: SubtitleCycleKey(peripheralID: peripheralID, sessionID: cycle.sessionID)
        )
    }

    private func finishSubtitleCycle(peripheralID: UUID, sessionID: UInt32, hideOverlay: Bool) {
        guard let cycle = subtitleCycle(peripheralID: peripheralID, sessionID: sessionID) else { return }
        NSLog("Subtitle cycle finish dev=VS-\(cycle.deviceID ?? "unknown") session=\(sessionID) hide_overlay=\(hideOverlay) active=\(activeSubtitleSessions[peripheralID].map(String.init) ?? "nil")")
        if hideOverlay {
            statusController.hideOverlay(deviceID: cycle.deviceID)
        }
        clearActiveSubtitleSession(peripheralID: peripheralID, sessionID: sessionID)
        if !hasActiveSubtitleSession(peripheralID: peripheralID) {
            statusController.setStatus("Ready")
            ble.sendUIState("ready", to: peripheralID)
        }
        subtitleCycles.removeValue(forKey: SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID))
    }

    private func shouldHideOverlayForFinishedSubtitleCycle(peripheralID: UUID, sessionID: UInt32) -> Bool {
        isActiveSubtitleCycle(peripheralID: peripheralID, sessionID: sessionID) ||
            !hasActiveSubtitleSession(peripheralID: peripheralID)
    }

    private func canUpdateOverlayForSubtitleCycle(peripheralID: UUID, sessionID: UInt32) -> Bool {
        activeSubtitleSessions[peripheralID].map { $0 == sessionID } ?? true
    }

    private func shouldSendSubtitlePartialToDevice(_ cycle: SubtitleCycle) -> Bool {
        guard isActiveSubtitleCycle(peripheralID: cycle.peripheralID, sessionID: cycle.sessionID) else {
            return false
        }
        return cycle.sentFinalAudioChunk
    }

    private func subtitleCycle(peripheralID: UUID, sessionID: UInt32) -> SubtitleCycle? {
        subtitleCycles[SubtitleCycleKey(peripheralID: peripheralID, sessionID: sessionID)]
    }

    private func activeSubtitleCycle(peripheralID: UUID) -> SubtitleCycle? {
        guard let sessionID = activeSubtitleSessions[peripheralID] else { return nil }
        return subtitleCycle(peripheralID: peripheralID, sessionID: sessionID)
    }

    private func isActiveSubtitleCycle(peripheralID: UUID, sessionID: UInt32) -> Bool {
        activeSubtitleSessions[peripheralID] == sessionID
    }

    private func hasActiveSubtitleSession(peripheralID: UUID) -> Bool {
        activeSubtitleSessions[peripheralID] != nil
    }

    private func clearActiveSubtitleSession(peripheralID: UUID, sessionID: UInt32) {
        if activeSubtitleSessions[peripheralID] == sessionID {
            activeSubtitleSessions.removeValue(forKey: peripheralID)
        }
    }

    private func cancelSubtitleCycles(peripheralID: UUID, reason: String) {
        NSLog("Cancel subtitle cycles \(peripheralID) reason=\(reason)")
        activeSubtitleSessions.removeValue(forKey: peripheralID)
        let keys = subtitleCycles.keys.filter { $0.peripheralID == peripheralID }
        for key in keys {
            guard let cycle = subtitleCycles[key] else { continue }
            cycle.asr.cancel()
            cycle.debugAudioRecorder.discard()
            cancelSubtitleAudioEndTimeout(cycle)
            subtitleCycles.removeValue(forKey: key)
        }
        statusController.hideOverlay(deviceID: deviceID(for: peripheralID))
        ble.sendUIState("ready", to: peripheralID)
    }

    private func showSubtitleText(
        _ text: String,
        profile: OutputProfile,
        deviceID: String?,
        completion: ((Bool) -> Void)? = nil
    ) {
        guard let deviceID else {
            completion?(false)
            return
        }
        transformText(text, profile: profile, deviceID: deviceID) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let outputText):
                NSLog("Subtitle show text dev=VS-\(deviceID) text_len=\(outputText.count)")
                self.subtitleController.show(
                    text: outputText,
                    deviceID: deviceID,
                    color: self.config.themeColor(for: deviceID)
                )
                completion?(true)
            case .failure(let error):
                self.statusController.showError(error.localizedDescription, deviceID: deviceID)
                completion?(false)
            }
        }
    }

    private func transformText(
        _ text: String,
        profile: OutputProfile,
        deviceID: String?,
        completion: @escaping (Result<String, Error>) -> Void
    ) {
        guard profile.transform == .translate else {
            completion(.success(text))
            return
        }
        translator.translate(
            text,
            targetLanguage: profile.translationTarget,
            hotwords: hotwordsForLlmPrompts()
        ) { [weak self] result in
            DispatchQueue.main.async {
                guard self != nil else { return }
                completion(result)
            }
        }
    }

    // MARK: - 流式精修取消与热词使用统计（对齐 Windows CancelStreamingRefinement /
    // HotwordsForLlmPrompts / RecordHotwordUsageFromText）

    /// 取消进行中的流式精修（新会话开始/取消识别/配置热更新时调用；仅主线程）。
    private func cancelStreamingRefinement() {
        refinementCancelToken?.cancel()
        refinementCancelToken = nil
        cancelRefineBudgetTimers()
    }

    // MARK: - 精修预算超时回退（B2；Windows 对齐见方案 B7）

    /// arm 双预算定时器（仅主线程；重复 arm 前先清旧的，防悬挂定时器误杀后续会话）。
    /// 首 token 预算单独一个，onToken 首次到达即失效；总预算全程 armed。
    private func armRefineBudgetTimers(text: String, token: RefineCancelToken, beginAt: Date) {
        cancelRefineBudgetTimers()
        refineFirstTokenTimer = Timer.scheduledTimer(
            withTimeInterval: kRefineFirstTokenBudget, repeats: false
        ) { [weak self] _ in
            self?.handleRefineBudgetExceeded(token: token, beginAt: beginAt, text: text,
                                             kind: "first_token")
        }
        let totalBudget = max(kRefineTotalBudgetFloor,
                              kRefineTotalBudgetPerByte * Double(text.utf8.count))
        refineTotalTimer = Timer.scheduledTimer(
            withTimeInterval: totalBudget, repeats: false
        ) { [weak self] _ in
            self?.handleRefineBudgetExceeded(token: token, beginAt: beginAt, text: text,
                                             kind: "total")
        }
    }

    private func cancelRefineBudgetTimers() {
        refineFirstTokenTimer?.invalidate()
        refineFirstTokenTimer = nil
        refineTotalTimer?.invalidate()
        refineTotalTimer = nil
    }

    /// 预算触发：取消精修（迟到的 complete 被 token 守卫丢弃）+ 回退原文走完既有粘贴链路。
    private func handleRefineBudgetExceeded(token: RefineCancelToken, beginAt: Date,
                                            text: String, kind: String) {
        // 二次校验会话身份：过期定时器误触发为无害 no-op。
        guard refinementCancelToken === token, !pastedFinalText else { return }
        NSLog("refine budget exceeded (%@) after %dms; pasting original text_len=%d",
              kind, Int(Date().timeIntervalSince(beginAt) * 1000), text.utf8.count)
        token.cancel()
        refinementCancelToken = nil
        cancelRefineBudgetTimers()
        finishWithRefinedText(text)
    }

    private var hotwordUsageURL: URL {
        AppConfig.configDirectory.appendingPathComponent("hotword_usage.json")
    }

    /// 精修/翻译 prompt 热词段：评分 top-50（高频 × 新近度 × 手动加权，对齐 Windows
    /// HotwordsForLlmPrompts——防大库稀释小模型注意力；ASR 通道热词不受此裁剪）。
    private func hotwordsForLlmPrompts() -> [String] {
        if hotwordUsageStore == nil {
            hotwordUsageStore = HotwordSelector.loadUsage(url: hotwordUsageURL)
        }
        return HotwordSelector.trimForPrompt(
            hotwordUsageStore ?? [:],
            hotwords: config.asrHotwords,
            maxWords: kHotwordPromptMaxWords,
            nowS: Int64(Date().timeIntervalSince1970)
        )
    }

    /// 最终文本命中热词的计数与时间戳刷新（大小写不敏感子串匹配），立即落盘。
    private func recordHotwordUsage(fromText text: String) {
        guard !text.isEmpty, !config.asrHotwords.isEmpty else { return }
        if hotwordUsageStore == nil {
            hotwordUsageStore = HotwordSelector.loadUsage(url: hotwordUsageURL)
        }
        var store = hotwordUsageStore ?? [:]
        HotwordSelector.recordUsage(
            &store, text: text, hotwords: config.asrHotwords,
            nowS: Int64(Date().timeIntervalSince1970)
        )
        hotwordUsageStore = store
        HotwordSelector.saveUsage(store, url: hotwordUsageURL)
    }

    private var usageEventsURL: URL {
        (userDataDirectoryOverride ?? AppConfig.configDirectory)
            .appendingPathComponent("usage_events.jsonl")
    }

    /// 负信号事件落 JSONL（用户数据飞轮 P1-1；对齐 WeType「上屏与回退都记录」）。
    /// 只记文本长度与热词命中名单，绝不记文本本身。
    private func recordUsageEvent(
        kind: String, text: String?, dwellSeconds: TimeInterval? = nil
    ) {
        let lowered = (text ?? "").lowercased()
        let matched = config.asrHotwords.filter { !$0.isEmpty && lowered.contains($0.lowercased()) }
        UsageEventLog.append(
            url: usageEventsURL,
            event: UsageEvent(
                kind: kind,
                ts: Int64(Date().timeIntervalSince1970),
                deviceID: activeDeviceID,
                textLength: (text ?? "").count,
                hotwords: matched,
                dwellSeconds: dwellSeconds
            )
        )
    }

    /// 取消结果文本中命中的热词负样本记分（评分负项，P1-1 采集的消费端）。
    private func recordHotwordNegative(fromText text: String) {
        guard !text.isEmpty, !config.asrHotwords.isEmpty else { return }
        if hotwordUsageStore == nil {
            hotwordUsageStore = HotwordSelector.loadUsage(url: hotwordUsageURL)
        }
        var store = hotwordUsageStore ?? [:]
        HotwordSelector.recordNegative(&store, text: text, hotwords: config.asrHotwords)
        hotwordUsageStore = store
        HotwordSelector.saveUsage(store, url: hotwordUsageURL)
    }

    private var correctionPairsURL: URL {
        (userDataDirectoryOverride ?? AppConfig.configDirectory)
            .appendingPathComponent("correction_pairs.json")
    }

    /// 精修 prompt 纠错 few-shot 段候选（用户数据飞轮 P0-1，对齐 Windows
    /// CorrectionPairsForLlmPrompts）：按 count/新近度 top-8，每次精修从磁盘现读，
    /// 不做内存缓存——文件小且写点分散。
    private func correctionPairsForLlmPrompts() -> [CorrectionPair] {
        return CorrectionPairs.trimForPrompt(
            CorrectionPairs.load(url: correctionPairsURL),
            maxPairs: kCorrectionPromptMaxPairs,
            app: frontmostAppBundleID?()
        )
    }

    /// 精修完成后的纠错对挖掘：original→refined 提取最小替换片段对，经 B13 单写
    /// 入口落盘 correction_pairs.json（与 config.toml 同目录），按当前前台应用
    /// 分桶（doubao §3.4：微信聊天桶与 IDE 桶的改写几乎不重叠）。
    private func recordCorrectionPairs(original: String, refined: String) {
        guard original != refined else { return }
        CorrectionPairs.recordToDisk(
            url: correctionPairsURL,
            pairs: CorrectionPairs.extractReplacementPairs(original, refined),
            nowTs: Int64(Date().timeIntervalSince1970),
            app: frontmostAppBundleID?() ?? ""
        )
    }

    /// 改写对全句精确命中应用（doubao 方案 §3.1 应用位置 1）：命中即返回替换文本
    /// 并把 use_count 落盘；未命中/库停用返回 nil 走既有精修管线。
    private func applyCorrectionPairExact(rawAsr: String) -> String? {
        let url = correctionPairsURL
        guard let pair = CorrectionPairs.exactReplacement(
            for: rawAsr, in: CorrectionPairs.load(url: url), app: frontmostAppBundleID?()
        ) else { return nil }
        CorrectionPairs.markUsedOnDisk(
            url: url, pair: pair, nowTs: Int64(Date().timeIntervalSince1970)
        )
        appliedCorrectionPair = pair
        NSLog("correction pair applied exact: len=%d", pair.after.utf8.count)
        return pair.after
    }

    private var hotwordCandidatesFileURL: URL {
        (userDataDirectoryOverride ?? AppConfig.configDirectory)
            .appendingPathComponent("hotword_candidates.json")
    }

    /// 精修完成后的热词候选挖掘（用户数据飞轮 P0-2，对齐 Windows
    /// MineHotwordCandidatesFromRefinement/RecordAndNotifyHotwordCandidates）：
    /// refined 新出现的标识符样式词走 B13 单写入口计数，本次新晋达阈值的词弹悬浮
    /// 提示（Windows 同款「托盘可能被静默、悬浮消息保证必现」策略），由用户在
    /// 设置-热词候选区确认加入/忽略——挖掘只产候选，绝不自动入表。
    private func mineHotwordCandidates(original: String, refined: String) {
        let mined = HotwordCandidateMiner.mineRefinementCandidates(
            original: original, refined: refined, existingHotwords: config.asrHotwords
        )
        guard !mined.isEmpty else { return }
        let suggestions = HotwordCandidateMiner.recordToDisk(url: hotwordCandidatesFileURL, words: mined)
        guard !suggestions.isEmpty else { return }
        NSLog("hotword candidates suggested: %@", suggestions.joined(separator: ", "))
        let suffix = trText?("hotwordCandidateNotifyBodySuffix") ?? "hotwordCandidateNotifyBodySuffix"
        statusController.showTimedMessage(suggestions.joined(separator: ", ") + suffix, duration: 3)
    }

    /// 最近精修文本作为火山 dialog_ctx 上下文（用户数据飞轮 P1-6，零新数据纯接线）：
    /// 取最近完成会话的 refined 文本（latestRefinedTexts 旧→新，符合对话语序），
    /// 总量截到 600 字符（dialog_ctx 预算 800 tokens，留余量给分词差异）——让
    /// 同音纠错在 ASR 第一遍发生，而不是等精修兜底。
    private func recentDialogContext() -> [String] {
        guard let store = sessionHistoryStore else { return [] }
        var dialog: [String] = []
        var total = 0
        for text in store.latestRefinedTexts(limit: 3) {
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty else { continue }
            if total + trimmed.count > 600 { break }
            dialog.append(trimmed)
            total += trimmed.count
        }
        return dialog
    }

    /// 会话历史草稿（用户数据飞轮 P0-3）：final 文本产出即建立；空文本/工厂未装配
    ///（测试默认 nil）跳过。库打开/迁移失败降级停用，绝不阻塞主链路。
    private func beginSessionHistory(rawAsr: String) {
        pendingSessionHistory = nil
        guard let makeStore = makeSessionHistoryStore,
              !rawAsr.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        if sessionHistoryStore == nil {
            sessionHistoryStore = makeStore(SessionHistoryStore.defaultURL)
        }
        guard sessionHistoryStore != nil else { return }
        let profile = outputProfile(for: activeDeviceID)
        let hotwordsJSON = config.asrHotwords.isEmpty
            ? nil
            : (try? JSONSerialization.data(withJSONObject: config.asrHotwords))
                .flatMap { String(data: $0, encoding: .utf8) }
        pendingSessionHistory = PendingSessionHistory(
            id: UUID().uuidString,
            createdAt: Int64(Date().timeIntervalSince1970),
            deviceID: activeDeviceID,
            asrProvider: config.asrProvider.rawValue,
            mode: profile.transform == .translate ? "translate" : "original",
            rawAsr: rawAsr,
            duration: mainInputState.startedAt.map { Date().timeIntervalSince($0) },
            hotwordsSent: hotwordsJSON
        )
    }

    /// 历史落库（用户数据飞轮 P0-3）：返回行 ID 供 translate 回填 injected。
    private func completeSessionHistory(finalText: String) -> String? {
        guard let store = sessionHistoryStore, let pending = pendingSessionHistory else {
            pendingSessionHistory = nil
            return nil
        }
        pendingSessionHistory = nil
        var record = SessionHistoryRecord(
            id: pending.id,
            createdAt: pending.createdAt,
            deviceID: pending.deviceID,
            asrProvider: pending.asrProvider,
            mode: pending.mode,
            rawAsr: pending.rawAsr,
            refinedText: finalText,
            injectedText: finalText,
            duration: pending.duration,
            hotwordsSent: pending.hotwordsSent
        )
        // 客户端元数据：app 版本（Typeless app_version 字段的等价物）。
        if let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String {
            record.clientMetadata = "{\"app_version\":\"\(version)\"}"
        }
        store.insert(record)
        lastSessionHistoryID = pending.id
        return pending.id
    }

    // ---- 编辑追踪（用户数据飞轮 P0-2，Typeless 方案：上屏之后才算完）----
    // 注入成功后启动有限读窗（8s 首读 + 每 10s 一读、最多 5 读）：读到的焦点字段
    // 与注入文本做区间 diff，产物（改动 span）落 history.edited_text 并把「注入→
    // 编辑后」对按 source=edit 记入纠错对库——用户手动修改是最强纠错信号。
    // 隐私红线：字段全文只在内存参与 diff，落库的只有改动 span；读取闭包未装配
    //（测试/未授权）整个追踪静默停用。

    private func beginEditTracking(injectedText: String) {
        finishEditTracking(status: EditTrackingStatus.skipped, editedSpan: nil)
        guard let readFocusedFieldText, let historyID = lastSessionHistoryID,
              !injectedText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        editTrackingGeneration += 1
        editTrackingSession = EditTrackingSession(
            historyID: historyID, injectedText: injectedText, attempts: 0
        )
        if let store = sessionHistoryStore {
            store.updateEditedText(
                id: historyID, text: nil,
                status: EditTrackingStatus.pending, attempts: 0
            )
        }
        scheduleEditTrackingAttempt(delay: kEditTrackingFirstDelay)
    }

    private func scheduleEditTrackingAttempt(delay: TimeInterval) {
        guard editTrackingSession != nil else { return }
        let generation = editTrackingGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            self?.runEditTrackingAttempt(generation: generation)
        }
    }

    private func runEditTrackingAttempt(generation: Int) {
        guard generation == editTrackingGeneration, let tracking = editTrackingSession else { return }
        editTrackingSession?.attempts += 1
        // 焦点不可读（应用无 AX 树/权限/无文本焦点）：窗口内重试，耗尽判 TIMEOUT。
        guard let fieldText = readFocusedFieldText?(), !fieldText.isEmpty else {
            if let current = editTrackingSession, current.attempts >= kEditTrackingMaxAttempts {
                finishEditTracking(status: EditTrackingStatus.timeout, editedSpan: nil)
                return
            }
            scheduleEditTrackingAttempt(delay: kEditTrackingRetryDelay)
            return
        }
        // 未编辑（一致）或无法归因（公共前后缀 < 注入长度一半，焦点已切走/整体重写）：
        // 按无编辑收口（首次可读即收，避免无编辑会话烧满窗口）。
        guard let region = EditTracking.editedRegion(
            fieldText: fieldText, injectedText: tracking.injectedText
        ) else {
            finishEditTracking(status: EditTrackingStatus.extracted, editedSpan: nil)
            return
        }
        // 纯删除形态（字段侧中段为空）：无「错形→正形」锚点，不进纠错对库。
        guard !region.after.isEmpty else {
            finishEditTracking(status: EditTrackingStatus.extracted, editedSpan: nil)
            return
        }
        // 用户改动：落 history.edited_text（只存改动区间），并把「注入中段→编辑后」
        // 对按 source=edit 记入纠错对库（typeless P0-3 通道 2 的素材基础——用户手动
        // 修改是最强纠错信号）。
        finishEditTracking(status: EditTrackingStatus.extracted, editedSpan: region.after)
        let pairs = CorrectionPairs.extractReplacementPairs(region.before, region.after)
        CorrectionPairs.recordToDisk(
            url: correctionPairsURL, pairs: pairs,
            nowTs: Int64(Date().timeIntervalSince1970), source: "edit",
            app: frontmostAppBundleID?() ?? ""
        )
        NSLog("edit tracking captured span len=%d", region.after.utf8.count)
    }

    private func finishEditTracking(status: String, editedSpan: String?) {
        guard let tracking = editTrackingSession else { return }
        editTrackingSession = nil
        editTrackingGeneration += 1
        sessionHistoryStore?.updateEditedText(
            id: tracking.historyID, text: editedSpan, status: status, attempts: tracking.attempts
        )
        if status != EditTrackingStatus.pending {
            NSLog("edit tracking finished status=%@ attempts=%d", status, tracking.attempts)
        }
    }

    private func finishWithASRError(_ message: String) {
        NSLog("ASR error: \(message)")
        cancelAudioEndTimeout()
        asr.cancel()
        pendingPasteState = .idle
        debugAudioRecorder.discard()
        isShowingASRError = true
        errorRecoveryToken += 1
        let token = errorRecoveryToken
        let errorPeripheralID = activePeripheralID
        mainInputState = .error(peripheralID: errorPeripheralID)
        finishRecognitionCycle()
        sendUIStateForActiveDevice("error", text: message)
        statusController.showError(message, deviceID: activeDeviceID) { [weak self] in
            guard let self, self.errorRecoveryToken == token else { return }
            self.recoverFromASRError(hideOverlay: false)
        }
    }

    private func presentASRUpgradeAlert(url: URL, message: String) {
        statusController.hideOverlay { [weak self] in
            guard let self else { return }
            self.recoverFromASRError(hideOverlay: false)
            self.presentUpgradeAlertUI?(url, message)
        }
    }

    private func recoverFromASRError(hideOverlay: Bool = true) {
        guard isShowingASRError else { return }
        errorRecoveryToken += 1
        isShowingASRError = false
        if hideOverlay {
            statusController.hideOverlay()
        }
        if pairedDeviceIDs.isEmpty {
            statusController.setStatus("Pair a VoiceStick")
        } else {
            statusController.setStatus("Ready")
            sendUIStateForActiveDevice("ready")
            mainInputState = .ready
        }
    }

    private func commitPendingPaste(text: String, manual: Bool) {
        guard pendingPasteText == text else {
            return
        }

        completePendingPaste(text: text, manual: manual)
    }

    /// 无辅助功能权限时的注入兜底：悬浮窗提示（10s 节流，防编码器旋转等高频路径
    /// 刷屏）+ 转发 AppDelegate 弹每次启动一次的设置引导窗。
    private var lastAccessibilityWarningAt: Date?

    private func handleAccessibilityPermissionMissing() {
        let now = Date()
        if lastAccessibilityWarningAt == nil
            || now.timeIntervalSince(lastAccessibilityWarningAt!) > 10 {
            lastAccessibilityWarningAt = now
            statusController.showTimedMessage(
                (trText?("accessibilityPasteBlocked") ?? "accessibilityPasteBlocked"), duration: 2.5, deviceID: activeDeviceID
            )
        }
        onAccessibilityPermissionMissing?()
    }

    private func completePendingPaste(text: String, manual: Bool) {
        NSLog("Complete pending paste text_len=%d auto_enter=%d accessibility_trusted=%d",
              text.utf8.count, config.autoEnter ? 1 : 0, (isProcessTrusted?() ?? false) ? 1 : 0)
        let shouldPressEnter = config.autoEnter
        // P3 反馈环保留信号（用户数据飞轮）：精确应用的对被用户确认保留 → recall+1。
        if let pair = appliedCorrectionPair {
            appliedCorrectionPair = nil
            CorrectionPairs.markRecalledOnDisk(url: correctionPairsURL, pair: pair)
        }
        // 负信号采集（P1-1）：确认动作 + 停留时长（疑惑度信号）。
        recordUsageEvent(
            kind: manual ? UsageEventKind.pasteManuallyConfirmed : UsageEventKind.pasteAutoConfirmed,
            text: text,
            dwellSeconds: Date().timeIntervalSince(pendingPasteEnteredAt)
        )
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
        inputInjector.paste(text: text, pressEnter: shouldPressEnter)
        // 语音编辑：记录最近一次注入（方案 §6.1 记录点唯一 = 唯一粘贴 chokepoint）。
        // 无辅助功能权限时 paste 的 CGEvent 会被系统静默丢弃，此时不记，避免后续
        // 误以为自己注入过；新注入覆盖旧记录（始终只保留最近一次）。
        if !text.isEmpty, isProcessTrusted?() ?? false {
            lastInjection = LastInjection(
                text: text,
                remainingText: text,
                injectedAt: Date(),
                pressedEnter: shouldPressEnter,
                appID: frontmostAppBundleID?()
            )
            voiceEditGeneration += 1
        }
        beginEditTracking(injectedText: text)
    }

    // MARK: - 语音编辑指令（Voice Edit Commands；Doc/Plan/voice-edit-commands.md §4/§6/§8.1）

    /// finishWithFinalText 入口拦截：整句解析为编辑指令时吞掉本轮并按命令执行。
    /// 返回 true = 已按命令处理（调用方直接 return，绝不再粘贴/精修/写历史）。
    ///
    /// 顺序（方案 §8.1）：parse → pastedFinalText=true → finishEditTracking(skipped)
    /// → cancelStreamingRefinement → L0/L1 出计划 → 收尾（会清 pastedFinalText，故计划
    /// 先算好）→ 异步退格 → 完成后回主线程更新 LastInjection + toast（放收尾后，
    /// 避免被 hideOverlay 吞掉）。
    private func handleVoiceEditCommandIfMatched(_ text: String) -> Bool {
        let settings = config.voiceEdit
        guard settings.enabled else { return false }
        // 仅 focused_app 生效（决策点 D7）：字幕/微信输入法模式按原链路处理，不拦截。
        guard outputProfile(for: activeDeviceID).target == .focusedApp else { return false }
        guard let command = VoiceEditParser.parse(
            text, defaultChars: settings.defaultChars, maxChars: settings.maxChars
        ) else { return false }

        // 命中即吞掉本轮：置 pastedFinalText 阻止迟到回调二次粘贴。
        pastedFinalText = true
        // 命令轮不属于「语音输入内容」：结束上一轮编辑追踪（否则退格造成的字段变化
        // 会被误判为用户编辑写进纠错对），并停掉可能的精修（防御，正常不会在跑）。
        finishEditTracking(status: EditTrackingStatus.skipped, editedSpan: nil)
        cancelStreamingRefinement()

        let result = evaluateVoiceEditCommand(command, settings: settings)
        let commandDescription = describeVoiceEditCommand(command)

        // 收尾先做（命令轮不新增状态，方案 §8.1）：计划/记录已在上面算好；
        // toast 必须在收尾之后，避免被 hideOverlay 吞掉。
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready

        switch result {
        case .failure(let code):
            NSLog("voice_edit command=%@ result=failure code=%@", commandDescription, code.rawValue)
            showVoiceEditFailureToast(code)
        case .plan(let plan):
            NSLog("voice_edit command=%@ result=plan count=%d chars=%d newline=%d verification=%@",
                  commandDescription, plan.count, plan.charCount, plan.newlineCount,
                  plan.verification.rawValue)
            injectVoiceEditBackspaces(plan.count) { [weak self] in
                guard let self else { return }
                self.lastInjection?.remainingText = plan.newRemainingText
                self.lastInjection?.pressedEnter = plan.newPressedEnter
                self.showVoiceEditSuccessToast(command: command, plan: plan)
            }
        }
        return true
    }

    /// L0 作用域闸 + L1 目标校验（方案 §6.2/§6.3）：返回删除计划或拒绝码；
    /// 任一闸失败都绝不注入按键。检查顺序 = 方案 §6.2 编号顺序（记录→时间→应用→权限）。
    private func evaluateVoiceEditCommand(
        _ command: VoiceEditCommand, settings: VoiceEditSettings
    ) -> VoiceEditPlanResult {
        guard let record = lastInjection, !record.remainingText.isEmpty else {
            return .failure(.noInjection)
        }
        let elapsed = Date().timeIntervalSince(record.injectedAt)
        guard elapsed <= TimeInterval(settings.scopeSeconds) else { return .failure(.scopeExpired) }
        guard let recordedApp = record.appID, frontmostAppBundleID?() == recordedApp else {
            return .failure(.appChanged)
        }
        // 辅助功能权限与 CGEvent 注入同源：权限被收回后注入会被静默丢弃。
        guard isProcessTrusted?() ?? false else { return .failure(.accessibilityDenied) }
        // L1：可验证→必须通过；不可验证→由 verify_mode + unverified 窗口决定（§6.5）。
        let fieldState = readFocusedFieldState?()
        return VoiceEditPlanner.plan(
            remainingText: record.remainingText,
            pressedEnter: record.pressedEnter,
            action: command.action,
            fieldText: fieldState?.text,
            caretUTF16: fieldState?.caretUTF16,
            selectionLength: fieldState?.selectionLength,
            maxChars: settings.maxChars,
            verifyMode: settings.verifyMode,
            unverifiedWindowOpen: elapsed <= TimeInterval(settings.unverifiedSeconds)
        )
    }

    /// 异步退格注入（方案 §7.1）：后台串行队列 + 约 12ms 节拍；完成后回主线程收尾。
    /// CGEvent 投递线程安全（且 L0 已确认辅助功能权限），不占主线程。
    private func injectVoiceEditBackspaces(_ count: Int, completion: @escaping () -> Void) {
        guard count > 0, let backspace = KeySpec.parse("backspace") else {
            DispatchQueue.main.async(execute: completion)
            return
        }
        let injector = inputInjector
        voiceEditInjectionQueue.async {
            for index in 0..<count {
                injector.sendKeyCombo(backspace)
                if index + 1 < count { Thread.sleep(forTimeInterval: 0.012) }
            }
            DispatchQueue.main.async(execute: completion)
        }
    }

    // ---- 反馈文案（经 trText 单点闭包；缺 key 时 trText 现有行为回落 statusError）----

    private func describeVoiceEditCommand(_ command: VoiceEditCommand) -> String {
        switch command.action {
        case .deleteLastChars(let count): return "deleteLastChars(\(count))"
        case .deleteLastSentences(let count): return "deleteLastSentences(\(count))"
        case .deleteAll: return "deleteAll"
        }
    }

    /// 文案取值：trText 返回格式串（含 %d 时按 count 插值）。测试可注入恒等闭包
    ///（返回 key 本身）断言走到的 key。
    private func voiceEditText(_ key: String, count: Int? = nil) -> String {
        let format = trText?(key) ?? key
        guard let count else { return format }
        return String(format: format, count)
    }

    private func showVoiceEditFailureToast(_ code: VoiceEditFailureCode) {
        let key: String
        switch code {
        case .noInjection: key = "voiceEditNoInjection"
        case .scopeExpired: key = "voiceEditScopeExpired"
        case .appChanged: key = "voiceEditAppChanged"
        case .fieldMismatch: key = "voiceEditFieldMismatch"
        case .caretNotEnd: key = "voiceEditCaretNotEnd"
        case .selectionPresent: key = "voiceEditSelectionPresent"
        case .verifyUnavailable: key = "voiceEditVerifyUnavailable"
        case .foregroundElevated: key = "voiceEditForegroundElevated"
        case .accessibilityDenied: key = "voiceEditAccessibilityDenied"
        case .injectFailed: key = "voiceEditInjectFailed"
        }
        statusController.showTimedMessage(
            voiceEditText(key), duration: 2.5, deviceID: activeDeviceID
        )
    }

    private func showVoiceEditSuccessToast(command: VoiceEditCommand, plan: VoiceEditDeletionPlan) {
        let message: String
        switch command.action {
        case .deleteLastChars:
            message = voiceEditText("voiceEditDeletedChars", count: plan.charCount)
        case .deleteLastSentences(let count):
            message = count == 1
                ? voiceEditText("voiceEditDeletedSentenceOne")
                : voiceEditText("voiceEditDeletedSentences", count: count)
        case .deleteAll:
            message = voiceEditText("voiceEditDeletedAll")
        }
        let suffix = plan.verification == .unverified
            ? voiceEditText("voiceEditUnverifiedSuffix") : ""
        statusController.showTimedMessage(
            message + suffix, duration: 2.0, deviceID: activeDeviceID
        )
    }

    // MARK: - Targeted 语音编辑（删除/替换最近一次输入中的指定词）

    /// finishWithFinalText 第二道拦截（旧解析器优先）：整句命中 targeted 语法（含
    /// 「刚才的」作用域的删除/替换）时吞掉本轮按命令执行；`.invalid`（空目标/空替换/
    /// 多分隔符/否定）同样吞掉并提示——命令文本绝不粘贴上屏。返回 true = 已接管。
    ///
    /// 安全模型（Doc/Plan/voice-edit-commands.md targeted 章节）：targeted 动作无论
    /// verify_mode 都必须 verified（严禁复用 8s unverified 盲删/盲改）；L1 要求完整
    /// remainingText 仍是字段后缀（不用旧最长前缀容错）；执行用平台精确选区 +
    /// 读回逐字确认，一次 Backspace 或 paste(pressEnter=false)，恢复末尾光标后终验。
    private func handleVoiceTargetedEditIfMatched(_ text: String) -> Bool {
        let settings = config.voiceEdit
        guard settings.enabled else { return false }
        guard outputProfile(for: activeDeviceID).target == .focusedApp else { return false }
        guard let parseResult = VoiceTargetedParser.parse(text) else { return false }
        // 自然简写不带「刚才的」时，只有最近一次注入确实存在目标词才接管；
        // 否则整句继续走普通文本，避免把不含上下文的「把A改成B」一律吞掉。
        if case .command(let command) = parseResult, !command.hasExplicitScope {
            guard let remainingText = lastInjection?.remainingText,
                  !VoiceTargetedMatcher.occurrences(
                      of: command.target, in: remainingText
                  ).isEmpty else { return false }
        }

        // 命中即吞掉本轮：置 pastedFinalText 阻止迟到回调二次粘贴；命令轮不属于
        // 「语音输入内容」——结束上一轮编辑追踪、停掉可能的精修、不写历史/纠错对。
        pastedFinalText = true
        finishEditTracking(status: EditTrackingStatus.skipped, editedSpan: nil)
        cancelStreamingRefinement()

        // 计划先算好（finishRecognitionCycle 会清 pastedFinalText，方案 §8.1 顺序）。
        let outcome: VoiceTargetedOutcome
        switch parseResult {
        case .invalid:
            outcome = .failure(.invalid)
        case .command(let command):
            outcome = evaluateVoiceTargetedCommand(command)
        }

        // 收尾先做（命令轮不新增状态）：toast 必须在收尾之后，避免被 hideOverlay 吞掉。
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready

        switch outcome {
        case .failure(let code):
            NSLog("voice_targeted result=failure code=%@", code.rawValue)
            showVoiceTargetedFailureToast(code)
        case .context(let command, let plan, let context):
            NSLog("voice_targeted action=%@ target_len=%d occ=%d/%d",
                  describeVoiceTargetedAction(command.action), plan.targetLength,
                  plan.occurrence.index + 1, plan.occurrence.total)
            executeVoiceTargetedEdit(command: command, plan: plan, context: context)
        }
        return true
    }

    private enum VoiceTargetedOutcome {
        case failure(VoiceTargetedFailureCode)
        /// 执行上下文快照（值类型拷贝；异步终验时不受记录后续变化影响）。
        case context(VoiceTargetedCommand, VoiceTargetedPlan, VoiceTargetedEditContext)
    }

    /// targeted 执行上下文：计划时刻从 LastInjection + 字段状态确定性拆解出的
    /// 后缀结构（值类型快照，供异步终验计算期望文本/光标，不引用可变记录）。
    private struct VoiceTargetedEditContext {
        let remainingText: String
        let pressedEnter: Bool
        /// 字段末尾是否存在已记录的 auto_enter 换行（planner 后缀校验保证确定性）。
        let hasRecordedNewline: Bool
        /// 字段中 remainingText 之前的前缀（用户既有内容，可空）。
        let fieldPrefix: String
    }

    /// L0 作用域闸 + L1 目标校验（targeted 恒 verified）→ 执行上下文或拒绝码。
    private func evaluateVoiceTargetedCommand(
        _ command: VoiceTargetedCommand
    ) -> VoiceTargetedOutcome {
        guard let record = lastInjection, !record.remainingText.isEmpty else {
            return .failure(.noInjection)
        }
        let elapsed = Date().timeIntervalSince(record.injectedAt)
        guard elapsed <= TimeInterval(config.voiceEdit.scopeSeconds) else {
            return .failure(.scopeExpired)
        }
        guard let recordedApp = record.appID, frontmostAppBundleID?() == recordedApp else {
            return .failure(.appChanged)
        }
        guard isProcessTrusted?() ?? false else { return .failure(.accessibilityDenied) }
        guard let fieldState = readFocusedFieldState?() else {
            return .failure(.verifyUnavailable)
        }
        let planResult = VoiceTargetedPlanner.plan(
            command: command,
            remainingText: record.remainingText,
            pressedEnter: record.pressedEnter,
            fieldText: fieldState.text,
            caretUTF16: fieldState.caretUTF16,
            selectionLength: fieldState.selectionLength
        )
        guard case .plan(let plan) = planResult else {
            if case .failure(let code) = planResult { return .failure(code) }
            return .failure(.verifyUnavailable)
        }
        // 后缀结构拆解（planner 已验证后缀成立；与 planner 同顺序：pressedEnter 时
        // 优先按「remaining+\n」判定，避免 remaining 自身以换行结尾时把 auto_enter
        // 换行误并入前缀）。
        let remaining = record.remainingText
        let hasNewline: Bool
        let fieldPrefix: String
        if record.pressedEnter && fieldState.text.hasSuffix(remaining + "\n") {
            hasNewline = true
            fieldPrefix = String(fieldState.text.dropLast(remaining.count + 1))
        } else {
            hasNewline = false
            fieldPrefix = String(fieldState.text.dropLast(remaining.count))
        }
        let context = VoiceTargetedEditContext(
            remainingText: remaining,
            pressedEnter: record.pressedEnter,
            hasRecordedNewline: hasNewline,
            fieldPrefix: fieldPrefix
        )
        return .context(command, plan, context)
    }

    /// targeted 执行（主线程串行；单条命令内无循环注入，无 UI 阻塞）：
    ///   1. 平台精确选区（UTF-16，绝对偏移 = 字段尾差 + remaining 内偏移）；
    ///   2. 读回实际选中文本，逐字 == target 才继续（不符 → 恢复末尾光标 + 提示，
    ///      零删除/零粘贴，记录保留——字段文本未被改动）；
    ///   3. 一次 Backspace（删除）或 paste(pressEnter=false)（替换，复用粘贴通道）；
    ///   4. 延时后恢复光标到字段末尾并终验（读回字段文本 + 光标折叠在末尾）；
    ///   5. 终验通过 → 更新 remainingText（pressedEnter 不变；targeted 不消费换行）
    ///      + 成功 toast；不通过 → 记录失效（防陈旧偏移继续编辑）+ 提示检查，不重试。
    private func executeVoiceTargetedEdit(
        command: VoiceTargetedCommand, plan: VoiceTargetedPlan,
        context: VoiceTargetedEditContext
    ) {
        let generation = voiceEditGeneration
        guard let writer = writeTargetedSelection, let selectionReader = readTargetedSelectedText else {
            // 平台能力缺失 = 不可验证：targeted 恒 verified → 拒绝（不修改）。
            showVoiceTargetedFailureToast(.verifyUnavailable)
            return
        }
        let fieldUTF16 = context.remainingText.utf16.count + context.fieldPrefix.utf16.count
            + (context.hasRecordedNewline ? 1 : 0)
        // 绝对选区起点 = 前缀 UTF-16 长度 + remaining 内偏移（前缀/换行已在计划时刻
        // 确定性拆解，天然覆盖 auto_enter 换行在尾部的布局）。
        let selectionLocation = context.fieldPrefix.utf16.count + plan.occurrence.utf16Start

        // 1. 设置精确选区（只写选区，不写整个字段 Value）；失败尽力恢复安全光标。
        guard selectionLocation >= 0, writer(selectionLocation, plan.occurrence.utf16Length) else {
            _ = writer(fieldUTF16, 0)
            showVoiceTargetedFailureToast(.verifyUnavailable)
            return
        }
        // 2. 读回实际选中文本并逐字确认。
        let selectedText = selectionReader()
        guard selectedText == command.target else {
            // 选区读回不符（含读不到 nil）：恢复安全光标（末尾折叠），零删除/零粘贴；
            // 字段文本未被改动 → 记录保留有效。
            _ = writer(fieldUTF16, 0)
            showVoiceTargetedFailureToast(.fieldMismatch)
            return
        }
        // 3. 执行（单次动作）。
        switch plan.action {
        case .delete:
            guard let backspace = KeySpec.parse("backspace") else {
                // 修改前失败（key_spec 白名单恒支持 backspace，防御路径）：恢复安全
                // 光标、记录保留有效，不按「结果未确认」失效记录。
                _ = writer(fieldUTF16, 0)
                showVoiceTargetedFailureToast(.verifyUnavailable)
                return
            }
            inputInjector.sendKeyCombo(backspace)
        case .replace(let replacement):
            inputInjector.paste(text: replacement, pressEnter: false)
        }
        // 4/5. 延时恢复末尾光标 + 终验（stale generation 直接丢弃，不覆盖新记录）。
        let expectedFinalText = context.fieldPrefix + plan.newRemainingText
            + (context.hasRecordedNewline ? "\n" : "")
        DispatchQueue.main.asyncAfter(deadline: .now() + kVoiceTargetedVerifyDelay) { [weak self] in
            guard let self else { return }
            guard generation == self.voiceEditGeneration else { return }
            _ = self.writeTargetedSelection?(expectedFinalText.utf16.count, 0)
            guard let state = self.readFocusedFieldState?(),
                  state.text == expectedFinalText,
                  state.caretUTF16 == expectedFinalText.utf16.count,
                  state.selectionLength == 0 else {
                // 结果未确认：不自动重试、不宣称成功、不盲目撤销；记录失效。
                self.invalidateVoiceTargetedRecord(generation: generation)
                self.showVoiceTargetedFailureToast(.resultUnconfirmed)
                return
            }
            self.lastInjection?.remainingText = plan.newRemainingText
            self.showVoiceTargetedSuccessToast(plan: plan)
        }
    }

    private func invalidateVoiceTargetedRecord(generation: Int) {
        guard generation == voiceEditGeneration else { return }
        lastInjection = nil
    }

    private func describeVoiceTargetedAction(_ action: VoiceTargetedAction) -> String {
        switch action {
        case .delete: return "targetedDelete"
        case .replace: return "targetedReplace"
        }
    }

    private func showVoiceTargetedFailureToast(_ code: VoiceTargetedFailureCode) {
        let key: String
        switch code {
        case .notFound: key = "voiceTargetedNotFound"
        case .ambiguous: key = "voiceTargetedAmbiguous"
        case .invalid: key = "voiceTargetedInvalid"
        case .fieldMismatch: key = "voiceTargetedTextChanged"
        case .caretNotEnd: key = "voiceEditCaretNotEnd"
        case .selectionPresent: key = "voiceEditSelectionPresent"
        case .verifyUnavailable: key = "voiceEditVerifyUnavailable"
        case .resultUnconfirmed: key = "voiceTargetedUnconfirmed"
        case .noInjection: key = "voiceEditNoInjection"
        case .scopeExpired: key = "voiceEditScopeExpired"
        case .appChanged: key = "voiceEditAppChanged"
        case .accessibilityDenied: key = "voiceEditAccessibilityDenied"
        }
        statusController.showTimedMessage(
            voiceEditText(key), duration: 2.5, deviceID: activeDeviceID
        )
    }

    private func showVoiceTargetedSuccessToast(plan: VoiceTargetedPlan) {
        let key: String
        switch plan.action {
        case .delete: key = "voiceTargetedDeleted"
        case .replace: key = "voiceTargetedReplaced"
        }
        statusController.showTimedMessage(
            voiceEditText(key, count: plan.targetLength), duration: 2.0, deviceID: activeDeviceID
        )
    }

    private var isWaitingForFinalText: Bool {
        mainInputState.isFinalizing &&
            sentFinalAudioChunk &&
            asrStarted &&
            pendingPasteState.isIdle &&
            !pastedFinalText
    }

    private var pendingPasteText: String? {
        switch pendingPasteState {
        case .idle:
            return nil
        case .waitingToPaste(let text), .paused(let text):
            return text
        }
    }

    public func restoreLastInputConfirmation() -> Bool {
        restoreLastInputConfirmation(peripheralID: lastRecoverablePeripheralID)
    }

    private func restoreLastInputConfirmation(peripheralID: UUID?) -> Bool {
        guard
            pendingPasteState.isIdle,
            !mainInputState.isBusy,
            !isWaitingForFinalText,
            let text = lastRecoverableText,
            !text.isEmpty
        else {
            return false
        }

        pendingPasteState = .paused(text: text)
        if let peripheralID {
            mainInputState = .pausedConfirmation(peripheralID: peripheralID)
        }
        statusController.showPausedFinal(text, deviceID: activeDeviceID)
        sendUIStateForActiveDevice("pending_confirmation", text: text)
        return true
    }

    /// 「待粘贴确认」态的进入时刻（防卡死保险用：超过 60s 视为陈旧残留，
    /// 新会话按键时自动提交旧文本归位，而非静默吞掉按键）。
    private var pendingPasteEnteredAt = Date.distantPast

    private func handleFrontButtonDuringPendingPaste(peripheralID: UUID) -> Bool {
        switch pendingPasteState {
        case .idle:
            return false
        case .waitingToPaste(let text):
            guard activePeripheralID == peripheralID else { return true }
            // 防卡死保险：确认态滞留 >60s（正常 0.4s 自动提交）视为状态机残留
            //（如实例重启前的会话、hide 回调丢失），自动提交旧文本归位，
            // 本次按键继续走正常新会话路径而非被吞。
            if Date().timeIntervalSince(pendingPasteEnteredAt) > 60 {
                NSLog("Stale pending confirmation (%.0fs); force committing before new session",
                      Date().timeIntervalSince(pendingPasteEnteredAt))
                pendingPasteState = .idle
                completePendingPaste(text: text, manual: false)
                return false
            }
            pendingPasteState = .paused(text: text)
            mainInputState = .pausedConfirmation(peripheralID: peripheralID)
            statusController.showPausedFinal(text, deviceID: deviceID(for: peripheralID))
            sendUIStateForActiveDevice("pending_confirmation", text: text)
            return true
        case .paused(let text):
            guard activePeripheralID == peripheralID else { return true }
            statusController.hideOverlay { [weak self] in
                self?.commitPendingPaste(text: text, manual: true)
            }
            return true
        }
    }

    private func cancelPendingPaste(peripheralID: UUID) {
        if activeSessionID != nil {
            if activePeripheralID == peripheralID {
                cancelRecognitionInProgress()
            }
            return
        }
        if isWaitingForFinalText {
            guard activePeripheralID == peripheralID else { return }
            cancelRecognitionInProgress()
            return
        }
        if pendingPasteText == nil {
            _ = restoreLastInputConfirmation(peripheralID: peripheralID)
            return
        }
        guard activePeripheralID == peripheralID else { return }
        // P3 反馈环：应用后被取消 = 未保留，清锚不增 recall。
        appliedCorrectionPair = nil
        // 负信号采集（P1-1）：待确认文本被取消 + 停留时长 + 热词负样本记分。
        if case .waitingToPaste(let text) = pendingPasteState {
            recordHotwordNegative(fromText: text)
            recordUsageEvent(
                kind: UsageEventKind.pasteCanceled, text: text,
                dwellSeconds: Date().timeIntervalSince(pendingPasteEnteredAt)
            )
        }
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
    }

    private func cancelRecognitionInProgress() {
        // 负信号采集（P1-1）：识别中途取消/重录（无文本）。
        recordUsageEvent(kind: UsageEventKind.recognitionCanceled, text: nil)
        cancelStreamingRefinement()
        cancelAudioEndTimeout()
        asr.cancel()
        pendingPasteState = .idle
        finishRecognitionCycle()
        statusController.hideOverlay()
        statusController.setStatus("Ready")
        sendUIStateForActiveDevice("ready")
        mainInputState = .ready
    }

    private func cancelActiveCycleIfDeviceDisconnected() {
        // 断连设备的体感态清理（对齐 Windows CancelActiveCycleIfDeviceDisconnected）。
        clearAirMouseForInactiveDevices(reason: "disconnected")
        // 断连后不会再有 gateway_key 抬起沿：清连发 hold 防僵尸连发
        //（repeater 另有 maxHoldMs 兜底，此处即时收敛）。
        gatewayRepeater.reset()
        syncGatewayRepeatTimer()
        let disconnectedSubtitleKeys = subtitleCycles.keys.filter { !ble.isConnected($0.peripheralID) }
        for key in disconnectedSubtitleKeys {
            guard let cycle = subtitleCycles[key] else { continue }
            cycle.asr.cancel()
            cycle.debugAudioRecorder.discard()
            statusController.hideOverlay(deviceID: cycle.deviceID)
            clearActiveSubtitleSession(peripheralID: key.peripheralID, sessionID: key.sessionID)
            subtitleCycles.removeValue(forKey: key)
        }
        guard let activePeripheralID, !ble.isConnected(activePeripheralID) else { return }
        if waitingForAudioEnd {
            sendFinalOggChunkIfNeeded(recordingDuration: currentRecordingDuration)
            return
        }
        asr.cancel()
        pendingPasteState = .idle
        mainInputState = .ready
        debugAudioRecorder.discard()
        finishRecognitionCycle()
        statusController.hideOverlay()
        subtitleController.hideAll()
    }

    private func finishRecognitionCycle() {
        cancelAudioEndTimeout()
        asrStarted = false
        sentFinalAudioChunk = false
        pastedFinalText = false
        bufferedOggChunks.removeAll(keepingCapacity: true)
    }

    private func updateDeviceFirmwareInfo(event: StateEvent, peripheralID: UUID) {
        guard let deviceID = ble.deviceID(for: peripheralID) else { return }
        var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
        if let hardware = event.hardware {
            info.hardware = hardware
        }
        if let firmwareVersion = event.firmwareVersion {
            info.currentVersion = firmwareVersion
        }
        info.errorMessage = nil
        firmwareInfoByDeviceID[deviceID] = info
        refreshFirmwareAvailability()
    }

    private func startFirmwareManifestRefreshTimer() {
        firmwareManifestRefreshTimer?.invalidate()
        firmwareManifestRefreshTimer = Timer.scheduledTimer(withTimeInterval: 60 * 60, repeats: true) { [weak self] _ in
            self?.checkFirmwareUpdatesIfNeeded(force: false, showErrors: false)
        }
    }

    private func checkFirmwareUpdatesIfNeeded(force: Bool, showErrors: Bool) {
        if firmwareManifestCheckInFlight {
            return
        }
        if !force,
           let lastFirmwareManifestCheckAt,
           Date().timeIntervalSince(lastFirmwareManifestCheckAt) < firmwareManifestCacheDuration {
            refreshFirmwareAvailability()
            return
        }

        firmwareManifestCheckInFlight = true
        setFirmwareChecking(true)
        firmwareManifestClient.fetchManifest { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                self.firmwareManifestCheckInFlight = false
                self.setFirmwareChecking(false)
                switch result {
                case .success(let manifest):
                    NSLog("Firmware manifest version=\(manifest.version) hardware=\(manifest.hardware)")
                    self.lastFirmwareManifestCheckAt = Date()
                    self.latestFirmwareManifest = manifest
                    self.clearFirmwareErrors()
                    self.refreshFirmwareAvailability()
                case .failure(let error):
                    NSLog("Firmware manifest check failed: \(error.localizedDescription)")
                    if showErrors {
                        self.setFirmwareError(error.localizedDescription)
                    } else {
                        self.refreshFirmwareAvailability()
                    }
                }
            }
        }
    }

    private func refreshFirmwareAvailability() {
        for (deviceID, var info) in firmwareInfoByDeviceID {
            info.latestVersion = nil
            info.updateAvailable = false
            guard let manifest = latestFirmwareManifest else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            guard let hardware = info.hardware else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            guard let currentVersion = info.currentVersion else {
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            guard hardware == manifest.hardware else {
                NSLog("Firmware availability VS-\(deviceID) hardware=\(hardware) current=\(currentVersion) latest=\(manifest.version) update=false reason=hardware_mismatch manifest_hardware=\(manifest.hardware)")
                firmwareInfoByDeviceID[deviceID] = info
                continue
            }
            info.latestVersion = manifest.version
            info.updateAvailable = FirmwareVersion.isVersion(currentVersion, olderThan: manifest.version)
            firmwareInfoByDeviceID[deviceID] = info
            NSLog("Firmware availability VS-\(deviceID) hardware=\(hardware) current=\(currentVersion) latest=\(manifest.version) update=\(info.updateAvailable)")
            maybeShowFirmwareUpdatePromptAfterPairing(deviceID: deviceID, info: info)
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
    }

    private func maybeShowFirmwareUpdatePromptAfterPairing(deviceID: String, info: DeviceFirmwareInfo) {
        guard
            pendingFirmwareUpdatePromptDeviceIDs.contains(deviceID),
            let currentVersion = info.currentVersion,
            let latestVersion = info.latestVersion
        else {
            return
        }
        guard info.updateAvailable else {
            pendingFirmwareUpdatePromptDeviceIDs.remove(deviceID)
            return
        }
        pendingFirmwareUpdatePromptDeviceIDs.remove(deviceID)
        // 最低兼容版本优先用 manifest 下发的 min_version（可远端抬升门槛），
        // 缺失时回退本地常量（老 manifest/离线场景）。
        let minimumVersion = latestFirmwareManifest?.effectiveMinimumVersion(
            fallback: AppConfig.minimumCompatibleFirmwareVersion
        ) ?? AppConfig.minimumCompatibleFirmwareVersion
        let isBelowMinimum = FirmwareVersion.isVersion(
            currentVersion,
            olderThan: minimumVersion
        )
        DispatchQueue.main.async { [onFirmwareUpdatePrompt] in
            onFirmwareUpdatePrompt?(deviceID, currentVersion, latestVersion, isBelowMinimum)
        }
    }

    private func setFirmwareChecking(_ isChecking: Bool) {
        for deviceID in pairedDeviceIDs {
            var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
            info.isChecking = isChecking
            if isChecking {
                info.errorMessage = nil
            }
            firmwareInfoByDeviceID[deviceID] = info
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
    }

    private func clearFirmwareErrors() {
        for (deviceID, var info) in firmwareInfoByDeviceID {
            info.errorMessage = nil
            firmwareInfoByDeviceID[deviceID] = info
        }
    }

    private func setFirmwareError(_ message: String) {
        for deviceID in pairedDeviceIDs {
            var info = firmwareInfoByDeviceID[deviceID] ?? DeviceFirmwareInfo()
            info.errorMessage = message
            firmwareInfoByDeviceID[deviceID] = info
        }
        statusController.setFirmwareInfo(firmwareInfoByDeviceID)
    }

    private var activeSessionID: UInt32? {
        mainInputState.sessionID
    }

    private var activePeripheralID: UUID? {
        mainInputState.peripheralID
    }

    private var activeSessionStartedAt: Date? {
        mainInputState.startedAt
    }

    private func sendUIStateForActiveDevice(_ state: String, text: String = "") {
        ble.sendUIState(state, text: text, to: activePeripheralID)
    }

    private var activeDeviceID: String? {
        activePeripheralID.flatMap { ble.deviceID(for: $0) }
    }

    private func outputProfile(for deviceID: String?) -> OutputProfile {
        config.outputProfile(for: deviceID)
    }

    private func shouldUseDefiniteSegments(for profile: OutputProfile) -> Bool {
        profile.target == .subtitle && config.interactionMode == .clickToTalk
    }

    private func deviceID(for peripheralID: UUID) -> String? {
        ble.deviceID(for: peripheralID)
    }

    /// 调试录音文件名前缀（RC- / VS-，按设备类）。
    private func deviceNamePrefix(for peripheralID: UUID) -> String {
        ble.deviceClass(for: peripheralID) == .xiaomiRemote2Pro ? "RC-" : "VS-"
    }

    // MARK: - 全局热键（对齐 Windows HandleGlobalHotkeyPressed/Released）

    private var hotkeyIsDown = false
    private var hotkeyActiveDeviceID: String?
    private var nextHotkeyRequestID: UInt32 = 1

    /// 热键按下：目标 = 活跃设备（已连接）否则首台已连接 StickS3；
    /// 按住说话模式记录按下态等松开，点击说话模式固件侧按 down 翻转起停。
    public func handleGlobalHotkeyPressed() {
        guard !hotkeyIsDown else { return }
        let connected = ble.connectedStickDeviceIDs()
        let target: String?
        if let active = activeDeviceID, connected.contains(active) {
            target = active
        } else {
            target = connected.first
        }
        guard let targetDevice = target else {
            statusController.setStatus(config.pairedDeviceIDs.isEmpty
                ? "Hotkey: pair a VoiceStick first"
                : "Hotkey: VoiceStick not connected; press the main button to wake it")
            return
        }
        let requestID = nextHotkeyRequestID
        nextHotkeyRequestID &+= 1
        if config.interactionMode == .holdToTalk {
            hotkeyIsDown = true
            hotkeyActiveDeviceID = targetDevice
        }
        ble.sendRemoteButton(action: "down", deviceID: targetDevice, requestID: requestID)
        statusController.setStatus("Recording (hotkey) on VS-\(targetDevice)")
    }

    /// 热键松开：仅按住说话模式补发 remote_button_up。
    public func handleGlobalHotkeyReleased() {
        guard config.interactionMode == .holdToTalk else { return }
        guard hotkeyIsDown else { return }
        let target = hotkeyActiveDeviceID
        hotkeyIsDown = false
        hotkeyActiveDeviceID = nil
        if let target, ble.isConnected(deviceID: target) {
            let requestID = nextHotkeyRequestID
            nextHotkeyRequestID &+= 1
            ble.sendRemoteButton(action: "up", deviceID: target, requestID: requestID)
        }
    }
}
