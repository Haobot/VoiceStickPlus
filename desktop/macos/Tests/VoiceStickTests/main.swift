import Foundation
import VoiceStickCore

// VoiceStick 无框架测试 runner（本机无 Xcode，XCTest/swift-testing 不可用）：
// 逐条调用各主题套件，最后汇总 PASSED x/y，有失败 exit(1)。
// 覆盖范围说明（N1 第一刀后更新）：normalizedDeviceID 双前缀 / paired_devices CSV
// / XiaomiSettings 结构已下沉 VoiceStickCore（ConfigParsing.swift），由
// runConfigParsingTests 覆盖；[device.<id>.xiaomi] TOML 映射（依赖 DeviceConfigFile
// 模型闭包）与全量 load/save 仍在 app 模块、留下一刀下沉。
print("NOTE: xiaomi TOML 映射与 AppConfig 全量 load/save 仍在 app 模块未覆盖（identity+paired CSV+XiaomiSettings 已下沉并测）")

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

let passed = totalChecks - failedChecks
print("PASSED \(passed)/\(totalChecks) (golden fixtures: \(goldenChecked) session(s))")
if failedChecks > 0 {
    print("FAILED checks:")
    for name in failedNames { print("  - \(name)") }
    exit(1)
}
