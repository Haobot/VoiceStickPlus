import Foundation
import VoiceStickCore

// 跨端契约测试（tests/contract/README.md）：消费 fixtures/manifest.json 的黄金
// 字节，用本端解析器/构建器对拍期望。键序不构成契约（JSONSerialization 键序不
// 保证），control 组比对对象语义；expect 只取两端公共字段（单端缺口见 README）。

private func contractManifestURL() -> URL {
    TestSupport.repoRoot.appendingPathComponent("tests/contract/fixtures/manifest.json")
}

private func unhex(_ hex: String) -> Data? {
    var data = Data()
    var idx = hex.startIndex
    while idx < hex.endIndex {
        let next = hex.index(idx, offsetBy: 2)
        guard next <= hex.endIndex, let b = UInt8(hex[idx..<next], radix: 16) else { return nil }
        data.append(b)
        idx = next
    }
    return data
}

private func hexOf(_ data: Data) -> String {
    data.map { String(format: "%02x", $0) }.joined()
}

private func num(_ v: Any?) -> NSNumber? { v as? NSNumber }

/// expect 键 → StateEvent 字段映射（线上字段名）；未映射键即失败（防静默漏断言）。
private func checkStateExpect(_ name: String, _ expect: [String: Any], _ ev: StateEvent) {
    for (key, value) in expect {
        switch key {
        case "event":
            check(ev.event == (value as? String), "\(name).event")
        case "hardware":
            check(ev.hardware == (value as? String), "\(name).hardware")
        case "firmware_version":
            check(ev.firmwareVersion == (value as? String), "\(name).firmware_version")
        case "button":
            check(ev.button == (value as? String), "\(name).button")
        case "source":
            check(ev.source == (value as? String), "\(name).source")
        case "direction":
            check(ev.direction == (value as? String), "\(name).direction")
        case "key":
            check(ev.gatewayKey == (value as? String), "\(name).key")
        case "mode":
            // gateway_status.mode 线上值 "gateway"/"normal"；macOS 保字符串。
            check(ev.gatewayMode == (value as? String), "\(name).mode")
        case "session_id":
            check(ev.sessionID != nil && ev.sessionID == num(value)?.uint32Value,
                  "\(name).session_id")
        case "duration_ms":
            check(ev.durationMs != nil && ev.durationMs == num(value)?.uint32Value,
                  "\(name).duration_ms")
        case "steps":
            check(ev.steps != nil && ev.steps == num(value)?.uint32Value, "\(name).steps")
        case "level":
            check(ev.batteryLevel != nil && ev.batteryLevel == num(value)?.intValue,
                  "\(name).level")
        case "present":
            check(ev.encoderPresent != nil && ev.encoderPresent == num(value)?.boolValue,
                  "\(name).present")
        case "charging":
            check(ev.batteryCharging != nil && ev.batteryCharging == num(value)?.boolValue,
                  "\(name).charging")
        case "usb_powered":
            check(ev.batteryUsbPowered != nil && ev.batteryUsbPowered == num(value)?.boolValue,
                  "\(name).usb_powered")
        case "pressed":
            check(ev.gatewayPressed != nil && ev.gatewayPressed == num(value)?.boolValue,
                  "\(name).pressed")
        default:
            check(false, "\(name): 未映射的 expect 键 \(key)")
        }
    }
}

