import Foundation
import VoiceStickCore

/// 编译期内置凭据（对齐 Windows desktop/windows/src/builtin_secrets.h.in 机制）。
///
/// 本文件是**占位模板**（公开构建语义：全空，运行时正常读用户配置）。内测构建由
/// `scripts/build-macos.sh` 在编译前从本机 `~/Library/Application Support/
/// VoiceStick/config.toml`（可用 VOICESTICK_BUILTIN_CONFIG_SOURCE 覆盖）提取
/// 7 项字段重写本文件、构建完成后 `git checkout` 恢复占位（trap 兜底），
/// 构建日志出现 "Injecting built-in credentials into VoiceStickApp"——见
/// "building WITHOUT built-in credentials" 即为无凭据公开包（对齐 Windows
/// build-msi.bat 门禁措辞）。
///
/// 运行时语义（AppConfig.active*）：配置字段 trim 后非空用配置值，空则回退
/// 这里；不修改 config 字段、不落盘。凭据编译进二进制可被逆向提取，仅用于
/// 内测分发，勿用于公开发布（P0-4 扫描 --allow-builtin 仅告警）。
enum BuiltinSecrets {
    /// 火山引擎 ASR API Key（volcengine 模式）。
    static let volcengineAPIKey = ""

    /// 腾讯云 ASR 凭据（tencent 模式）。
    static let tencentSecretID = ""
    static let tencentSecretKey = ""
    static let tencentAppid = ""

    /// DeepSeek LLM 凭据（OpenAI 兼容；精修/翻译复用）。
    static let llmAPIKey = ""
    static let llmBaseURL = ""
    static let llmModel = ""

    /// 是否带可用 ASR 凭据（首启引导：有则跳过 onboarding 直接可用的 provider）。
    static var hasTencentCredentials: Bool {
        !tencentSecretID.isEmpty && !tencentSecretKey.isEmpty && !tencentAppid.isEmpty
    }

    static var hasVolcengineCredentials: Bool {
        !volcengineAPIKey.isEmpty
    }

    static var hasASRCredentials: Bool {
        hasTencentCredentials || hasVolcengineCredentials
    }

    /// 内置凭据覆盖的 provider（对齐 Windows config.template 默认 tencent）；
    /// 无任何内置时返回 nil（保持 .voiceStickCloud 原默认）。
    static var builtinProvider: ASRProvider? {
        if hasTencentCredentials { return .tencent }
        if hasVolcengineCredentials { return .volcengine }
        return nil
    }
}
