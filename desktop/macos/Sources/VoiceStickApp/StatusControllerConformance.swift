import Foundation
import VoiceStickCore

// N1 切5：StatusController 满足 VoiceStickStatusSink（方法/默认参原样——默认值不参与
// 协议匹配，省略形由协议 extension 重载承接）。协调器成员已换协议类型。
extension StatusController: VoiceStickStatusSink {}