/// control 构建器分发（args → 本端 payload）。两端缺一边的构建器不入公共样本。
private func buildControlPayload(_ kind: String, _ args: [String: Any]) -> Data? {
    let s = { (k: String) -> String in args[k] as? String ?? "" }
    let b = { (k: String) -> Bool in (args[k] as? NSNumber)?.boolValue ?? false }
    let i = { (k: String) -> Int in (args[k] as? NSNumber)?.intValue ?? 0 }
    switch kind {
    case "ui_state":
        return BleProtocol.uiStatePayload(state: s("state"), text: s("text"))
    case "interaction_mode":
        guard let mode = InteractionMode(rawValue: s("mode")) else { return nil }
        return BleProtocol.interactionModePayload(mode)
    case "show_imu_debug":
        return BleProtocol.showIMUDebugPayload(enabled: b("enabled"))
    case "imu_wake_sensitivity":
        return BleProtocol.imuWakeSensitivityPayload(thresholdLsb: i("threshold"))
    case "tap_enabled":
        return BleProtocol.tapEnabledPayload(enabled: b("enabled"))
    case "tap_sensitivity":
        return BleProtocol.tapSensitivityPayload(level: i("level"))
    case "encoder_led_color":
        return BleProtocol.encoderLedColorPayload(color: s("color"))
    case "encoder_recording_gate":
        return BleProtocol.encoderRecordingGatePayload(enabled: b("enabled"))
    case "gateway_keymap_set":
        return BleProtocol.gatewayKeymapSetPayload(key: s("key"), route: s("route"))
    case "gateway_target_info":
        return BleProtocol.gatewayTargetInfoPayload(name: s("name"))
    case "air_mouse_enabled":
        return BleProtocol.airMouseEnabledPayload(enabled: b("enabled"))
    case "usb_auto_off":
        return BleProtocol.usbAutoOffPayload(enabled: b("enabled"))
    case "battery_status_request":
        return BleProtocol.batteryStatusRequestPayload()
    case "remote_button":
        return BleProtocol.remoteButtonPayload(action: s("action"), button: s("button"),
                                               source: s("source"),
                                               requestID: UInt32(truncating: num(args["request_id"])!))
    case "power_log_dump":
        return BleProtocol.powerLogDumpPayload(offset: UInt32(truncating: num(args["offset"])!),
                                               max: UInt32(truncating: num(args["max"])!))
    case "power_log_clear":
        return BleProtocol.powerLogClearPayload()
    default:
        return nil
    }
}

