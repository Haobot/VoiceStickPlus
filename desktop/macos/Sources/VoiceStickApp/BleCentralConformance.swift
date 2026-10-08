import Foundation
import VoiceStickCore

// N1 切5 第二步：BleCentral 满足 VoiceStickBleServing（成员/默认参原样——省略形由
// 协议 extension 重载承接；协议 24 面与具体类一一对应）。
extension BleCentral: VoiceStickBleServing {}
