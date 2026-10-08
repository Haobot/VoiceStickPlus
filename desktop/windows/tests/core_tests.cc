#include "air_mouse_kin.h"
#include "app_config.h"
#include "asr_client_tencent.h"
#include "asr_protocol.h"
#include "audio_opus_decoder.h"
#include "audio_opus_encoder.h"
#include "ble_protocol.h"
#include "test_suites.h"  // N8: suite registry for split test files
#include "ima_adpcm_decoder.h"
#include "pcm_postprocessor.h"
#include "xiaomi_atvv_protocol.h"
#include "xiaomi_atvv_session.h"
#include "xiaomi_buttons.h"
#include "xiaomi_keymap_interceptor.h"
#include "xiaomi_usage_tap.h"
#include "xiaomi_usage_tap_decoder.h"
#include "xiaomi_usage_tap_host.h"
#include "xiaomi_usage_tap_manager.h"
#include "xiaomi_usage_tap.h"
#include "cmd_line.h"
#include "com_port_selector.h"

#include <opus.h>
#include "byte_utils.h"
#include "clipboard_vault.h"
#include "cJSON.h"
#include "esptool_flash_command.h"
#include "esptool_progress.h"
#include "firmware_manifest.h"
#include "hotword_extractor.h"
#include "hotword_candidate_miner.h"
#include "hotword_selector.h"
#include "tencent_asr_vocab_client.h"
#include "key_spec.h"
#include "llm_refinement_client.h"
#include "log.h"
#include "localization.h"
#include "ogg_opus_muxer.h"
#include "ogg_opus_demuxer.h"
#include "local_asr_client_win.h"
#include "local_refinement_client.h"
#include "model_manifest.h"
#include "model_downloader.h"
#include "model_download_session.h"
#include "voice_stick_cloud_api_win.h"
#ifdef VOICESTICK_LOCAL_REFINE_ENABLED
#include "llama_cpp_engine.h"
#endif
#include "text_refiner.h"
#include "pinyin_guard.h"
#include "refine_history.h"
#include "mic_capture.h"
#include "license.h"
#include "serial_base32.h"
#include "wasapi_mic_capture.h"
#include "push_to_talk_key.h"
#include "provider_combo.h"
#include "selection_correction.h"
#include "shortcut_capture.h"
#include "onboarding_dialog.h"
#include "pair_device_helper.h"
#include "pcm_ring_buffer.h"
#include "power_log_monitor.h"
#include "voice_stick_coordinator.h"
#include "voice_stick_flash_tool.h"
#include "wasapi_render_sink.h"
#include "wasapi_virtual_mic_renderer.h"
#include "wechat_input_method_hotkey.h"
#include "default_audio_device_controller.h"
#include "device_switch_state.h"
#include "debug_audio_recorder.h"
#include "encoder_speed.h"

#include <algorithm>
#include <winsock2.h>
#include <windows.h>  // SEH 探针（AddVectoredExceptionHandlerFirst）；置于 winsock2 之后避免 winsock 冲突
#include <bcrypt.h>
#include <thread>
#include <utility>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cctype>
#include <cmath>
#include <crtdbg.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <future>
#include <optional>
#include <atomic>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace voicestick;