func runContractFixtureTests() {
    guard let data = try? Data(contentsOf: contractManifestURL()),
          let manifest = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
        check(false, "contract: manifest.json 无法读取（tests/contract/fixtures/）")
        return
    }

    func frames(_ key: String) -> [[String: Any]] {
        manifest[key] as? [[String: Any]] ?? []
    }

    // 1) state 事件：黄金字节 → parseStateEvent → 公共字段期望。
    for item in frames("state_frames") {
        let name = item["name"] as? String ?? "?"
        guard let bytes = unhex(item["hex"] as? String ?? ""),
              let ev = BleProtocol.parseStateEvent(bytes),
              let expect = item["expect"] as? [String: Any] else {
            check(false, "contract state \(name): hex/parse/expect 失败")
            continue
        }
        checkStateExpect(name, expect, ev)
    }

    // 2) power_mgmt：独立解析器。
    for item in frames("power_mgmt_frames") {
        let name = item["name"] as? String ?? "?"
        guard let bytes = unhex(item["hex"] as? String ?? ""),
              let pm = BleProtocol.parsePowerMgmtEvent(bytes),
              let expect = item["expect"] as? [String: Any] else {
            check(false, "contract power_mgmt \(name): hex/parse/expect 失败")
            continue
        }
        let want = expect["usb_auto_off"] as? Bool
        check(pm.usbAutoOff == want, "contract power_mgmt \(name).usb_auto_off")
    }

    // 3) OTA state 五态。
    for item in frames("ota_state_frames") {
        let name = item["name"] as? String ?? "?"
        guard let bytes = unhex(item["hex"] as? String ?? ""),
              let ota = BleProtocol.parseFirmwareOTAStateEvent(bytes),
              let expect = item["expect"] as? [String: Any] else {
            check(false, "contract ota_state \(name): hex/parse/expect 失败")
            continue
        }
        check(ota.event == (expect["event"] as? String), "contract ota \(name).event")
        if let v = expect["transfer_id"] {
            check(ota.transferID == num(v)?.uint32Value, "contract ota \(name).transfer_id")
        }
        if let v = expect["written"] {
            check(ota.written == num(v)?.uint32Value, "contract ota \(name).written")
        }
        if let v = expect["size"] {
            check(ota.size == num(v)?.uint32Value, "contract ota \(name).size")
        }
        if let v = expect["code"] {
            check(ota.code == (v as? String), "contract ota \(name).code")
        }
        if let v = expect["esp_err"] {
            check(ota.espErr == num(v)?.intValue, "contract ota \(name).esp_err")
        }
        if let v = expect["reboot_ms"] {
            check(ota.rebootMs == num(v)?.intValue, "contract ota \(name).reboot_ms")
        }
    }

    // 4) 二进制 audio / motion。
    for item in frames("binary_frames") {
        let name = item["name"] as? String ?? "?"
        let kind = item["kind"] as? String ?? ""
        guard let bytes = unhex(item["hex"] as? String ?? ""),
              let expect = item["expect"] as? [String: Any] else {
            check(false, "contract binary \(name): hex/expect 失败")
            continue
        }
        if kind == "audio" {
            guard let audio = BleProtocol.parseAudioFrame(bytes) else {
                check(false, "contract audio \(name): parse 失败")
                continue
            }
            check(audio.sessionID == num(expect["session_id"])?.uint32Value,
                  "contract audio \(name).session_id")
            check(audio.seq == num(expect["seq"])?.uint32Value, "contract audio \(name).seq")
            check(audio.flags == (num(expect["flags"])?.uint8Value ?? 0xFF),
                  "contract audio \(name).flags")
            if let want = expect["payload_hex"] as? String {
                check(hexOf(audio.payload) == want, "contract audio \(name).payload")
            }
        } else if kind == "motion" {
            guard let motion = BleProtocol.parseMotionFrame(bytes) else {
                check(false, "contract motion \(name): parse 失败")
                continue
            }
            check(Int(motion.dx) == (num(expect["dx"])?.intValue ?? 1 << 30),
                  "contract motion \(name).dx")
            check(Int(motion.dy) == (num(expect["dy"])?.intValue ?? 1 << 30),
                  "contract motion \(name).dy")
        } else {
            check(false, "contract binary \(name): 未知 kind \(kind)")
        }
    }

    // 5) control 构建 → 对象语义比对（键序无关）。
    for item in frames("control_payloads") {
        let name = item["name"] as? String ?? "?"
        let kind = item["kind"] as? String ?? ""
        guard let args = item["args"] as? [String: Any],
              let expect = item["expect"] as? [String: Any],
              let built = buildControlPayload(kind, args),
              let actualObj = try? JSONSerialization.jsonObject(with: built) else {
            check(false, "contract control \(name): 构建/解析失败")
            continue
        }
        let actualDict = actualObj as? NSDictionary
        check(actualDict?.isEqual(expect) == true,
              "contract control \(name): 构建输出与期望不符 " +
              (actualDict.map { "\($0)" } ?? "nil"))
    }

    // 6) OTA 控制二进制帧：整帧字节相等。
    for item in frames("ota_control_frames") {
        let name = item["name"] as? String ?? "?"
        let kind = item["kind"] as? String ?? ""
        guard let args = item["args"] as? [String: Any],
              let want = unhex(item["hex"] as? String ?? "") else {
            check(false, "contract ota_control \(name): args/hex 失败")
            continue
        }
        let built: Data?
        switch kind {
        case "ota_begin":
            built = BleProtocol.otaBeginPayload(imageSize: UInt32(truncating: num(args["image_size"])!),
                                                transferID: UInt32(truncating: num(args["transfer_id"])!))
        case "ota_data":
            guard let chunk = unhex(args["chunk_hex"] as? String ?? "") else {
                check(false, "contract ota_control \(name): chunk_hex 失败")
                continue
            }
            built = BleProtocol.otaDataPayload(transferID: UInt32(truncating: num(args["transfer_id"])!),
                                               offset: UInt32(truncating: num(args["offset"])!),
                                               chunk: chunk)
        case "ota_end":
            built = BleProtocol.otaEndPayload(transferID: UInt32(truncating: num(args["transfer_id"])!),
                                              imageSize: UInt32(truncating: num(args["image_size"])!))
        case "ota_abort":
            built = BleProtocol.otaAbortPayload(transferID: UInt32(truncating: num(args["transfer_id"])!))
        default:
            built = nil
        }
        guard let built = built else {
            check(false, "contract ota_control \(name): 未知 kind \(kind)")
            continue
        }
        check(built == want,
              "contract ota_control \(name): 字节不符 got=\(hexOf(built)) want=\(hexOf(want))")
    }

    let total = frames("state_frames").count + frames("power_mgmt_frames").count +
        frames("ota_state_frames").count + frames("binary_frames").count +
        frames("control_payloads").count + frames("ota_control_frames").count
    print("contract fixtures: \(total) 样本已对拍（manifest @ tests/contract/fixtures/）")
}
