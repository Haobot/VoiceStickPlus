import Foundation
import VoiceStickCore

// VoiceStick 无框架测试 runner（本机无 Xcode，XCTest/swift-testing 不可用）：
// 逐条调用各主题套件，最后汇总 PASSED x/y，有失败 exit(1)。
// 覆盖范围说明（N1 第三刀后更新）：normalizedDeviceID / paired CSV /
// XiaomiSettings / TOML 模型闭包（ConfigFile 家族 + xiaomi 转换器 + toml 助手）
// 已全部下沉 VoiceStickCore 并覆盖；interaction/encoder/buttons/output 转换器
//（依赖 App 域 KeySpec 等）与 AppConfig 全量 load/save 留在 app 模块待后刀。
print("NOTE: interaction/encoder/buttons/output 转换器与 AppConfig 全量 load/save 仍在 app 模块（TOML 模型闭包+xiaomi 映射已下沉并测）")

runAtvvProtocolTests()
runImaAdpcmDecoderTests()
let goldenChecked = runImaAdpcmGoldenFixtures()
runPcmPostprocessorTests()
runAudioOpusEncoderTests()
runAtvvSessionTests()
runF5PredicateTests()
runBleProtocolHelpersTests()
runRemoteButtonHIDTests()
runGatewaySupportTests()
runAirMouseKinTests()
runAirMouseProtocolTests()
runLlmSseParserTests()
runHotwordSelectorTests()
runActiveSecretTests()
runOtaFlowControlTests()
runProtoNegotiationTests()
runUiStateBudgetTests()
runGatewayKeymapChunkTests()
runContractFixtureTests()
runConfigParsingTests()
runOggMuxerTests()
runConfigFileModelTests()
runCoordinatorFsmTests()
runConfigDomainTests()

let passed = totalChecks - failedChecks
print("PASSED \(passed)/\(totalChecks) (golden fixtures: \(goldenChecked) session(s))")
if failedChecks > 0 {
    print("FAILED checks:")
    for name in failedNames { print("  - \(name)") }
    exit(1)
}