namespace {


#include "test_support.h"


void TestDeviceIds() {
    assert(BleProtocol::NormalizeDeviceId("vs-c3d8") == "C3D8");
    assert(BleProtocol::NormalizeDeviceId("09af") == "09AF");
    assert(!BleProtocol::DeviceIdFromName("Other").has_value());
    assert(BleProtocol::DeviceIdFromName("VS-C3D8").value() == "C3D8");

    const ByteVector complete_name_ad = {0x02, 0x01, 0x06, 0x08, 0x09, 'V', 'S', '-', 'C', '3', 'D', '8'};
    assert(BleProtocol::LocalNameFromAdvertisementData(complete_name_ad).value() == "VS-C3D8");
    const ByteVector shortened_name_ad = {0x08, 0x08, 'V', 'S', '-', 'A', '1', 'B', '2'};
    assert(BleProtocol::LocalNameFromAdvertisementData(shortened_name_ad).value() == "VS-A1B2");
    const ByteVector malformed_ad = {0x08, 0x09, 'V', 'S'};
    assert(!BleProtocol::LocalNameFromAdvertisementData(malformed_ad).has_value());
    const ByteVector service_uuid_ad = {
        0x02, 0x01, 0x06,
        0x11, 0x07,
        0x00, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
        0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f,
    };
    assert(BleProtocol::HasVoiceStickServiceUuid(service_uuid_ad));
    assert(!BleProtocol::HasVoiceStickServiceUuid(complete_name_ad));
    assert(BleProtocol::DeviceIdFromBluetoothAddress(0xAABBCCDDEEFF) == "EEFF");
}

void TestPlanReconnectAfterConnectFailure() {
    using namespace std::chrono_literals;
    const auto cooldown = 5s;
    const std::string_view subscribe_timeout =
        "atvv control subscribe timeout after 2500ms";

    // 用户主动取消（CancelPendingConnect → fail(kConnectFailureReasonCancelled)）：不得自动重连。
    const auto cancelled_plan = BleProtocol::PlanReconnectAfterConnectFailure(
        kConnectFailureReasonCancelled, true, false, cooldown);
    assert(!cancelled_plan.schedule);

    // 失败时设备已被忘记（paired 集合不再包含）：重连只会对着空座位喊话。
    const auto unpaired_plan = BleProtocol::PlanReconnectAfterConnectFailure(
        subscribe_timeout, false, false, cooldown);
    assert(!unpaired_plan.schedule);

    // 常规失败：仍配对则入队主动重连，等待一个失败退避期再由心跳发起。
    const auto normal_plan = BleProtocol::PlanReconnectAfterConnectFailure(
        subscribe_timeout, true, false, cooldown);
    assert(normal_plan.schedule);
    assert(normal_plan.delay == cooldown);

    // 僵尸拆链免退避窗口：与失败退避互斥，入队后立即到期，心跳下一跳即重试。
    const auto zombie_plan = BleProtocol::PlanReconnectAfterConnectFailure(
        subscribe_timeout, true, true, cooldown);
    assert(zombie_plan.schedule);
    assert(zombie_plan.delay == 0ms);

    // 取消语义优先级最高：即便设备仍配对、处于僵尸免退避窗口也不重试。
    const auto cancelled_zombie_plan = BleProtocol::PlanReconnectAfterConnectFailure(
        kConnectFailureReasonCancelled, true, true, cooldown);
    assert(!cancelled_zombie_plan.schedule);
}

void TestPlanZombieRecovery() {
    constexpr std::int64_t kTimeout = 90000;
    using Action = ZombieRecoveryAction;

    // 链路属性已翻 Disconnected：普通断连，重扫即可（与静默时长无关）。
    assert(BleProtocol::PlanZombieRecovery(true, -1, kTimeout, true) == Action::kScanOnly);
    assert(BleProtocol::PlanZombieRecovery(true, kTimeout * 10, kTimeout, true) == Action::kScanOnly);

    // 从未收到任何入站流量（silent_ms < 0）：按既有语义不判僵尸。
    assert(BleProtocol::PlanZombieRecovery(false, -1, kTimeout, true) == Action::kNone);

    // 恰好等于超时阈值：尚未越界，继续探测（边界与既有 silent_ms > timeout 一致）。
    assert(BleProtocol::PlanZombieRecovery(false, kTimeout, kTimeout, true) == Action::kNone);

    // 越界且链路仍报 Connected ⇒ 僵尸会话，VS 设备需要系统级重新配对。
    assert(BleProtocol::PlanZombieRecovery(false, kTimeout + 1, kTimeout, true) == Action::kRepairBond);

    // 小米遥控器不走重配路径（无系统级 bond 概念）：只重扫，不打扰用户。
    assert(BleProtocol::PlanZombieRecovery(false, kTimeout + 1, kTimeout, false) == Action::kScanOnly);
}

void TestPlanZombieHeal() {
    using Level = ZombieHealLevel;
    constexpr int kLightMax = kZombieLightAttemptsBeforeRepair;
    constexpr int kFullMax = kZombieMaxFullRepairsPerEpisode;

    // 小米遥控器（无系统级 bond）：永远停在轻量重连，不碰系统配对。
    assert(BleProtocol::PlanZombieHeal(0, 0, false) == Level::kLightReconnect);
    assert(BleProtocol::PlanZombieHeal(kLightMax, 0, false) == Level::kLightReconnect);
    assert(BleProtocol::PlanZombieHeal(kLightMax * 4, kFullMax * 4, false) == Level::kLightReconnect);

    // VS 设备首次判僵尸：先走零副作用的轻量重连（A），不动系统配对。
    assert(BleProtocol::PlanZombieHeal(0, 0, true) == Level::kLightReconnect);

    // 边界：用满轻量重连次数那一次才升级（< kLightMax 仍为轻量）。
    assert(BleProtocol::PlanZombieHeal(kLightMax - 1, 0, true) == Level::kLightReconnect);
    assert(BleProtocol::PlanZombieHeal(kLightMax, 0, true) == Level::kFullRepair);

    // 全量修复已用满：即便轻量次数更多也转用户处理，不再反复 radio reset。
    assert(BleProtocol::PlanZombieHeal(kLightMax * 3, kFullMax, true) == Level::kUserAction);

    // 计数已越界（不应发生，但必须稳定不越权）：仍停在用户处理而不是回到修复。
    assert(BleProtocol::PlanZombieHeal(kLightMax * 3, kFullMax + 5, true) == Level::kUserAction);
}

void TestPlanAfterOsBondAttempt() {
    using Follow = OsBondFollowUp;

    // 已配对（含"本来就配过对"的幂等路径）：两类设备都直接继续。
    assert(BleProtocol::PlanAfterOsBondAttempt(true, true) == Follow::kContinue);
    assert(BleProtocol::PlanAfterOsBondAttempt(true, false) == Follow::kContinue);

    // 未配对 + 硬前置（小米遥控器）：中止报错——ATVV GATT 没有 bond 连上也没用。
    assert(BleProtocol::PlanAfterOsBondAttempt(false, true) == Follow::kAbortWithError);

    // 未配对 + 软前置（VS 设备）：降级继续——app 自身 GATT 不需要 bond，
    // 只有 HOGP 按键直通需要，不能因系统侧失败把语音也一起卡死。
    assert(BleProtocol::PlanAfterOsBondAttempt(false, false) == Follow::kContinueWithWarning);
}

void TestPairDeviceHelpers() {
    assert(ParseManualPairDeviceId("abcd").value() == "ABCD");
    assert(ParseManualPairDeviceId("VS-abcd").value() == "ABCD");
    assert(ParseManualPairDeviceId(" vs-09af ").value() == "09AF");
    assert(ParseManualPairDeviceId("RC-3a7f").value() == "3A7F");
    assert(!ParseManualPairDeviceId("VS-123").has_value());
    assert(!ParseManualPairDeviceId("VoiceStick").has_value());

    // 异步配对消息的地址匹配：陈旧消息（上一目标迟到回调）地址不符即丢弃。
    assert(MatchesPendingPairAddress(0xAABBCCDDEEFF, 0xAABBCCDDEEFF));
    assert(!MatchesPendingPairAddress(0x112233445566, 0xAABBCCDDEEFF));
    assert(!MatchesPendingPairAddress(0, 0xAABBCCDDEEFF));
    assert(!MatchesPendingPairAddress(0xAABBCCDDEEFF, 0));

    PairingCandidate ready;
    ready.device_id = "C3D8";
    ready.display_name = "VS-C3D8";
    ready.bluetooth_address = 0xAABBCCDDEEFF;
    ready.id_source = PairingCandidateIdSource::kName;
    assert(CandidateDisplayTitle(ready) == "VS-C3D8");
    assert(CanPairCandidate(ready));

    PairingCandidate existing = ready;
    existing.is_existing_device = true;
    assert(CandidateDisplayTitle(existing) == "VS-C3D8 (paired)");
    assert(!CanPairCandidate(existing));

    PairingCandidate temporary = ready;
    temporary.device_id = "EEFF";
    temporary.display_name.clear();
    temporary.id_source = PairingCandidateIdSource::kAddressFallback;
    temporary.is_temporary_candidate = true;
    assert(CandidateDisplayTitle(temporary) == "VoiceStick (waiting for name)");
    assert(!CanPairCandidate(temporary));

    std::vector<PairingCandidate> candidates;
    MergePairingCandidate(&candidates, temporary);
    assert(candidates.size() == 1);
    assert(VisiblePairingCandidates(candidates, {}, 1000, 3000).empty());
    ready.bluetooth_address = 0x112233445566;
    ready.device_id = temporary.device_id;
    ready.display_name = "VS-EEFF";
    MergePairingCandidate(&candidates, ready);
    assert(candidates.size() == 1);
    assert(!candidates.front().is_temporary_candidate);
    assert(candidates.front().display_name == "VS-EEFF");

    PairingCandidate late_temporary = temporary;
    late_temporary.bluetooth_address = 0x66778899AABB;
    MergePairingCandidate(&candidates, late_temporary);
    assert(candidates.size() == 1);
    assert(!candidates.front().is_temporary_candidate);

    candidates.push_back(late_temporary);
    std::vector<RetainedPairingCandidate> retained;
    RetainNamedPairingCandidate(&retained, candidates.front(), 1000);
    const auto visible = VisiblePairingCandidates(candidates, retained, 2000, 3000);
    assert(visible.size() == 1);
    assert(!visible.front().is_temporary_candidate);

    std::vector<PairingCandidate> temporary_only{late_temporary};
    const auto retained_visible = VisiblePairingCandidates(temporary_only, retained, 2500, 3000);
    assert(retained_visible.size() == 1);
    assert(retained_visible.front().display_name == "VS-EEFF");
    assert(VisiblePairingCandidates(temporary_only, retained, 5001, 3000).empty());

    // 回归用例：固件名称在 SCAN_RSP、ADV 只带 service UUID（见 voice_ble.c），
    // 同一物理地址会交替出现命名候选（广播名 VS-D63C）与临时候选（MAC 低位
    // VS-D63E）。同地址合并时命名候选必须优先，后到的临时包不得覆盖已确认的
    // 命名候选——否则用户看到列表是 D63C、点配对取到的却是临时候选 D63E，
    // 配对对话框命中 "waiting for name" 分支不发起连接，设备卡 Pairing。
    {
        std::vector<PairingCandidate> merged;
        PairingCandidate named;
        named.bluetooth_address = 0x70041DD5D63E;
        named.device_id = "D63C";
        named.display_name = "VS-D63C";
        named.id_source = PairingCandidateIdSource::kName;
        named.rssi = -68;
        MergePairingCandidate(&merged, named);

        PairingCandidate later_temporary = named;
        later_temporary.device_id = "D63E";
        later_temporary.display_name.clear();
        later_temporary.id_source = PairingCandidateIdSource::kAddressFallback;
        later_temporary.is_temporary_candidate = true;
        later_temporary.rssi = -66;
        MergePairingCandidate(&merged, later_temporary);

        assert(merged.size() == 1);
        assert(!merged.front().is_temporary_candidate);
        assert(merged.front().device_id == "D63C");
        assert(merged.front().display_name == "VS-D63C");

        // 反向到达顺序：先临时后命名，命名应正常替换临时。
        std::vector<PairingCandidate> merged_reverse;
        MergePairingCandidate(&merged_reverse, later_temporary);
        MergePairingCandidate(&merged_reverse, named);
        assert(merged_reverse.size() == 1);
        assert(!merged_reverse.front().is_temporary_candidate);
        assert(merged_reverse.front().device_id == "D63C");
    }

    // OS bond 清理按地址匹配 DeviceInformation：解析 Windows 地址属性字符串。
    // 真机事实（2026-09-07 probe）：System.DeviceInterface.Bluetooth.DeviceAddress
    // 为无分隔符 12 位十六进制（"c05d39c36459"），System.Devices.Aep.DeviceAddress
    // 为冒号分隔（"c0:5d:39:c3:64:59"），两种格式都必须可解析——首版只认分隔
    // 格式导致全部记录跳过、忘记设备假成功、系统列表残留。
    assert(ParseBluetoothAddressString("AA:BB:CC:DD:EE:FF").value() == 0xAABBCCDDEEFFull);
    assert(ParseBluetoothAddressString("aa:bb:cc:dd:ee:ff").value() == 0xAABBCCDDEEFFull);
    assert(ParseBluetoothAddressString("  AA:BB:CC:DD:EE:FF  ").value() == 0xAABBCCDDEEFFull);
    assert(ParseBluetoothAddressString("00:00:00:00:00:00").value() == 0ull);
    assert(ParseBluetoothAddressString("AA-BB-CC-DD-EE-FF").value() == 0xAABBCCDDEEFFull);
    assert(ParseBluetoothAddressString("c05d39c36459").value() == 0xC05D39C36459ull);
    assert(ParseBluetoothAddressString("C05D39C36459").value() == 0xC05D39C36459ull);
    assert(ParseBluetoothAddressString(" c05d39c36459 ").value() == 0xC05D39C36459ull);
    assert(ParseBluetoothAddressString("000000000000").value() == 0ull);
    assert(!ParseBluetoothAddressString("").has_value());
    assert(!ParseBluetoothAddressString("AA:BB:CC:DD:EE").has_value());
    assert(!ParseBluetoothAddressString("AA:BB:CC:DD:EE:FF:00").has_value());
    assert(!ParseBluetoothAddressString("AA:BB:CC:DD:EE:GG").has_value());
    assert(!ParseBluetoothAddressString("A:BB:CC:DD:EE:FF").has_value());
    assert(!ParseBluetoothAddressString("AA: BB:CC:DD:EE:FF").has_value());
    assert(!ParseBluetoothAddressString("AA::CC:DD:EE:FF").has_value());
    assert(!ParseBluetoothAddressString("c05d39c3645").has_value());
    assert(!ParseBluetoothAddressString("c05d39c3645g").has_value());
}

void TestPairingAdvertisementClassify() {
    // 名称命中 VS- 前缀 → StickS3 / kName
    auto match = ClassifyPairingAdvertisement("VS-C3D8", true, false, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_id == "C3D8");
    assert(match->device_class == DeviceClass::kStickS3);
    assert(match->id_source == PairingCandidateIdSource::kName);
    assert(!match->is_temporary);

    // 名称命中 RC- 前缀 → 小米遥控器 / kName
    match = ClassifyPairingAdvertisement("RC-3A7F", false, false, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_id == "3A7F");
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(match->id_source == PairingCandidateIdSource::kName);
    assert(!match->is_temporary);

    // 小米名称白名单（中文名，无内嵌 ID）→ 地址低 16 位 / 非临时
    match = ClassifyPairingAdvertisement("小米蓝牙语音遥控器", false, false, 0xAABBCCDD3A7F);
    assert(match.has_value());
    assert(match->device_id == "3A7F");
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(match->id_source == PairingCandidateIdSource::kAddressFallback);
    assert(!match->is_temporary);

    // 小米名称白名单（英文名）
    match = ClassifyPairingAdvertisement("Xiaomi Bluetooth Remote 2 Pro", false, false,
                                         0xAABBCCDD3A7F);
    assert(match.has_value());
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(match->device_id == "3A7F");

    // 白名单名 "RC001"（非 RC-XXXX 前缀模式）→ 不误中名内 ID，走地址兜底
    match = ClassifyPairingAdvertisement("RC001", false, false, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(match->device_id == "EEFF");
    assert(match->id_source == PairingCandidateIdSource::kAddressFallback);

    // 仅 ATVV service UUID（无名称）→ 小米地址兜底候选
    match = ClassifyPairingAdvertisement("", false, true, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_id == "EEFF");
    assert(match->device_class == DeviceClass::kXiaomiRemote2Pro);
    assert(!match->is_temporary);

    // 仅 VoiceStick service UUID（无名称）→ StickS3 临时候选
    match = ClassifyPairingAdvertisement("", true, false, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_id == "EEFF");
    assert(match->device_class == DeviceClass::kStickS3);
    assert(match->id_source == PairingCandidateIdSource::kAddressFallback);
    assert(match->is_temporary);

    // VS service 与 ATVV 同时存在且无名称：VoiceStick service 优先（固件 SCAN_RSP 拆分场景）
    match = ClassifyPairingAdvertisement("", true, true, 0xAABBCCDDEEFF);
    assert(match.has_value());
    assert(match->device_class == DeviceClass::kStickS3);
    assert(match->is_temporary);

    // 无关广告 → 不识别
    assert(!ClassifyPairingAdvertisement("", false, false, 0xAABBCCDDEEFF).has_value());
    assert(!ClassifyPairingAdvertisement("Some Headphones", false, false, 0xAABBCCDDEEFF).has_value());
}

void TestAudioFrameParsing() {
    ByteVector frame = {1, 0x01, 16, 0};
    AppendLe32(frame, 123);
    AppendLe32(frame, 7);
    frame.push_back(0x03);
    frame.push_back(0);
    AppendLe16(frame, 3);
    frame.push_back(10);
    frame.push_back(11);
    frame.push_back(12);
    auto parsed = BleProtocol::ParseAudioFrame(frame);
    assert(parsed.has_value());
    assert(parsed->session_id == 123);
    assert(parsed->seq == 7);
    assert(parsed->IsStart());
    assert(parsed->IsEnd());
    assert(parsed->payload.size() == 3);
}

void TestBleControlPayloads() {
    auto battery_request = BleProtocol::BatteryStatusRequestPayload();
    assert(std::string(battery_request.begin(), battery_request.end()) == "{\"event\":\"battery_status_request\"}");

    // 敲击灵敏度 1~10 档，桌面端下发数值 level。
    auto tap_sens = BleProtocol::TapSensitivityPayload(5);
    assert(std::string(tap_sens.begin(), tap_sens.end()) == "{\"event\":\"tap_sensitivity\",\"level\":5}");
    auto tap_sens_high = BleProtocol::TapSensitivityPayload(10);
    assert(std::string(tap_sens_high.begin(), tap_sens_high.end()) == "{\"event\":\"tap_sensitivity\",\"level\":10}");

    // 体感鼠标开关下发。
    auto air_on = BleProtocol::AirMouseEnabledPayload(true);
    assert(std::string(air_on.begin(), air_on.end()) == "{\"event\":\"air_mouse_enabled\",\"enabled\":true}");
    auto air_off = BleProtocol::AirMouseEnabledPayload(false);
    assert(std::string(air_off.begin(), air_off.end()) == "{\"event\":\"air_mouse_enabled\",\"enabled\":false}");
}

void TestMotionFrameParsing() {
    // 合法 6 字节帧：version=1, type=0x11, dx=+100, dy=-50（小端）。
    ByteVector frame = {1, 0x11};
    AppendLe16(frame, static_cast<std::uint16_t>(static_cast<std::int16_t>(100)));
    AppendLe16(frame, static_cast<std::uint16_t>(static_cast<std::int16_t>(-50)));
    auto motion = BleProtocol::ParseMotionFrame(frame);
    assert(motion.has_value());
    assert(motion->dx == 100);
    assert(motion->dy == -50);

    // version 错。
    ByteVector bad_version = frame;
    bad_version[0] = 2;
    assert(!BleProtocol::ParseMotionFrame(bad_version).has_value());

    // type 错（0x10 是 JSON 状态帧，不是 motion）。
    ByteVector bad_type = frame;
    bad_type[1] = 0x10;
    assert(!BleProtocol::ParseMotionFrame(bad_type).has_value());

    // 长度不足。
    ByteVector too_short = {1, 0x11, 0x00};
    assert(!BleProtocol::ParseMotionFrame(too_short).has_value());

    // JSON 状态帧不应被误解析为 motion。
    ByteVector state_frame = {1, 0x10, 0x02, 0x00, '{', '}'};
    assert(!BleProtocol::ParseMotionFrame(state_frame).has_value());
}

void TestParseControlCliArgs() {
    using namespace voicestick;
    // 原始 JSON 原样透传。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--control", L"{\"event\":\"x\"}"};
        auto r = ParseControlCliArgs(3, argv);
        assert(r.has_value());
        assert(r->json == "{\"event\":\"x\"}");
    }
    // 免引号简写：字符串/布尔/整数取值。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--control", L"gateway_menu:action=open"};
        auto r = ParseControlCliArgs(3, argv);
        assert(r.has_value());
        assert(r->json == "{\"event\":\"gateway_menu\",\"action\":\"open\"}");
    }
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--control",
                                 L"gateway_select_target:self=true,index=2"};
        auto r = ParseControlCliArgs(3, argv);
        assert(r.has_value());
        assert(r->json ==
               "{\"event\":\"gateway_select_target\",\"self\":true,\"index\":2}");
    }
    // 无该选项 / 缺取值 / 简写非法（无冒号或无 key）。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe"};
        assert(!ParseControlCliArgs(1, argv).has_value());
    }
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--control"};
        assert(!ParseControlCliArgs(2, argv).has_value());
    }
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--control", L"no_colon_here"};
        assert(!ParseControlCliArgs(3, argv).has_value());
    }
}

