import Foundation

/// Provider 下拉与 `ASRProvider` 的映射纯函数（对齐 Windows `provider_combo.h`，D4：
/// 索引映射抽成可测纯函数，UI 只做转发）。
///
/// 下拉项序（对齐 Windows SettingsDialog，`asr-settings-local-provider-merge.md` D1）：
/// `[VoiceStick Cloud 仅老配置仍为其时插入] + Volcengine + Tencent + 本地（恒末位）`。
/// 「本地语音识别（离线）」是**虚项**——不新增枚举值：选中 = `[local_asr] enabled=true`，
/// `asr_provider` 保留云端值（切回云端项无缝恢复密钥/热词配置）。
public enum ProviderPopupMapping {

    /// 本地项恒为下拉最后一项（Windows `ProviderComboLocalIndex` 同语义）。
    public static func isLocal(index: Int, itemCount: Int) -> Bool {
        itemCount > 0 && index >= 0 && index == itemCount - 1
    }

    /// 下拉项是否含老配置的 VoiceStick Cloud 前插项。
    public static func needsCloudLegacyItem(currentProvider: ASRProvider) -> Bool {
        currentProvider == .voiceStickCloud
    }

    /// 云端枚举在指定下拉索引处的取值；本地项（末位）返回 nil。
    /// 越界防御对齐 Windows `ProviderComboIsLocal` 的 CB_ERR 处理。
    public static func cloudProvider(
        at index: Int, itemCount: Int, hasCloudLegacy: Bool
    ) -> ASRProvider? {
        guard index >= 0, index < itemCount,
              !isLocal(index: index, itemCount: itemCount) else { return nil }
        if hasCloudLegacy {
            switch index {
            case 0: return .voiceStickCloud
            case 1: return .volcengine
            default: return .tencent
            }
        }
        return index == 0 ? .volcengine : .tencent
    }

    /// 云端枚举对应的下拉索引（本地项无映射，返回 nil）。
    public static func indexOf(
        _ provider: ASRProvider, itemCount: Int, hasCloudLegacy: Bool
    ) -> Int? {
        let offset = hasCloudLegacy ? 1 : 0
        let index: Int
        switch provider {
        case .voiceStickCloud:
            guard hasCloudLegacy else { return nil }
            index = 0
        case .volcengine: index = offset
        case .tencent: index = offset + 1
        }
        guard index < itemCount - 1 else { return nil }   // 末位是本地项
        return index
    }
}
