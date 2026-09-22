import Foundation
import VoiceStickCore

// VoiceStick 无框架测试 runner（本机无 Xcode，XCTest/swift-testing 不可用）：
// 逐条调用各主题套件，最后汇总 PASSED x/y，有失败 exit(1)。
// 覆盖范围说明：AppConfig 模块内容（normalizedDeviceID 双前缀 / paired_devices
// CSV / [device.<id>.xiaomi] TOML）在 app 模块，runner 只链接 VoiceStickCore，
// 依赖不到、故不覆盖——这部分随旧 XCTest DeviceIDAndConfigTests 一并下线，
// 后续若需要应把对应纯逻辑下沉 core 再测。
print("NOTE: AppConfig 模块（normalizedDeviceID / paired CSV / xiaomi TOML）不在覆盖范围（runner 只链接 VoiceStickCore）")

runAtvvProtocolTests()
runImaAdpcmDecoderTests()
let goldenChecked = runImaAdpcmGoldenFixtures()
runPcmPostprocessorTests()
runAudioOpusEncoderTests()
runAtvvSessionTests()
runF5PredicateTests()
runBleProtocolHelpersTests()
runRemoteButtonHIDTests()

let passed = totalChecks - failedChecks
print("PASSED \(passed)/\(totalChecks) (golden fixtures: \(goldenChecked) session(s))")
if failedChecks > 0 {
    print("FAILED checks:")
    for name in failedNames { print("  - \(name)") }
    exit(1)
}