void TestParseGatewayTargetCliArgs() {
    using namespace voicestick;
    // 无该选项。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/fw.bin"};
        assert(!ParseGatewayTargetCliArgs(3, argv).has_value());
    }
    // --gateway-target self / clear。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--gateway-target", L"self"};
        auto r = ParseGatewayTargetCliArgs(3, argv);
        assert(r.has_value());
        assert(r->self);
    }
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--gateway-target", L"clear"};
        auto r = ParseGatewayTargetCliArgs(3, argv);
        assert(r.has_value());
        assert(!r->self);
    }
    // 缺取值 / 取值非法 → nullopt。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--gateway-target"};
        assert(!ParseGatewayTargetCliArgs(2, argv).has_value());
    }
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--gateway-target", L"other"};
        assert(!ParseGatewayTargetCliArgs(3, argv).has_value());
    }
    // 与 --ota 混用互不干扰。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/fw.bin",
                                 L"--gateway-target", L"self"};
        auto r = ParseGatewayTargetCliArgs(5, argv);
        assert(r.has_value() && r->self);
    }
}

void TestEncoderRotateStateParsing() {    const std::string json = "{\"event\":\"encoder_rotate\",\"direction\":\"ccw\",\"steps\":3}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "encoder_rotate");
    assert(event->direction == "ccw");
    assert(event->steps.has_value());
    assert(event->steps.value() == 3);

    // 缺字段容错：direction 为空串、steps 为 nullopt，不影响整体解析。
    const std::string sparse = "{\"event\":\"encoder_rotate\"}";
    ByteVector sparse_frame = {1, 0x10};
    AppendLe16(sparse_frame, static_cast<std::uint16_t>(sparse.size()));
    sparse_frame.insert(sparse_frame.end(), sparse.begin(), sparse.end());
    auto sparse_event = BleProtocol::ParseStateEvent(sparse_frame);
    assert(sparse_event.has_value());
    assert(sparse_event->event == "encoder_rotate");
    assert(sparse_event->direction.empty());
    assert(!sparse_event->steps.has_value());
}

