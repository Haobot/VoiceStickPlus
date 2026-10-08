import Foundation
import VoiceStickCore

// A15：gateway_keymap 分片回执（Doc/Ref/protocol.md gateway_keymap 章节）——
// 13 键全表 ≈470B 超 state_tx 单帧预算（ATT MTU 247 → JSON ≤240B），固件按
// seq/more 分片发送；本端 seq0 起累计、more=false 收口，旧式无 seq/more 单帧
// 即到即完整。
// A14：ui_state 帧预算（ATT MTU247-3=244B，protocol.md）——超长 text 按**字符边界**
// 截断（String.dropLast 不切 UTF-8）、state 恒完整；原发送端零校验 → 固件 512B 缓冲
// 截出半截 JSON → 整帧丢（设备屏卡 thinking）。
// D9：协议版本协商（protocol.md「Protocol version & negotiation」）——
// proto_info 解析 + proto_negotiate 构造 + 三端常量同源。
func runProtoNegotiationTests() {
    let infoJson = #"{"event":"proto_info","proto":1,"min_proto":1}"#
    let frame = stateFrameBytes(infoJson)
    let info = BleProtocol.parseStateEvent(frame)
    checkNotNil(info, "D9: proto_info 解析")
    check(info?.proto == BleProtocol.kProtocolVersion, "D9: proto == kProtocolVersion")
    check(info?.minProto == BleProtocol.kProtocolMinVersion, "D9: min_proto == kProtocolMinVersion")

    let payload = BleProtocol.protoNegotiatePayload()
    let sent = String(data: payload, encoding: .utf8) ?? ""
    check(sent.contains(#""event":"proto_negotiate""#), "D9: 上报帧事件名")
    check(sent.contains(#""proto":\#(BleProtocol.kProtocolVersion)"#), "D9: 上报帧值取自常量")

    // 非 proto_info 事件不带版本字段。
    let other = BleProtocol.parseStateEvent(stateFrameBytes(#"{"event":"gateway_status","mode":"gateway"}"#))
    check(other?.proto == nil && other?.minProto == nil, "D9: 非该事件无版本字段")
}

// D9/D14 共用的 state 帧构造（{1,0x10}+LE16+JSON）。
private func stateFrameBytes(_ json: String) -> Data {
    var d = Data([1, 0x10])
    let bytes = Array(json.utf8)
    d.append(UInt8(bytes.count & 0xff))
    d.append(UInt8((bytes.count >> 8) & 0xff))
    d.append(contentsOf: bytes)
    return d
}

func runUiStateBudgetTests() {
    let ok = BleProtocol.uiStatePayload(state: "thinking", text: "ok")
    let okStr = String(data: ok, encoding: .utf8) ?? ""
    check(okStr.contains(#""text":"ok""#), "A14: 正常帧不改")

    let asciiLong = BleProtocol.uiStatePayload(state: "thinking",
                                               text: String(repeating: "x", count: 500))
    check(asciiLong.count <= 244, "A14: ASCII 长文 ≤244")
    let asciiStr = String(data: asciiLong, encoding: .utf8) ?? ""
    check(asciiStr.contains(#""state":"thinking""#), "A14: ASCII 案 state 完整")
    check(asciiStr.hasSuffix(#""}"#), "A14: ASCII 案 JSON 收尾完整")

    // 中文长文：每轮整体重建，最终 Data 必仍可按 UTF-8 解码（证明没有切开码点）。
    let zhLong = BleProtocol.uiStatePayload(state: "recording",
                                            text: String(repeating: "热", count: 200))
    check(zhLong.count <= 244, "A14: 中文长文 ≤244")
    let zhStr = String(data: zhLong, encoding: .utf8)
    check(zhStr != nil, "A14: 中文截断后仍是合法 UTF-8")
    check((zhStr ?? "").contains(#""state":"recording""#), "A14: 中文案 state 完整")
}

func runGatewayKeymapChunkTests() {
    func stateFrame(_ json: String) -> Data {
        var d = Data([1, 0x10])
        let bytes = Array(json.utf8)
        d.append(UInt8(bytes.count & 0xff))
        d.append(UInt8((bytes.count >> 8) & 0xff))
        d.append(contentsOf: bytes)
        return d
    }

    // 分片0：more=true → 收不到完成。
    let c0json = #"{"event":"gateway_keymap","seq":0,"more":true,"routes":[{"key":"back","route":"software"}]}"#
    let c0 = BleProtocol.parseStateEvent(stateFrame(c0json))
    checkNotNil(c0, "A15: chunk0 解析")
    check(c0?.keymapSeq == 0 && c0?.keymapMore == true, "A15: chunk0 seq=0 more=true")
    check(c0?.keymapRoutes?.count == 1 && c0?.keymapRoutes?.first?.key == "back",
          "A15: chunk0 routes")

    // 分片1：more=false → 收口为完整表。
    let c1json = #"{"event":"gateway_keymap","seq":1,"more":false,"routes":[{"key":"menu","route":"passthrough"}]}"#
    let c1 = BleProtocol.parseStateEvent(stateFrame(c1json))
    check(c1?.keymapSeq == 1 && c1?.keymapMore == false, "A15: chunk1 seq=1 more=false")

    var pending: [GatewayKeymapRoute] = []
    if let c0 = c0 {
        check(!c0.accumulateKeymap(into: &pending), "A15: more=true 不收口")
        check(pending.count == 1, "A15: 累计1片")
    }
    if let c1 = c1 {
        check(c1.accumulateKeymap(into: &pending), "A15: more=false 收口")
        check(pending.count == 2 && pending[0].key == "back" && pending[1].key == "menu",
              "A15: 完整表两键且有序")
    }

    // seq0 重启未完成累计（帧丢失/新表自愈）。
    if let c0 = c0 {
        _ = c0.accumulateKeymap(into: &pending)
        check(!c0.accumulateKeymap(into: &pending) && pending.count == 1,
              "A15: seq0 重启累计")
    }

    // 旧式单帧（无 seq/more 字段）→ 即到即完整（向后兼容）。
    let legacyjson = #"{"event":"gateway_keymap","routes":[{"key":"back","route":"software"}]}"#
    if let legacy = BleProtocol.parseStateEvent(stateFrame(legacyjson)) {
        check(legacy.keymapSeq == nil && legacy.keymapMore == nil, "A15: 旧式单帧无分片字段")
        var legacyPending: [GatewayKeymapRoute] = []
        check(legacy.accumulateKeymap(into: &legacyPending) && legacyPending.count == 1,
              "A15: 旧式单帧即完整")
    } else {
        check(false, "A15: 旧式单帧解析失败")
    }
}
