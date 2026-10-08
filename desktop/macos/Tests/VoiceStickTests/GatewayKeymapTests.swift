import Foundation
import VoiceStickCore

// A15：gateway_keymap 分片回执（Doc/Ref/protocol.md gateway_keymap 章节）——
// 13 键全表 ≈470B 超 state_tx 单帧预算（ATT MTU 247 → JSON ≤240B），固件按
// seq/more 分片发送；本端 seq0 起累计、more=false 收口，旧式无 seq/more 单帧
// 即到即完整。
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