void TestEncoderStatusParsing() {
    // encoder_status 独立小帧：{"event":"encoder_status","present":true}。
    const std::string json = "{\"event\":\"encoder_status\",\"present\":true}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "encoder_status");
    assert(event->encoder_present.has_value());
    assert(event->encoder_present.value() == true);

    // present=false（未装编码器）。
    const std::string absent = "{\"event\":\"encoder_status\",\"present\":false}";
    ByteVector absent_frame = {1, 0x10};
    AppendLe16(absent_frame, static_cast<std::uint16_t>(absent.size()));
    absent_frame.insert(absent_frame.end(), absent.begin(), absent.end());
    auto absent_event = BleProtocol::ParseStateEvent(absent_frame);
    assert(absent_event.has_value());
    assert(absent_event->encoder_present.has_value());
    assert(absent_event->encoder_present.value() == false);

    // 缺 present 字段容错：解析为 nullopt，不影响整体解析。
    const std::string sparse = "{\"event\":\"encoder_status\"}";
    ByteVector sparse_frame = {1, 0x10};
    AppendLe16(sparse_frame, static_cast<std::uint16_t>(sparse.size()));
    sparse_frame.insert(sparse_frame.end(), sparse.begin(), sparse.end());
    auto sparse_event = BleProtocol::ParseStateEvent(sparse_frame);
    assert(sparse_event.has_value());
    assert(!sparse_event->encoder_present.has_value());

    // 其它事件不携带该字段（老固件无 encoder_status 事件，消费端按「在线」处理）。
    const std::string legacy =
        "{\"event\":\"device_info\",\"hardware\":\"stick_s3\",\"firmware_version\":\"2.2.0\"}";
    ByteVector legacy_frame = {1, 0x10};
    AppendLe16(legacy_frame, static_cast<std::uint16_t>(legacy.size()));
    legacy_frame.insert(legacy_frame.end(), legacy.begin(), legacy.end());
    auto legacy_event = BleProtocol::ParseStateEvent(legacy_frame);
    assert(legacy_event.has_value());
    assert(!legacy_event->encoder_present.has_value());

    // DeviceInfo 默认 encoder_present=true（未收到 encoder_status 时保持设置可见）。
    DeviceInfo default_info;
    assert(default_info.encoder_present);
}

