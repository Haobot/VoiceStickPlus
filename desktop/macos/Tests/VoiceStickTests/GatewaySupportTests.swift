import Foundation
import VoiceStickCore

/// 网关模式纯逻辑测试（P2 macOS 网关适配）：StateEvent 网关字段解析（gateway_key/
/// gateway_status/gateway_keymap/未知事件容忍）、路由表推导、目标机名规整、
/// payload 构造（字段名对齐 Doc/Ref/protocol.md 与 Windows ble_protocol.cc）。
func runGatewaySupportTests() {
    // ---- state_tx 帧构造辅助：4 字节帧头（0x01 0x10 + LE 长度）+ JSON ----
    func stateEvent(_ json: String) -> StateEvent? {
        let payload = Array(json.utf8)
        var frame = Data([0x01, 0x10,
                          UInt8(payload.count & 0xFF), UInt8((payload.count >> 8) & 0xFF)])
        frame.append(contentsOf: payload)
        return BleProtocol.parseStateEvent(frame)
    }

    // ---- gateway_key 解析（key + pressed 沿）----
    let keyDown = stateEvent("{\"event\":\"gateway_key\",\"key\":\"back\",\"pressed\":true}")
    checkEqual(keyDown?.gatewayKey, "back", "GatewayEvent.keyDownKey")
    checkEqual(keyDown?.gatewayPressed, true, "GatewayEvent.keyDownPressed")
    checkNil(keyDown?.gatewayMode, "GatewayEvent.keyDownNoMode")

    let keyUp = stateEvent("{\"event\":\"gateway_key\",\"key\":\"volume_up\",\"pressed\":false}")
    checkEqual(keyUp?.gatewayKey, "volume_up", "GatewayEvent.keyUpKey")
    checkEqual(keyUp?.gatewayPressed, false, "GatewayEvent.keyUpPressed")

    // ---- gateway_status / gateway_keymap / 未知事件容忍 ----
    let status = stateEvent("{\"event\":\"gateway_status\",\"mode\":\"gateway\"}")
    checkEqual(status?.gatewayMode, "gateway", "GatewayEvent.statusMode")
    checkEqual(stateEvent("{\"event\":\"gateway_status\",\"mode\":\"normal\"}")?.gatewayMode,
               "normal", "GatewayEvent.statusNormal")

    let routes = stateEvent("{\"event\":\"gateway_keymap\",\"routes\":[" +
        "{\"key\":\"back\",\"route\":\"software\"},{\"key\":\"ok\",\"route\":\"passthrough\"}]}")
    checkEqual(routes?.keymapRoutes?.count, 2, "GatewayEvent.routesCount")
    checkEqual(routes?.keymapRoutes?.first,
               GatewayKeymapRoute(key: "back", route: "software"), "GatewayEvent.routesFirst")

    let unknown = stateEvent("{\"event\":\"something_new\"}")
    checkEqual(unknown?.event, "something_new", "GatewayEvent.unknownTolerated")
    checkNil(unknown?.gatewayKey, "GatewayEvent.unknownNoKey")

    // ---- 路由表推导 ----
    let defaultRoutes = GatewaySupport.routes(for: .default)
    checkEqual(defaultRoutes.count, GatewaySupport.routableKeys.count, "GatewayRoutes.defaultCount13")
    check(defaultRoutes.allSatisfy { $0.route == "passthrough" }, "GatewayRoutes.defaultPassthrough")

    var settings = ButtonsSettings.default
    settings.setMapping(ButtonMapping(action: .key, key: "alt+left"), for: .back)
    settings.setMapping(ButtonMapping(action: .disabled, key: ""), for: .volUp)
    let customRoutes = GatewaySupport.routes(for: settings)
    checkEqual(customRoutes.first { $0.key == "back" }?.route, "software",
               "GatewayRoutes.keySoftware")
    checkEqual(customRoutes.first { $0.key == "volume_up" }?.route, "software",
               "GatewayRoutes.disabledSoftware")
    checkEqual(customRoutes.first { $0.key == "ok" }?.route, "passthrough",
               "GatewayRoutes.nativePassthrough")

    // 语音键不参与路由（ATVV 会话层固定语义）；macOS 无映射 UI 的键仍在全集内
    //（显式推 passthrough，避免继承上一目标的 software 路由）。
    check(!GatewaySupport.routableKeys.contains("voice_double_click"), "GatewayRoutes.voiceExcluded")
    check(GatewaySupport.routableKeys.contains("power") &&
          GatewaySupport.routableKeys.contains("volume_mute"), "GatewayRoutes.powerMuteIncluded")

    // ---- 目标机名规整 ----
    checkEqual(GatewaySupport.targetInfoName("MacBook Pro"), "MacBook Pro", "GatewayName.ascii")
    checkEqual(GatewaySupport.targetInfoName("  pad  "), "pad", "GatewayName.trimmed")
    checkNil(GatewaySupport.targetInfoName("   "), "GatewayName.blankNil")

    // 恰 23 字节（8 ASCII + 5×3B 汉字）不截断；26 字节截到 23（完整汉字边界）。
    let exact23 = "aaaaaaaa" + String(repeating: "中", count: 5)
    checkEqual(GatewaySupport.targetInfoName(exact23), exact23, "GatewayName.exact23Kept")
    let over26 = "aaaaaaaa" + String(repeating: "中", count: 6)
    checkEqual(GatewaySupport.targetInfoName(over26), exact23, "GatewayName.overTruncated")

    // 多字节名不被切碎：3 ASCII + 7×3B 汉字 = 24 字节 → 截后 3+18=21 字节。
    let multibyte = "abc" + String(repeating: "中", count: 7)
    checkEqual(GatewaySupport.targetInfoName(multibyte),
               "abc" + String(repeating: "中", count: 6), "GatewayName.multibyteBoundary")

    // ---- payload 构造（字段名对齐协议/Windows 参照）----
    let target = (try? JSONSerialization.jsonObject(
        with: BleProtocol.gatewayTargetInfoPayload(name: "Mac"))) as? [String: String]
    checkEqual(target?["event"], "gateway_target_info", "GatewayPayload.targetEvent")
    checkEqual(target?["name"], "Mac", "GatewayPayload.targetName")

    let keymap = (try? JSONSerialization.jsonObject(
        with: BleProtocol.gatewayKeymapSetPayload(key: "back", route: "software"))) as? [String: String]
    checkEqual(keymap?["event"], "gateway_keymap_set", "GatewayPayload.keymapEvent")
    checkEqual(keymap?["key"], "back", "GatewayPayload.keymapKey")
    checkEqual(keymap?["route"], "software", "GatewayPayload.keymapRoute")
}
