// N1 AppConfig 根刀 S3：内置凭据槽位——BuiltinSecrets（App 构建产物、含真实 key
// 的文件不进 Core）在启动最早期把 7 槽灌入本结构；active 解析与 bootstrap 读槽。
// 派生逻辑（has*/builtinProvider）与 BuiltinSecrets 同源拷贝（跨模块各自纯算）。

public enum ASRProvider: String {
    case voiceStickCloud = "voicestick_cloud"
    case volcengine
    case tencent

    public var displayName: String {
        switch self {
        case .voiceStickCloud:
            return "VoiceStick Cloud"
        case .volcengine:
            return "Volcengine"
        case .tencent:
            return "Tencent Cloud ASR"
        }
    }
}


public struct BuiltInSecretSlots {
    public static var volcengineAPIKey = ""
    public static var tencentSecretID = ""
    public static var tencentSecretKey = ""
    public static var tencentAppid = ""
    public static var llmAPIKey = ""
    public static var llmBaseURL = ""
    public static var llmModel = ""

    public static var hasTencentCredentials: Bool {
        !tencentSecretID.isEmpty && !tencentSecretKey.isEmpty && !tencentAppid.isEmpty
    }
    public static var hasVolcengineCredentials: Bool {
        !volcengineAPIKey.isEmpty
    }
    public static var hasASRCredentials: Bool {
        hasTencentCredentials || hasVolcengineCredentials
    }
    /// 内置凭据覆盖的 provider（对齐 Windows config.template 默认 tencent）。
    public static var builtinProvider: ASRProvider? {
        if hasTencentCredentials { return .tencent }
        if hasVolcengineCredentials { return .volcengine }
        return nil
    }
}