void TestStateEventSourceParsing() {
    // 编码器按键事件带 source 字段。
    const std::string json = "{\"event\":\"button_click\",\"button\":\"primary\",\"duration_ms\":131,\"source\":\"encoder\"}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "button_click");
    assert(event->source == "encoder");

    // 物理键事件不带 source：解析为空串（缺省=物理键）。
    const std::string plain = "{\"event\":\"button_down\",\"button\":\"primary\",\"session_id\":42}";
    ByteVector plain_frame = {1, 0x10};
    AppendLe16(plain_frame, static_cast<std::uint16_t>(plain.size()));
    plain_frame.insert(plain_frame.end(), plain.begin(), plain.end());
    auto plain_event = BleProtocol::ParseStateEvent(plain_frame);
    assert(plain_event.has_value());
    assert(plain_event->source.empty());
}

void TestOggMuxer() {
    OggOpusMuxer muxer(16000, 1);
    ByteVector opus = {1, 2, 3, 4};
    auto ogg = muxer.Append(opus, false);
    assert(ogg.size() > 64);
    assert(std::string(reinterpret_cast<const char*>(ogg.data()), 4) == "OggS");
    auto tail = muxer.Finish();
    assert(std::string(reinterpret_cast<const char*>(tail.data()), 4) == "OggS");
}

