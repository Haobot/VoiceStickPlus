import Foundation
import VoiceStickCore

// N1 S3：启动最早期把构建注入的内置凭据灌入 Core 槽位（load() 的 active 解析
// 与 bootstrapBuiltinDefaultsIfNeeded 均读槽；真实 key 文件永留 App）。
extension BuiltinSecrets {
    static func loadIntoCoreSlots() {
        BuiltInSecretSlots.volcengineAPIKey = volcengineAPIKey
        BuiltInSecretSlots.tencentSecretID = tencentSecretID
        BuiltInSecretSlots.tencentSecretKey = tencentSecretKey
        BuiltInSecretSlots.tencentAppid = tencentAppid
        BuiltInSecretSlots.llmAPIKey = llmAPIKey
        BuiltInSecretSlots.llmBaseURL = llmBaseURL
        BuiltInSecretSlots.llmModel = llmModel
    }
}
