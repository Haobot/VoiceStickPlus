import Foundation

/// 交互模式（rawValue 是 TOML 配置与固件 interaction_mode 帧的线上格式，勿改字符串）。
/// 从 app 模块移入 core：XiaomiAtvvSession（core）与 AppConfig/StatusController（app）共用。
public enum InteractionMode: String {
    case holdToTalk = "hold_to_talk"
    case clickToTalk = "click_to_talk"

    public var displayName: String {
        switch self {
        case .holdToTalk:
            return "Hold to Talk"
        case .clickToTalk:
            return "Click to Talk"
        }
    }
}