void TestAsrProtocol() {
    AppConfig config = AppConfig::Defaults();
    config.asr_hotwords = {"小智", "VoiceStick"};
    auto event_payload = [](const ByteVector& frame, const std::string& session_id) {
        const std::size_t payload_size_offset = 12 + session_id.size();
        const auto payload_size = ReadBe32(std::span(frame.data() + payload_size_offset, 4));
        const std::size_t payload_offset = payload_size_offset + 4;
        assert(payload_size == frame.size() - payload_offset);
        return std::string(reinterpret_cast<const char*>(frame.data() + payload_offset),
                           frame.size() - payload_offset);
    };

    const std::string payload_session_id = "payload-session";
    auto request = AsrProtocol::MakeStartSessionFrame(config, payload_session_id);
    assert(request.size() > 16 + payload_session_id.size());
    assert(request[0] == 0x11);
    assert((request[1] >> 4) == 0x01);
    assert(ReadBe32(std::span(request.data() + 4, 4)) == 100);
    const auto payload = event_payload(request, payload_session_id);
    assert(payload.find("\"corpus\"") != std::string::npos);
    assert(payload.find("\\\"hotwords\\\"") != std::string::npos);
    assert(payload.find("\\\"word\\\":\\\"VoiceStick\\\"") != std::string::npos);

    const std::string body =
        "{\"error\":\"invalid_token\",\"message\":\"VoiceStick Cloud API key is invalid.\","
        "\"upgrade_url\":\"https://example.test/upgrade\"}";
    ByteVector response = {0x11, 0xf0, 0x10, 0x00};
    AppendBe32(response, 44002);
    AppendBe32(response, static_cast<std::uint32_t>(body.size()));
    response.insert(response.end(), body.begin(), body.end());
    auto parsed = AsrProtocol::ParseResponse(response);
    assert(parsed.has_value());
    assert(parsed->is_error);
    assert(parsed->text == "ASR 44002: VoiceStick Cloud API key is invalid.");
    assert(parsed->upgrade_url && *parsed->upgrade_url == "https://example.test/upgrade");

    auto start_connection = AsrProtocol::MakeStartConnectionFrame(config);
    assert(start_connection.size() > 12);
    assert((start_connection[1] >> 4) == 0x01);
    assert((start_connection[1] & 0x0f) == 0x04);
    assert(ReadBe32(std::span(start_connection.data() + 4, 4)) == 1);

    const std::string session_id = "session-1";
    auto start_session = AsrProtocol::MakeStartSessionFrame(config, session_id);
    assert(ReadBe32(std::span(start_session.data() + 4, 4)) == 100);
    assert(ReadBe32(std::span(start_session.data() + 8, 4)) == session_id.size());
    assert(std::string(reinterpret_cast<const char*>(start_session.data() + 12),
                       session_id.size()) == session_id);

    ByteVector opus = {1, 2, 3};
    auto task = AsrProtocol::MakeTaskRequestFrame(opus, session_id);
    assert((task[1] >> 4) == 0x02);
    assert(ReadBe32(std::span(task.data() + 4, 4)) == 200);

    const std::string event_body = "{\"result\":{\"text\":\"hi\"}}";
    ByteVector event_response = {0x11, 0x94, 0x10, 0x00};
    AppendBe32(event_response, 451);
    AppendBe32(event_response, static_cast<std::uint32_t>(session_id.size()));
    event_response.insert(event_response.end(), session_id.begin(), session_id.end());
    AppendBe32(event_response, static_cast<std::uint32_t>(event_body.size()));
    event_response.insert(event_response.end(), event_body.begin(), event_body.end());
    auto parsed_event = AsrProtocol::ParseEventResponse(event_response);
    assert(parsed_event.has_value());
    assert(parsed_event->event == AsrEvent::kAsrResponse);
    assert(parsed_event->session_id == session_id);
    assert(AsrProtocol::ExtractTranscript(parsed_event->payload_text) == "hi");

    AsrSessionOptions options;
    options.hotwords = {"VoiceStick"};
    options.show_utterances = true;
    options.result_type = AsrResultType::kSingle;
    const std::string utterance_session_id = "utterance-session";
    auto utterance_request = AsrProtocol::MakeStartSessionFrame(config, utterance_session_id, options);
    const auto utterance_payload = event_payload(utterance_request, utterance_session_id);
    assert(utterance_payload.find("\"show_utterances\":true") != std::string::npos);
    assert(utterance_payload.find("\"result_type\":\"single\"") != std::string::npos);

    const std::string segment_json =
        "{\"result\":{\"text\":\"hello world\",\"utterances\":["
        "{\"text\":\"hello\",\"definite\":true,\"start_time\":0,\"end_time\":500},"
        "{\"text\":\"world\",\"definite\":false,\"start_time\":500,\"end_time\":900}]}}";
    auto segments = AsrProtocol::ExtractSegments(segment_json);
    assert(segments.size() == 2);
    assert(segments[0].text == "hello");
    assert(segments[0].definite);
    std::set<std::string> emitted;
    auto definite = AsrProtocol::ExtractNewDefiniteSegments(segment_json, &emitted);
    assert(definite.size() == 1);
    assert(AsrProtocol::ExtractNewDefiniteSegments(segment_json, &emitted).empty());
}

void TestAsrHotwordCorpusBudget() {
    // token 估算：CJK 每字 1，ASCII 每 3 字符 1。
    assert(AsrProtocol::EstimateHotwordTokens("") == 0);
    assert(AsrProtocol::EstimateHotwordTokens("小智") == 2);
    assert(AsrProtocol::EstimateHotwordTokens("AGENTS.md") == 3);
    assert(AsrProtocol::EstimateHotwordTokens("VoiceStick") == 4);
    assert(AsrProtocol::EstimateHotwordTokens("Expe 记忆") == 4);
    assert(AsrProtocol::EstimateHotwordTokens("a") == 1);

    // 单词超 kHotwordMaxWordTokens 的被丢弃，其余保持顺序。
    const std::string too_long(40, 'x');  // ceil(40/3)=14 tokens
    auto fitted = AsrProtocol::FitHotwordsToCorpusBudget({"小智", too_long, "VoiceStick"});
    assert((fitted == std::vector<std::string>{"小智", "VoiceStick"}));

    // 累计超预算的词被丢弃，预算内的保留且顺序不变。
    std::vector<std::string> many;
    for (int i = 0; i < 30; ++i) many.push_back("热词编号" + std::to_string(i));  // 每个 4+1~2 tokens
    auto trimmed = AsrProtocol::FitHotwordsToCorpusBudget(many);
    assert(trimmed.size() < many.size());
    int used = 0;
    for (const auto& word : trimmed) used += AsrProtocol::EstimateHotwordTokens(word);
    assert(used <= AsrProtocol::kHotwordCorpusTokenBudget);
    for (std::size_t i = 0; i < trimmed.size(); ++i) assert(trimmed[i] == many[i]);

    // payload 集成：超预算的词不进入 corpus，预算内的保留。
    AppConfig config = AppConfig::Defaults();
    config.asr_hotwords = {"AGENTS.md", too_long, "CLAUDE.md"};
    const std::string session_id = "budget-session";
    auto frame = AsrProtocol::MakeStartSessionFrame(config, session_id);
    const std::size_t payload_size_offset = 12 + session_id.size();
    const auto payload_size = ReadBe32(std::span(frame.data() + payload_size_offset, 4));
    const std::string payload(reinterpret_cast<const char*>(frame.data() + payload_size_offset + 4),
                              payload_size);
    assert(payload.find("\\\"word\\\":\\\"AGENTS.md\\\"") != std::string::npos);
    assert(payload.find("\\\"word\\\":\\\"CLAUDE.md\\\"") != std::string::npos);
    assert(payload.find(too_long) == std::string::npos);

    // 全部超预算时不产出 corpus 字段。
    config.asr_hotwords = {too_long};
    auto empty_frame = AsrProtocol::MakeStartSessionFrame(config, session_id);
    const auto empty_size = ReadBe32(std::span(empty_frame.data() + payload_size_offset, 4));
    const std::string empty_payload(
        reinterpret_cast<const char*>(empty_frame.data() + payload_size_offset + 4), empty_size);
    assert(empty_payload.find("\"corpus\"") == std::string::npos);

    // 自学习平台词表 ID 进入 corpus，与热词 context 共存。
    config.volcengine_boosting_table_id = "boost-123";
    config.volcengine_correct_table_id = "correct-456";
    config.asr_hotwords = {"AGENTS.md"};
    auto table_frame = AsrProtocol::MakeStartSessionFrame(config, session_id);
    const auto table_size = ReadBe32(std::span(table_frame.data() + payload_size_offset, 4));
    const std::string table_payload(
        reinterpret_cast<const char*>(table_frame.data() + payload_size_offset + 4), table_size);
    assert(table_payload.find("\"boosting_table_id\":\"boost-123\"") != std::string::npos);
    assert(table_payload.find("\"correct_table_id\":\"correct-456\"") != std::string::npos);
    assert(table_payload.find("\\\"word\\\":\\\"AGENTS.md\\\"") != std::string::npos);

    // 仅词表无热词时也有 corpus，且无 context 字段；ID 为空则不出现字段。
    config.asr_hotwords = {};
    config.volcengine_correct_table_id = "";
    auto table_only_frame = AsrProtocol::MakeStartSessionFrame(config, session_id);
    const auto table_only_size = ReadBe32(std::span(table_only_frame.data() + payload_size_offset, 4));
    const std::string table_only_payload(
        reinterpret_cast<const char*>(table_only_frame.data() + payload_size_offset + 4),
        table_only_size);
    assert(table_only_payload.find("\"boosting_table_id\":\"boost-123\"") != std::string::npos);
    assert(table_only_payload.find("\"correct_table_id\"") == std::string::npos);
    assert(table_only_payload.find("\"context\"") == std::string::npos);
}

void TestTencentHotwordCharFilter() {
    // 腾讯词表 API 拒绝含 '.' 等字符的词（InvalidWordWeight），同步前必须过滤。
    assert(TencentAsrVocabClient::IsValidHotwordChars("Opus"));
    assert(TencentAsrVocabClient::IsValidHotwordChars("覃海洋"));
    assert(TencentAsrVocabClient::IsValidHotwordChars("ESP32-S3"));
    assert(TencentAsrVocabClient::IsValidHotwordChars("VB-CABLE"));
    assert(TencentAsrVocabClient::IsValidHotwordChars("win_sparkle"));
    assert(!TencentAsrVocabClient::IsValidHotwordChars("CLAUDE.md"));
    assert(!TencentAsrVocabClient::IsValidHotwordChars("AGENTS.md"));
    assert(!TencentAsrVocabClient::IsValidHotwordChars("带空格 的词"));
    assert(!TencentAsrVocabClient::IsValidHotwordChars(""));
}

void TestSerialBase32RoundTrip() {
    using namespace voicestick;
    // 全 0 / 全 1 / 递增模式 / 随机 79 字节往返
    for (int seed = 0; seed < 8; ++seed) {
        std::vector<std::uint8_t> data(79);
        std::uint8_t v = static_cast<std::uint8_t>(seed * 37);
        for (auto& b : data) { b = v; v = static_cast<std::uint8_t>(v * 131 + 17); }
        const std::string enc = SerialBase32Encode(data);
        assert(enc.size() == 127);
        const auto dec = SerialBase32Decode(enc);
        assert(dec.has_value() && *dec == data);
    }
    // 输入规范化：小写 + 连字符 + 空格应等价
    std::vector<std::uint8_t> data(79, 0xAB);
    std::string enc = SerialBase32Encode(data);
    std::string messy;
    for (size_t i = 0; i < enc.size(); ++i) {
        messy += static_cast<char>(std::tolower(static_cast<unsigned char>(enc[i])));
        if (i % 5 == 4 && i + 1 < enc.size()) messy += '-';
        if (i % 17 == 8) messy += ' ';
    }
    const auto dec2 = SerialBase32Decode(messy);
    assert(dec2.has_value() && *dec2 == data);
    // 非法字符：Crockford 排除 I L O U
    assert(!SerialBase32Decode("IAAAA").has_value());
    assert(!SerialBase32Decode("LAAAA").has_value());
    assert(!SerialBase32Decode("UAAAA").has_value());
    assert(!SerialBase32Decode("OAAAA").has_value());
    assert(!SerialBase32Decode("!AAAA").has_value());
    // 尾部碎片：5n mod 8 ∈ {5,6,7} 时不足一字节，拒绝（127+2=129 字符）
    assert(!SerialBase32Decode(enc + "AA").has_value());
    assert(!SerialBase32Decode("A").has_value());
    // 128 字符 = 640 bit = 整 80 字节（无碎片，合法但长度由调用方校验）
    const auto dec5 = SerialBase32Decode(enc + "A");
    assert(dec5.has_value() && dec5->size() == 80 && dec5->front() == data.front());
    // 空输入 = 空字节串（合法）
    const auto dec3 = SerialBase32Decode("");
    assert(dec3.has_value() && dec3->empty());
    // 0 在值 0 位置合法（Crockford '0' 是字母表成员）
    const auto dec4 = SerialBase32Decode("00000");
    assert(dec4.has_value() && dec4->size() == 3 && dec4->front() == 0x00);
}

// 开发密钥对（scripts/license_private_key.hex，gitignored）签发的测试串码，
// 与 scripts/license_test_vectors.json 同源；公钥在 src/license_public_key.h。
// 发行密钥对替换时重新生成（见 Doc/Plan/offline-license-activation.md Task 4）。

static LONG WINAPI UnhandledExceptionProbe(PEXCEPTION_POINTERS info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // 广谱：所有 NTSTATUS 错误段（0xC000xxxx）+ C++ 异常（0xE06D7363，未捕获即
    // terminate→静默 abort）+ 断点。C++ 异常首机会也会为「已捕获」的异常触发，
    // 若为噪声以最后一条为准（死亡前最后一条即真凶）。
    if ((code & 0xC0000000u) == 0xC0000000u || code == 0xE06D7363u ||
        code == 0x80000003u) {
        std::printf("[probe] exception 0x%08lX addr=%p\n",
                    static_cast<unsigned long>(code),
                    info->ExceptionRecord->ExceptionAddress);
        if (code == 0xC0000005u && info->ExceptionRecord->ExceptionInformation[0] != 0) {
            std::printf("[probe] AV access addr=%p\n",
                        reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
        }
        fflush(stdout);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

int main() {
    SetUnhandledExceptionFilter(&UnhandledExceptionProbe);
#ifdef _DEBUG
    // CI/命令行友好：Debug 下 assert 失败写 stderr 后直接终止，
    // 避免 CRT 默认弹「Microsoft Visual C++ Runtime Library」对话框挂起测试进程。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // Watson 报告与 abort() 弹窗都会在无人值守时挂死测试进程
#endif
    // stdout 重定向到文件时默认全缓冲，断言 abort 会丢掉之前的进度输出。
    setvbuf(stdout, nullptr, _IONBF, 0);
    // 全程异常围栏：未捕获 C++ 异常（0xE06D7363 → terminate → 静默 abort）打出 what()，
    // 否则只剩 SEH 探针的错误码，定位不到抛出点。
    try {
    TestDeviceIds();
    TestPlanReconnectAfterConnectFailure();
    TestPlanZombieRecovery();
    TestPlanZombieHeal();
    TestPlanAfterOsBondAttempt();
    TestPairDeviceHelpers();
    TestPairingAdvertisementClassify();
    RunLicenseImaAtvvBatchTests();  // N8 cut14: suite in core_tests_license_ima.cc
    RunCoordinatorBatch6Tests();  // N8 cut9: suite in core_tests_coordinator6.cc
    printf(">> TestCoordinatorDeviceSessionRoutesToLocalAsrWhenEnabled\n"); fflush(stdout);
    printf(">> TestCoordinatorLocalMicShortPressDiscards\n"); fflush(stdout);
    printf(">> TestCoordinatorLocalMicDisabledDoesNothing\n"); fflush(stdout);
    printf(">> TestCoordinatorLocalMicCaptureStartFailureCancelsSession\n"); fflush(stdout);
    printf(">> TestWasapiMicCaptureSmoke\n"); fflush(stdout);
    printf(">> wasapi smoke done\n"); fflush(stdout);
    printf(">> TestClipboardVaultMultiFormatRoundTrip\n"); fflush(stdout);
    printf(">> TestClipboardVaultSkipsHandleFormats\n"); fflush(stdout);
    printf(">> TestClipboardVaultEmptyClipboardSnapshot\n"); fflush(stdout);
    printf(">> TestClipboardVaultSaveThrowsWhenBusy\n"); fflush(stdout);
    printf(">> vault tests all done\n"); fflush(stdout);
    TestAudioFrameParsing();
    TestBleControlPayloads();
    RunProtocolContractTests();  // N8: suite in core_tests_protocol.cc
    TestEncoderRotateStateParsing();
    TestStateEventSourceParsing();
    TestEncoderStatusParsing();
    // N8 cut14: orphan registration (pre-check: def was never called)
    TestMotionFrameParsing();
    RunCodecFixtureSerialBatchTests();  // N8 cut13: suite in core_tests_codec_serial.cc
    TestOggMuxer();
    TestAsrProtocol();
    TestAsrHotwordCorpusBudget();
    printf(">> cluster: B14 hotword validation unified\n"); fflush(stdout);
    TestTencentHotwordCharFilter();
    TestSerialBase32RoundTrip();
    printf(">> cluster: C8 log rotation + url redaction + prompt cap\n"); fflush(stdout);
    printf(">> cluster: C7 cloud url TLS-only policy\n"); fflush(stdout);
    printf(">> cluster: C2 license binding devices union\n"); fflush(stdout);
    printf(">> cluster: C3 DateToDays pre-epoch clamp\n"); fflush(stdout);
    printf(">> cluster: B13 hotword candidates single writer\n"); fflush(stdout);
    printf(">> cluster: C6 firmware version compare robustness\n"); fflush(stdout);
    printf(">> TestOtaMaxInFlightBytes\n"); fflush(stdout);
    printf(">> cluster: D6 OTA chunk size for PDU\n"); fflush(stdout);
    printf(">> TestParseOtaCliArgs\n"); fflush(stdout);
    TestParseGatewayTargetCliArgs();
    TestParseControlCliArgs();
    printf(">> cluster: coordinator hotkey/click/tap\n"); fflush(stdout);
    printf(">> cluster: B3 wechat start failure rolls back default capture\n"); fflush(stdout);
    RunCoordinatorBatchTests();  // N8 cut3: suite in core_tests_coordinator.cc
    RunDeviceInputMiscBatchTests();  // N8 cut12: suite in core_tests_device_misc.cc
    RunCoordinatorBatch2Tests();  // N8 cut4: suite in core_tests_coordinator2.cc
    RunTencentBatchTests();  // N8 cut11: suite in core_tests_tencent.cc
    printf(">> cluster: B7 tencent hotword vocab async sync\n"); fflush(stdout);
    printf(">> TestCoordinatorUpdateConfigDestroysOldAsrOffThread\n"); fflush(stdout);
    printf(">> TestCoordinatorConcurrentUpdateConfigStress\n"); fflush(stdout);
    RunXiaomiAtvvBatchTests();  // N8 cut7: suite in core_tests_xiaomi_atvv.cc
    RunXiaomiUsageTapBatchTests();  // N8 cut10: suite in core_tests_xiaomi_usage_tap.cc
    printf(">> cluster: B2 usage tap Stop bounded\n"); fflush(stdout);
    RunCoordinatorBatch4Tests();  // N8 cut6: suite in core_tests_coordinator4.cc
    RunCoordinatorBatch3Tests();  // N8 cut5: suite in core_tests_coordinator3.cc
    RunCoordinatorBatch5Tests();  // N8 cut8: suite in core_tests_coordinator5.cc
    printf(">> cluster: C5 model present sha256 verify\n"); fflush(stdout);
    printf(">> ALL TESTS DONE\n"); fflush(stdout);
    return 0;
    } catch (const std::exception& e) {
        printf("UNCAUGHT EXCEPTION: %s\n", e.what()); fflush(stdout);
        return 1;
    } catch (...) {
        printf("UNCAUGHT EXCEPTION: unknown type\n"); fflush(stdout);
        return 1;
    }
}
