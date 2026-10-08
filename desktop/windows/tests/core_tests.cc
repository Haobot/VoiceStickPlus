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
static const std::string kTestSerial1 =
    "048V0-GQ6JD-S7VXB-D040G-0000T-QR3RC-WZT5Q-TCNTZ-M5RZA-PH0Q8-VZPZN-Y4GQV-"
    "4H8EP-3H7MX-P8EKY-0MYKQ-Z1J6R-SBHS1-4BJHA-D7GTV-E7FXB-NSD3G-R0NJE-FYNGM-MJATM-0G";
static const std::string kTestSerial2 =
    "09XM4-63ZJ4-HJKKF-ZZW10-0000H-P1J9M-YA462-E9WSF-D3HE0-6KCZR-XXT6V-2XQR1-"
    "9254S-SVXB5-Y39VQ-GNR34-T6ZP8-918AT-RJM1J-8XBBT-XPVR9-1XQ88-75RGX-D9B6Z-M56XM-3R";
static const std::string kTestSerial3 =
    "070K0-W9PTN-ERNPB-C041G-00008-A5DQY-XFK9E-B1S5Z-025HR-C2B9N-K9BK9-QMR5G-"
    "E7650-GT39D-V9B0M-FH46M-Q05PZ-CBFPS-80QFK-J5F00-N3XW5-AVD6Q-AY1HD-AE2T7-1KY18-08";

    const auto anchor_payload = BleProtocol::PowerLogTimeAnchorPayload(1755800000u);
    const std::string anchor_json(anchor_payload.begin(), anchor_payload.end());
    assert(anchor_json.find("\"cmd\":\"time_anchor\"") != std::string::npos);
    assert(anchor_json.find("\"epoch\":1755800000") != std::string::npos);

    // 3b) 供电态（USB）自动关机：power_mgmt 事件解析 + 命令 payload。
    // ParseStateEvent 对 power_mgmt 帧返回 nullopt（由 ParsePowerMgmtEvent 消费）。
    const auto pm_on_frame = BuildStateJsonFrame(
        "{\"event\":\"power_mgmt\",\"usb_auto_off\":true}");
    assert(!BleProtocol::ParseStateEvent(pm_on_frame).has_value());
    const auto pm_on = BleProtocol::ParsePowerMgmtEvent(pm_on_frame);
    assert(pm_on.has_value() && *pm_on);
    const auto pm_off = BleProtocol::ParsePowerMgmtEvent(BuildStateJsonFrame(
        "{\"event\":\"power_mgmt\",\"usb_auto_off\":false}"));
    assert(pm_off.has_value() && !*pm_off);
    // 缺字段或非 power_mgmt 帧 → nullopt。
    assert(!BleProtocol::ParsePowerMgmtEvent(
        BuildStateJsonFrame("{\"event\":\"power_mgmt\"}")).has_value());
    assert(!BleProtocol::ParsePowerMgmtEvent(frame).has_value());  // power_log 分片帧
    // 命令 payload：set 带布尔 enabled，get 为查询命令。
    const auto set_on_payload = BleProtocol::UsbAutoOffPayload(true);
    const std::string set_on_json(set_on_payload.begin(), set_on_payload.end());
    assert(set_on_json.find("\"event\":\"usb_auto_off\"") != std::string::npos);
    assert(set_on_json.find("\"enabled\":true") != std::string::npos);
    const auto set_off_payload = BleProtocol::UsbAutoOffPayload(false);
    const std::string set_off_json(set_off_payload.begin(), set_off_payload.end());
    assert(set_off_json.find("\"enabled\":false") != std::string::npos);
    const auto get_payload = BleProtocol::UsbAutoOffGetPayload();
    const std::string get_json(get_payload.begin(), get_payload.end());
    assert(get_json.find("\"event\":\"usb_auto_off_get\"") != std::string::npos);

    // 4) 累积器：锚点 + 周期采样 + 非周期事件过滤 + epoch 对齐。
    PowerLogAccumulator accumulator;
    std::vector<std::uint8_t> blob1;
    blob1.insert(blob1.end(), anchor_entry.begin(), anchor_entry.end());
    // 非周期模式切换事件（无 PERIODIC 位）：不应产生采样。
    const auto mode_entry = BuildPowerLogEntry(1010, 0, 1, 0);
    blob1.insert(blob1.end(), mode_entry.begin(), mode_entry.end());
    // 两个周期采样（一个充电、一个放电）。
    const auto sample1 = BuildPowerLogEntry(1060, 4100, 0,
                                            kPowerLogFlagPeriodic | kPowerLogFlagUsbPowered |
                                                kPowerLogFlagCharging);
    blob1.insert(blob1.end(), sample1.begin(), sample1.end());
    const auto sample2 = BuildPowerLogEntry(1120, 4090, 0, kPowerLogFlagPeriodic);
    blob1.insert(blob1.end(), sample2.begin(), sample2.end());

    std::vector<PowerLogSample> new_samples;
    assert(accumulator.ConsumeIncrementalBlob(blob1.data(), blob1.size(), &new_samples));
    assert(new_samples.size() == 2);
    assert(accumulator.samples().size() == 2);
    // epoch 对齐：anchor(epoch=1755800000, uptime=1000) → uptime 1060 → +60s。
    assert(accumulator.samples()[0].epoch_s == 1755800060);
    assert(accumulator.samples()[1].epoch_s == 1755800120);
    assert(accumulator.samples()[0].vbat_mv == 4100);
    assert(accumulator.samples()[0].charging);
    assert(accumulator.samples()[0].usb_powered);
    assert(!accumulator.samples()[1].charging);
    assert(accumulator.last_uptime_s() == 1120);

    // 5) 增量第二段：仅新周期采样。
    std::vector<std::uint8_t> blob2;
    const auto sample3 = BuildPowerLogEntry(1180, 4080, 0, kPowerLogFlagPeriodic);
    blob2.insert(blob2.end(), sample3.begin(), sample3.end());
    new_samples.clear();
    assert(accumulator.ConsumeIncrementalBlob(blob2.data(), blob2.size(), &new_samples));
    assert(new_samples.size() == 1);
    assert(accumulator.samples().size() == 3);
    assert(accumulator.samples()[2].epoch_s == 1755800180);

    // 6) 重启检测：uptime 回退返回 false 且状态不变。
    std::vector<std::uint8_t> blob_restart;
    const auto stale = BuildPowerLogEntry(50, 4000, 0, kPowerLogFlagPeriodic);
    blob_restart.insert(blob_restart.end(), stale.begin(), stale.end());
    new_samples.clear();
    assert(!accumulator.ConsumeIncrementalBlob(blob_restart.data(), blob_restart.size(),
                                               &new_samples));
    assert(new_samples.empty());
    assert(accumulator.samples().size() == 3);

    // 7) 长度非 12 倍数视为流损坏。
    const std::vector<std::uint8_t> bad_blob(13, 0);
    assert(!accumulator.ConsumeIncrementalBlob(bad_blob.data(), bad_blob.size(), nullptr));

    // 8) CSV：表头 + 每采样一行 + 无效读数标记。
    const std::string csv = accumulator.FormatCsv();
    assert(csv.find("seq,timestamp_iso,epoch_s,uptime_s,vbat_mv,vbat_v") == 0);
    assert(csv.find("1755800060") != std::string::npos);
    assert(csv.find("4.100") != std::string::npos);
    assert(csv.find("S0_ACTIVE") != std::string::npos);

    // 9) 无锚点场景：epoch 未对齐（-1）但仍收集采样。
    PowerLogAccumulator bare;
    std::vector<std::uint8_t> blob_no_anchor;
    const auto orphan = BuildPowerLogEntry(60, 4050, 0, kPowerLogFlagPeriodic);
    blob_no_anchor.insert(blob_no_anchor.end(), orphan.begin(), orphan.end());
    assert(bare.ConsumeIncrementalBlob(blob_no_anchor.data(), blob_no_anchor.size(), nullptr));
    assert(bare.samples().size() == 1);
    assert(bare.samples()[0].epoch_s == -1);

    // 10) 锚点之后的更新锚点生效（取 uptime 最大者）。
    PowerLogAccumulator re_anchor;
    std::vector<std::uint8_t> blob_re_anchor;
    blob_re_anchor.insert(blob_re_anchor.end(), anchor_entry.begin(), anchor_entry.end());
    const auto anchor2 = BuildPowerLogEntry(2000, 0, 0xFF, kPowerLogFlagTimeAnchor, 1755810000u);
    blob_re_anchor.insert(blob_re_anchor.end(), anchor2.begin(), anchor2.end());
    const auto sample_after = BuildPowerLogEntry(2060, 4070, 0, kPowerLogFlagPeriodic);
    blob_re_anchor.insert(blob_re_anchor.end(), sample_after.begin(), sample_after.end());
    assert(re_anchor.ConsumeIncrementalBlob(blob_re_anchor.data(), blob_re_anchor.size(), nullptr));
    assert(re_anchor.samples().size() == 1);
    assert(re_anchor.samples()[0].epoch_s == 1755810060);

    printf("TestPowerLogMonitor passed\n");
}



void TestTextRefinerRules() {
    printf(">> TestTextRefinerRules\n"); fflush(stdout);
    struct Case { const char* in; const char* want; const char* note; };
    const Case cases[] = {
        // 句首语气词（嗯/呃 直接删；啊/哦/噢/哎/唉/诶 须后跟标点才删，防误伤实义开头）
        {"嗯，帮我把这个文件重命名一下。", "帮我把这个文件重命名一下。", "句首嗯+标点"},
        {"呃我们试试", "我们试试", "句首呃无标点"},
        {"啊，开会了。", "开会了。", "句首啊+标点"},
        {"哦，对了，会议改到下午三点了。", "对了，会议改到下午三点了。", "句首哦+标点"},
        {"哦对了开会", "哦对了开会", "哦后无标点不动（保守）"},
        {"嗯帮我打开", "帮我打开", "句首嗯无标点"},
        // CJK 叠字：连续同字 >=3 时，笑声字（哈/嘿/呵/嘻）保留 2 个，其余保留 1 个
        {"我我我想去吃火锅。", "我想去吃火锅。", "口吃叠字保留1"},
        {"哈哈哈", "哈哈", "3哈保留2"},
        {"哈哈哈哈", "哈哈", "4哈保留2"},
        {"哈哈", "哈哈", "2次不规整"},
        {"AAAA", "AAAA", "拉丁叠字不规整"},
        {"555", "555", "数字叠字不规整"},
        // 中文（CJK）字符之间的停顿空格清除；拉丁/数字周围空格保留
        {"搜一下 这个 项目", "搜一下这个项目", "CJK间空格清除"},
        {"搜一下 llama.cpp 这个", "搜一下 llama.cpp 这个", "拉丁周围空格保留"},
        // 重复标点规整（省略号 …… 为合法双码点，连续超 2 个收敛为 2 个）
        {"好。。", "好。", "重复句号"},
        {"真的？？？", "真的？", "重复问号"},
        {"嗯……我觉得还行", "我觉得还行", "省略号跟随句首嗯"},
        // 句首孤立标点清理
        {"，我今天想", "我今天想", "句首孤立标点"},
        // 边界
        {"", "", "空文本"},
        {"嗯。", "", "纯口水词句清空"},
        {"帮我在 GitHub 上搜一下 llama.cpp 这个项目。",
         "帮我在 GitHub 上搜一下 llama.cpp 这个项目。", "干净文本不动"},
    };
    for (const auto& c : cases) {
        const std::string got = RuleRefineText(c.in);
        if (got != c.want) {
            printf("   RuleRefineText 失败 [%s]\n     in  =%s\n     got =%s\n     want=%s\n",
                   c.note, c.in, got.c_str(), c.want);
            fflush(stdout);
            assert(false);
        }
    }
    printf("TestTextRefinerRules passed\n");
}

void TestRefineGuardSafety() {
    printf(">> TestRefineGuardSafety\n"); fflush(stdout);
    // 放行：纯口水词删减（m0/refine spike 1.7B 实际输出形态）
    assert(RefineResultSafe("嗯，帮我把这个文件重命名一下。",
                            "帮我把这个文件重命名一下。"));
    assert(RefineResultSafe("啊，那个，你等一下，我马上就来。",
                            "你等一下，我马上就来。"));
    assert(RefineResultSafe("呃 那个 这个项目 嗯 用的是 BLE 连接。",
                            "这个项目用的是 BLE 连接。"));
    assert(RefineResultSafe("I think um we should uh use the model.",
                            "I think we should use the model."));
    assert(RefineResultSafe("然后呢，我们接下来就是要做那个测试了。",
                            "我们接下来就是要做测试了。"));
    // 放行：叠字删减、标点/空白规整、ASCII 大小写纠正（sense voice -> SenseVoice）
    assert(RefineResultSafe("我我我想去吃火锅。", "我想去吃火锅。"));
    assert(RefineResultSafe("我们用的是3.5版本。", "我们用的是 3.5 版本。"));
    assert(RefineResultSafe("我们还是用那个 sense voice 吧。",
                            "我们还是用 SenseVoice 吧。"));
    assert(RefineResultSafe("嗯。", ""));
    // 拦截（spike 真实失误样本回归）：改写、删实词、删实义片段、换字、删专名
    assert(!RefineResultSafe("你吃饭了没有啊？", "你吃饭了吗。"));
    assert(!RefineResultSafe("好的好的，我知道了。", "好的"));
    assert(!RefineResultSafe("这个函数的名字叫 process_data，注意是下划线。",
                             "这个函数的名字叫 process_data。"));
    assert(!RefineResultSafe("就是，我想问一下就是，这个支持 Windows 吗？",
                             "就是，这个支持 Windows 吗？"));
    assert(!RefineResultSafe("帮我把这个文件重命名一下。",
                             "帮我把那个文件重命名一下。"));
    assert(!RefineResultSafe("帮我在 GitHub 上搜一下 llama.cpp 这个项目。",
                             "帮我在 GitHub 上搜一下这个项目。"));
    assert(!RefineResultSafe("嗯，帮我打开浏览器。", ""));
    // 热词守卫联动：原文已正确出现的热词被改丢（含大小写改坏）必须拦截
    assert(!RefineResultSafe("编辑 AGENTS.md 这个文件", "编辑这个文件", {"AGENTS.md"}));
    assert(!RefineResultSafe("编辑 AGENTS.md 这个文件", "编辑 agents.md 这个文件",
                             {"AGENTS.md"}));
    assert(RefineResultSafe("编辑 AGENTS.md 这个文件", "编辑 AGENTS.md 这个文件",
                            {"AGENTS.md"}));
    printf("TestRefineGuardSafety passed\n");
}

void TestLocalRefinementClientOrchestration() {
    printf(">> TestLocalRefinementClientOrchestration\n"); fflush(stdout);
    // 可编程假引擎：注入式驱动编排层（流式/失败/取消/守卫回退）
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;      // 生成的 assistant 文本
        bool fail = false;      // Chat 直接失败
        int chat_calls = 0;
        std::string last_user;
        std::string last_system;
        bool Chat(const std::string& system_prompt, const std::string& user_text,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            ++chat_calls;
            last_system = system_prompt;
            last_user = user_text;
            if (fail) return false;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };

    struct Out {
        bool ok = false;
        std::string text;
        std::vector<std::string> tokens;
        // 引擎观察值：on_complete 时 Chat 已返回，在工作线程内采集
        //（run 返回后 client 连带析构 engine，事后读裸指针是悬垂）。
        int chat_calls = -1;
        std::string last_user;
        std::string last_system;
    };
    // 运行一次精修并同步等待完成（client 栈上持有，析构 join 保证线程收尾）
    auto run = [](std::unique_ptr<FakeEngine> fake, const std::string& text,
                  std::shared_ptr<std::atomic_bool> cancel = nullptr) {
        std::promise<Out> pr;
        auto fut = pr.get_future();
        auto tokens = std::make_shared<std::vector<std::string>>();
        FakeEngine* observer = fake.get();
        LocalRefinementClient client(std::move(fake));
        client.Refine(
            text,
            [tokens](std::string t) { tokens->push_back(std::move(t)); },
            [&pr, tokens, observer](bool ok, std::string s) {
                Out out;
                out.ok = ok;
                out.text = std::move(s);
                out.tokens = *tokens;
                if (observer) {  // 场景 4 传空引擎：无观察值可采
                    out.chat_calls = observer->chat_calls;
                    out.last_user = observer->last_user;
                    out.last_system = observer->last_system;
                }
                pr.set_value(std::move(out));
            },
            std::move(cancel));
        return fut.get();
    };

    {   // 1) 正常精修：L1 规则先行（fake 收到的是规则级文本），守卫放行
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我把这个文件重命名一下。";
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。");
        assert(r.ok);
        assert(r.text == "帮我把这个文件重命名一下。");
        assert(r.chat_calls == 1);
        assert(r.last_user.find("输入：帮我把这个文件重命名一下") !=
               std::string::npos);
        assert(r.last_system.find("输入：") != std::string::npos);  // few-shot
        assert(!r.tokens.empty());
    }
    {   // 2) 模型改坏（加字）被守卫拦截：回退规则级结果
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我我在 GitHub 上搜一下。";
        auto r = run(std::move(fake), "帮我在 GitHub 上搜一下 llama.cpp 这个项目。");
        assert(r.ok);
        assert(r.text == "帮我在 GitHub 上搜一下 llama.cpp 这个项目。");
    }
    {   // 3) 引擎失败：回退规则级结果（含 L1 规整）
        auto fake = std::make_unique<FakeEngine>();
        fake->fail = true;
        auto r = run(std::move(fake), "嗯，帮我打开浏览器。");
        assert(r.ok);
        assert(r.text == "帮我打开浏览器。");
    }
    {   // 4) 空引擎：纯规则层降级
        auto r = run(nullptr, "嗯，好的。");
        assert(r.ok);
        assert(r.text == "好的。");
    }
    {   // 5) 模板残留剥离（输出：前缀）后守卫放行
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "输出：帮我把这个文件重命名一下。";
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。");
        assert(r.ok);
        assert(r.text == "帮我把这个文件重命名一下。");
    }
    {   // 6) 预置取消：不触发引擎，直接回退
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我把这个文件重命名一下。";
        auto cancel = std::make_shared<std::atomic_bool>(true);
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。", cancel);
        assert(!r.ok);
        assert(r.text == "帮我把这个文件重命名一下。");
        assert(r.chat_calls == 0);
    }
    {   // 7) 热词联动：LLM 结果丢热词回退规则级
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "编辑这个文件";   // 丢了 AGENTS.md
        // hotwords 用例：Refine 带 hotwords —— run 不支持，单独构造
        std::promise<std::pair<bool, std::string>> pr;
        auto fut = pr.get_future();
        LocalRefinementClient client(std::move(fake));
        client.Refine("编辑 AGENTS.md 这个文件",
                      [](std::string) {},
                      [&pr](bool ok, std::string s) {
                          pr.set_value({ok, std::move(s)});
                      },
                      nullptr, {"AGENTS.md"});
        auto [ok, text] = fut.get();
        assert(ok);
        assert(text == "编辑 AGENTS.md 这个文件");
    }
    // 8) StripReplyTemplate 纯函数
    assert(LocalRefinementClient::StripReplyTemplate(
               "输入：abc\n输出：\n帮我把文件重命名") == "帮我把文件重命名");
    assert(LocalRefinementClient::StripReplyTemplate(
               "<think>x</think>好的") == "好的");
    assert(LocalRefinementClient::StripReplyTemplate("  干净文本  ") == "干净文本");
    printf("TestLocalRefinementClientOrchestration passed\n");
}

// 自定义精修提示词（设置页可编辑的底层语义）：非空构造注入生效，空串/缺省
// 回退内置 few-shot 默认——空 = 默认，与云端 refine_prompt 语义一致。
void TestLocalRefinementCustomPrompt() {
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string last_system;
        bool Chat(const std::string& system_prompt, const std::string&,
                  const std::function<bool(const std::string&)>&,
                  std::string& completion) override {
            last_system = system_prompt;
            completion = "帮我把这个文件重命名一下。";
            return true;
        }
        bool IsReady() const override { return true; }
    };

    // 观察值在 on_complete 内采集（Chat 已返回、engine 仍存活）
    auto system_of = [](std::unique_ptr<FakeEngine> fake,
                        const std::string& prompt = {}) {
        std::promise<std::string> pr;
        auto fut = pr.get_future();
        FakeEngine* observer = fake.get();
        LocalRefinementClient client(std::move(fake), prompt);
        client.Refine("嗯，帮我把这个文件重命名一下。", nullptr,
                      [&pr, observer](bool, std::string) {
                          pr.set_value(observer->last_system);
                      });
        return fut.get();
    };

    const std::string custom = "自定义提示词：删掉所有口水词。\n输入：嗯 x\n输出：x";
    assert(system_of(std::make_unique<FakeEngine>(), custom) == custom);
    assert(system_of(std::make_unique<FakeEngine>()) ==
           LocalRefinementClient::BuildSystemPrompt());
    assert(system_of(std::make_unique<FakeEngine>(), "") ==
           LocalRefinementClient::BuildSystemPrompt());
    printf("TestLocalRefinementCustomPrompt passed\n");
}

// 诊断日志回调：归因精修结果来自哪一层（llm ok / guard blocked / llm fail /
// llm empty），协调器注入 LogCoordinatorLine 落 VoiceStickApp.log 供实测排查。
void TestLocalRefinementDiagnosticsLogs() {
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;
        bool fail = false;
        bool Chat(const std::string&, const std::string&,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            if (fail) return false;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    struct Out {
        std::string text;
        std::vector<std::string> logs;
    };
    // 构造注入 log 采集，跑一次精修同步取回（输入文本进 RunRefine 时已归一
    // 为规则级文本，日志 in= 即 L1 输出）。
    auto run = [](std::unique_ptr<FakeEngine> fake, const std::string& input) {
        std::promise<Out> pr;
        auto fut = pr.get_future();
        auto logs = std::make_shared<std::vector<std::string>>();
        LocalRefinementClient client(
            std::move(fake), {},
            [logs](std::string_view m) { logs->emplace_back(m); });
        client.Refine(input, nullptr,
                      [&pr, logs](bool, std::string s) {
                          Out out;
                          out.text = std::move(s);
                          out.logs = *logs;
                          pr.set_value(std::move(out));
                      });
        return fut.get();
    };
    auto has = [](const std::vector<std::string>& logs, const char* needle) {
        for (const auto& l : logs) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    {   // LLM 成功放行：in= + llm ok 两行
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我把这个文件重命名一下。";
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。");
        assert(r.text == "帮我把这个文件重命名一下。");
        assert(has(r.logs, "in='帮我把这个文件重命名一下。'"));
        assert(has(r.logs, "llm ok: '帮我把这个文件重命名一下。'"));
    }
    {   // 守卫拦截：guard blocked 行 + 回退规则级
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我我在 GitHub 上搜一下。";  // 加字必被守卫拦
        auto r = run(std::move(fake), "帮我在 GitHub 上搜一下 llama.cpp 这个项目。");
        assert(r.text == "帮我在 GitHub 上搜一下 llama.cpp 这个项目。");
        assert(has(r.logs, "guard blocked"));
    }
    {   // 引擎失败：llm fail 行 + 规则级兜底
        auto fake = std::make_unique<FakeEngine>();
        fake->fail = true;
        auto r = run(std::move(fake), "嗯，帮我打开浏览器。");
        assert(r.text == "帮我打开浏览器。");
        assert(has(r.logs, "llm fail -> rule"));
    }
    printf("TestLocalRefinementDiagnosticsLogs passed\n");
}

// 跨轮纠正指令管线（M2a）：context.turns 非空走 BuildCorrectionSystemPrompt +
// 「上文：/输入：/处理：」prompt + ApplyPinyinCorrections 受限执行；为空走
// 现行 few-shot 管线（回归保护）。场景锚定 M0 spike C 组案例。
void TestLocalRefinementCrossTurnOrchestration() {
    printf(">> TestLocalRefinementCrossTurnOrchestration\n"); fflush(stdout);
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;
        bool fail = false;
        int chat_calls = 0;
        std::string last_user;
        std::string last_system;
        bool Chat(const std::string& system_prompt, const std::string& user_text,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            ++chat_calls;
            last_system = system_prompt;
            last_user = user_text;
            if (fail) return false;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    struct Out {
        bool ok = false;
        std::string text;
        int chat_calls = -1;
        std::string last_user;
        std::string last_system;
    };
    using Ctx = LocalRefinementClient::RefineContext;
    auto run = [](std::unique_ptr<FakeEngine> fake, const std::string& text,
                  Ctx context, std::vector<std::string> hotwords = {}) {
        std::promise<Out> pr;
        auto fut = pr.get_future();
        FakeEngine* observer = fake.get();
        LocalRefinementClient client(std::move(fake));
        client.Refine(
            text, [](std::string) {},
            [&pr, observer](bool ok, std::string s) {
                Out out;
                out.ok = ok;
                out.text = std::move(s);
                if (observer) {
                    out.chat_calls = observer->chat_calls;
                    out.last_user = observer->last_user;
                    out.last_system = observer->last_system;
                }
                pr.set_value(std::move(out));
            },
            nullptr, std::move(hotwords), std::move(context));
        return fut.get();
    };

    {   // 1) 纠正指令执行（C01 真机案例）：prompt 形态 + 受限替换放行
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "鱼器渍→语气词";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "我们刚才测了语气词过滤。"});
        auto r = run(std::move(fake), "那些鱼器渍已经被过滤掉了。", std::move(ctx));
        assert(r.ok);
        assert(r.text == "那些语气词已经被过滤掉了。");
        assert(r.chat_calls == 1);
        // system 是纠正指令模式（区别于 few-shot 生成模式）
        assert(r.last_system.find("参考上文") != std::string::npos);
        assert(r.last_system.find("错词→纠正词") != std::string::npos);
        // user 形态：历史续写块（FakeEngine 默认实现拼「输入：…处理：…」，
        // instruction 空轮次归一化为「无」——与真引擎 KV 重放形态自洽）
        // + 当句输入 + 处理锚（spike build_prompt 同款）
        assert(r.last_user.find("输入：\n处理：无\n") != std::string::npos);
        assert(r.last_user.find("输入：那些鱼器渍已经被过滤掉了。\n处理：") !=
               std::string::npos);
        // user 以「处理：」结尾（生成锚，spike 同款）
        assert(r.last_user.size() >= 9 &&
               r.last_user.compare(r.last_user.size() - 9, 9, "处理：") == 0);
    }
    {   // 1b) V2 热词注入：cross 模式热词非空时 user 含「热词：」行（输入
        //     行后、处理锚前），系统提示词教学示例提及热词；空表无热词行。
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "我们测了过滤。"});
        auto r = run(std::move(fake), "这些水池还没删。",
                     std::move(ctx), {"口水词", "语气词"});
        assert(r.ok);
        assert(r.last_user.find("输入：这些水池还没删。\n热词：口水词，语气词\n处理：") !=
               std::string::npos);
        assert(r.last_system.find("热词") != std::string::npos);
        // 空热词表：不残留热词行
        auto fake2 = std::make_unique<FakeEngine>();
        fake2->reply = "无";
        Ctx ctx2;
        ctx2.cross_turn = true;
        ctx2.turns.push_back({"", "我们测了过滤。"});
        auto r2 = run(std::move(fake2), "这些水池还没删。", std::move(ctx2));
        assert(r2.ok);
        assert(r2.last_user.find("热词：") == std::string::npos);
    }
    {   // 2) 模型输出「无」：结果=规则级文本（无提升无伤害）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "帮我把垃圾倒一下。"});
        auto r = run(std::move(fake), "嗯，帮我把垃圾倒一下。", std::move(ctx));
        assert(r.ok);
        assert(r.text == "帮我把垃圾倒一下。");
    }
    {   // 3) 越界指令部分拒绝：合法删除执行，越界替换拒绝（C05 形态）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "嗯，\n那个→明天\n办→半";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "明天下午三点的会议记得提醒我。"});
        auto r = run(std::move(fake), "嗯，那个会议改成三点办了。", std::move(ctx));
        assert(r.ok);
        assert(r.text == "那个会议改成三点办了。");  // 「嗯，」删除生效
    }
    {   // 4) 纠正词不在上文（C04）：指令拒绝，文本回退（规则级）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "口头鱼→口头语";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "口水词和语气词都要删掉。"});
        auto r = run(std::move(fake), "口头鱼也算语气词吗？", std::move(ctx));
        assert(r.ok);
        assert(r.text == "口头鱼也算语气词吗？");
    }
    {   // 5) 引擎失败：跨轮模式同样回退规则级
        auto fake = std::make_unique<FakeEngine>();
        fake->fail = true;
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "上文。"});
        auto r = run(std::move(fake), "嗯，帮我打开浏览器。", std::move(ctx));
        assert(r.ok);
        assert(r.text == "帮我打开浏览器。");
    }
    {   // 6) 热词保护：指令删除热词回退规则级
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "超导";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "上文。"});
        auto r = run(std::move(fake), "超导材料不错。", std::move(ctx),
                     {"超导"});
        assert(r.ok);
        assert(r.text == "超导材料不错。");
    }
    {   // 7) context 为空：走现行 few-shot 管线（system 含生成式教学）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我把这个文件重命名一下。";
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。", Ctx{});
        assert(r.ok);
        assert(r.text == "帮我把这个文件重命名一下。");
        assert(r.last_system.find("参考上文") == std::string::npos);
        assert(r.last_user.find("输出：") != std::string::npos);
        assert(r.last_user.find("处理：") == std::string::npos);
    }
    {   // 8) 多轮上文：逐轮续写块 + 守卫域含全部轮 refined
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "语音设别→语音识别";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "这个语音识别项目叫 VoiceStick。"});
        ctx.turns.push_back({"", "语音识别的准确率还可以。"});
        auto r = run(std::move(fake), "这个语音设别模型是哪个？", std::move(ctx));
        assert(r.ok);
        assert(r.text == "这个语音识别模型是哪个？");
        assert(r.last_user.find("输入：\n处理：无\n输入：\n处理：无\n") !=
               std::string::npos);
    }
    {   // 9) 3 参完成回调：跨轮成功时第三参=当轮模型指令输出（协调器存
        //     RefineHistory.instruction 的数据源——KV 重放 assistant 侧需
        //     形态自洽）；非跨轮管线恒给空串
        struct Triple {
            bool ok = false;
            std::string text;
            std::string instruction;
        };
        auto run3 = [](std::unique_ptr<FakeEngine> fake, const std::string& text,
                       Ctx context) {
            std::promise<Triple> pr;
            auto fut = pr.get_future();
            LocalRefinementClient client(std::move(fake));
            client.Refine(
                text, [](std::string) {},
                [&pr](bool ok, std::string s, std::string instr) {
                    Triple t;
                    t.ok = ok;
                    t.text = std::move(s);
                    t.instruction = std::move(instr);
                    pr.set_value(std::move(t));
                },
                nullptr, {}, std::move(context));
            return fut.get();
        };
        {
            auto fake = std::make_unique<FakeEngine>();
            fake->reply = "鱼器渍→语气词\n";
            Ctx ctx;
            ctx.cross_turn = true;
            ctx.turns.push_back({"", "我们刚才测了语气词过滤。", "无"});
            const auto r = run3(std::move(fake), "那些鱼器渍已经被过滤掉了。",
                                std::move(ctx));
            assert(r.ok);
            assert(r.text == "那些语气词已经被过滤掉了。");
            assert(r.instruction == "鱼器渍→语气词");  // stripped（尾部空白已剥）
        }
        {
            auto fake = std::make_unique<FakeEngine>();
            fake->reply = "帮我把这个文件重命名一下。";
            const auto r = run3(std::move(fake),
                                "嗯，帮我把这个文件重命名一下。", Ctx{});
            assert(r.ok);
            assert(r.text == "帮我把这个文件重命名一下。");
            assert(r.instruction.empty());  // 非跨轮管线无指令语义
        }
    }
    {   // 10) 引擎历史 assistant 侧 = instruction（形态自洽重放，防模型
        //     漂移为文本输出——重放 refined 实测 3 轮起漂移，smoke 2026-09-10）：
        //     带 instruction 的历史轮拼出「处理：{instruction}」，绝不出现
        //     refined 文本（守卫域只在 client 内部使用）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"那些鱼器渍被过滤了。", "那些语气词被过滤了。",
                             "鱼器渍→语气词"});
        auto r = run(std::move(fake), "帮我把垃圾倒一下。", std::move(ctx));
        assert(r.ok);
        assert(r.last_user.find("输入：那些鱼器渍被过滤了。\n处理：鱼器渍→语气词\n") !=
               std::string::npos);
        assert(r.last_user.find("那些语气词被过滤了。") == std::string::npos);
    }
    {   // 11) 热词锚点域：上文无正确写法（连续误识别）但热词表有 →
        //     client 须把 hotwords 下传 ApplyPinyinCorrections 作守卫锚点，
        //     指令放行（S2，划词纠错建立的词入热词表后即可自愈）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "逾期次→语气词";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "我们测了逾期次过滤。"});  // 上文同样误识别
        auto r = run(std::move(fake), "这些逾期次还没删干净。", std::move(ctx),
                     {"语气词"});
        assert(r.ok);
        assert(r.text == "这些语气词还没删干净。");
    }
    printf("TestLocalRefinementCrossTurnOrchestration passed\n");
}

// 跨轮管线诊断日志归因（协调器实测排查通道）：correct ok / correct none /
// correct partial（含拒绝明细）/ hotword blocked。
void TestLocalRefinementCrossTurnDiagnosticsLogs() {
    printf(">> TestLocalRefinementCrossTurnDiagnosticsLogs\n"); fflush(stdout);
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;
        bool Chat(const std::string&, const std::string&,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    using Ctx = LocalRefinementClient::RefineContext;
    auto run = [](const std::string& reply, const std::string& text, Ctx ctx,
                  std::vector<std::string> hotwords = {}) {
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = reply;
        auto logs = std::make_shared<std::vector<std::string>>();
        LocalRefinementClient client(
            std::move(fake), {},
            [logs](std::string_view line) { logs->emplace_back(line); });
        std::promise<std::string> pr;
        auto fut = pr.get_future();
        client.Refine(text, [](std::string) {},
                      [&pr](bool, std::string s) { pr.set_value(std::move(s)); },
                      nullptr, std::move(hotwords), std::move(ctx));
        fut.get();
        return std::move(*logs);
    };
    auto has = [](const std::vector<std::string>& logs, const std::string& needle) {
        for (const auto& l : logs)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    };
    printf("   logs scenario 1\n"); fflush(stdout);
    {   // 纠正成功归因
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "我们刚才测了语气词过滤。"});
        const auto logs = run("鱼器渍→语气词", "那些鱼器渍已经被过滤掉了。",
                              std::move(ctx));
        assert(has(logs, "in='那些鱼器渍已经被过滤掉了。'"));
        assert(has(logs, "correct ok: '那些语气词已经被过滤掉了。'"));
    }
    printf("   logs scenario 2\n"); fflush(stdout);
    {   // 无指令归因
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "帮我把垃圾倒一下。"});
        const auto logs = run("无", "帮我把垃圾倒一下。", std::move(ctx));
        assert(has(logs, "correct none"));
    }
    printf("   logs scenario 3\n"); fflush(stdout);
    {   // 部分拒绝归因（含拒绝指令明细）
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "明天下午三点的会议记得提醒我。"});
        const auto logs = run("嗯，\n那个→明天\n办→半",
                              "嗯，那个会议改成三点办了。", std::move(ctx));
        assert(has(logs, "correct partial"));
        assert(has(logs, "那个→明天"));
        assert(has(logs, "办→半"));
    }
    printf("   logs scenario 4\n"); fflush(stdout);
    {   // 热词拦截归因
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "上文。"});
        const auto logs = run("超导", "超导材料不错。", std::move(ctx), {"超导"});
        assert(has(logs, "hotword blocked"));
    }
    printf("   logs scenario 5\n"); fflush(stdout);
    {   // 引擎失败归因（跨轮同现行）
        struct FailEngine : LocalLlmEngine {
            bool Chat(const std::string&, const std::string&,
                      const std::function<bool(const std::string&)>&,
                      std::string&) override { return false; }
            bool IsReady() const override { return true; }
        };
        auto logs = std::make_shared<std::vector<std::string>>();
        LocalRefinementClient client(
            std::make_unique<FailEngine>(), {},
            [logs](std::string_view line) { logs->emplace_back(line); });
        std::promise<std::string> pr;
        auto fut = pr.get_future();
        Ctx ctx_fail;
        ctx_fail.cross_turn = true;
        ctx_fail.turns.push_back({"", "上文。"});
        client.Refine("嗯，帮我打开浏览器。", [](std::string) {},
                      [&pr](bool, std::string s) { pr.set_value(std::move(s)); },
                      nullptr, {}, std::move(ctx_fail));
        printf("   scenario 5: refine dispatched\n"); fflush(stdout);
        const std::string got5 = fut.get();
        printf("   scenario 5: got='%s'\n", got5.c_str()); fflush(stdout);
        assert(got5 == "帮我打开浏览器。");
        assert(has(*logs, "llm fail -> rule"));
        printf("   scenario 5: assertions done\n"); fflush(stdout);
    }
    printf("TestLocalRefinementCrossTurnDiagnosticsLogs passed\n");
}


// ---- 跨轮纠错 M1：拼音守卫 + 历史缓冲（Doc/Plan/local-asr-accuracy-and-cross-turn-refinement.md §3.5.1/2）----

// 判定矩阵锚定 M0 spike 对拍结果（m0/refine/run_cross_turn_spike.py same_or_near）：
// 韵母集合有交集即同音（声母不参与——渍zì/词cí、马mǎ/打dǎ 须放行）；
// 韵母命中模糊对（e/i 卷舌弱化）且声母交集非空 → 近音。
void TestPinyinSameOrNear() {
    printf(">> TestPinyinSameOrNear\n"); fflush(stdout);
    struct Case { std::uint32_t a, b; bool want; const char* note; };
    const Case cases[] = {
        {U'鱼', U'语', true,  "鱼/语 韵母v交集（真机案例）"},
        {U'器', U'气', true,  "器/气 同音qi"},
        {U'渍', U'词', true,  "渍/词 声母z/c不同但韵母i交集（M0 GO口径）"},
        {U'马', U'打', true,  "马/打 韵母a交集（宽松口径锚定）"},
        {U'设', U'识', true,  "设/识 e/i卷舌弱化+sh声母同"},
        {U'半', U'办', true,  "半/办 同音ban"},
        {U'女', U'旅', true,  "女/旅 韵母v交集"},
        {U'鱼', U'鱼', true,  "同字"},
        {U'A',  U'a',  true,  "ASCII忽略大小写"},
        {U'5',  U'5',  true,  "非汉字同字符"},
        {U'那', U'明', false, "那na/明ming 韵母无交集"},
        {U'塘', U'气', false, "塘tang/气qi 无交集"},
        {U'a',  U'鱼', false, "非汉字vs汉字"},
        {U'鱼', U'x',  false, "汉字vs非汉字"},
        {U'鱼', U'b',  false, "汉字vs字母不等"},
        {0x20000, 0x20001, false, "表外扩展B区字查不到按不同音"},
    };
    for (const auto& c : cases) {
        const bool got = PinyinSameOrNear(c.a, c.b);
        if (got != c.want) {
            printf("   PinyinSameOrNear 失败 [%s] U+%04X/U+%04X got=%d want=%d\n",
                   c.note, c.a, c.b, got, c.want);
            fflush(stdout);
            assert(false);
        }
    }
    printf("TestPinyinSameOrNear passed\n");
}

// golden = M0 spike C 组全 8 案例（report_cross_turn_4b.md）：
// 放行 4（C01/C02/C03/C08）、守卫拒绝回退/部分执行 3（C04/C05/C06）、直通 1（C07）。
void TestApplyPinyinCorrections() {
    printf(">> TestApplyPinyinCorrections\n"); fflush(stdout);
    {   // C01 真机案例：替换放行
        const auto r = ApplyPinyinCorrections(
            "那些鱼器渍已经被过滤掉了。", "鱼器渍→语气词",
            "我们刚才测了语气词过滤。");
        assert(r.text == "那些语气词已经被过滤掉了。");
        assert(r.rejected.empty());
    }
    {   // C02：替换放行
        const auto r = ApplyPinyinCorrections(
            "这个蓝崖遥控器的按键手感不错。", "蓝崖→蓝牙",
            "帮我用蓝牙遥控器测试一下。");
        assert(r.text == "这个蓝牙遥控器的按键手感不错。");
        assert(r.rejected.empty());
    }
    {   // C03：人名替换放行
        const auto r = ApplyPinyinCorrections(
            "章维说他会晚点到。", "章维→张伟",
            "张伟下午的会议来不了。");
        assert(r.text == "张伟说他会晚点到。");
        assert(r.rejected.empty());
    }
    {   // C08：e/i 卷舌弱化纠正放行
        const auto r = ApplyPinyinCorrections(
            "这个语音设别模型是哪个？", "语音设别→语音识别",
            "这个语音识别项目叫 VoiceStick。");
        assert(r.text == "这个语音识别模型是哪个？");
        assert(r.rejected.empty());
    }
    {   // C04：纠正词未在上文出现过 → 拒绝、原文直通
        const auto r = ApplyPinyinCorrections(
            "口头鱼也算语气词吗？", "口头鱼→口头语",
            "口水词和语气词都要删掉。");
        assert(r.text == "口头鱼也算语气词吗？");
        assert(r.rejected.size() == 1 && r.rejected[0] == "口头鱼→口头语");
    }
    {   // C05：混合指令——合法删除执行，两条越界替换拒绝（那/明韵母无交集、半不在上文）
        const auto r = ApplyPinyinCorrections(
            "嗯，那个会议改成三点办了。", "嗯，\n那个→明天\n办→半",
            "明天下午三点的会议记得提醒我。");
        assert(r.text == "那个会议改成三点办了。");  // 「嗯，」删除生效
        assert(r.rejected.size() == 2);
        assert(r.rejected[0] == "那个→明天");
        assert(r.rejected[1] == "办→半");
    }
    {   // C06：长度不等（2≠3）→ 拒绝
        const auto r = ApplyPinyinCorrections(
            "我在鱼塘里养了很多鱼。", "鱼塘→语气词",
            "那些语气词都被过滤掉了。");
        assert(r.text == "我在鱼塘里养了很多鱼。");
        assert(r.rejected.size() == 1);
    }
    {   // C07：无指令直通
        const auto r = ApplyPinyinCorrections("帮我把垃圾倒一下。", "无", "上文无关。");
        assert(r.text == "帮我把垃圾倒一下。");
        assert(r.rejected.empty());
    }
    {   // 边界：src 不在原文 → 拒绝
        const auto r = ApplyPinyinCorrections("今天天气不错。", "天气→气候", "气候很好。");
        assert(r.text == "今天天气不错。");
        assert(r.rejected.size() == 1);
    }
    {   // 边界：删除行含字母数字 → 拒绝（防误删内容词）
        const auto r = ApplyPinyinCorrections("嗯，打开 debug 开关。", "嗯，\ndebug",
                                              "无关。");
        assert(r.text == "打开 debug 开关。");
        assert(r.rejected.size() == 1 && r.rejected[0] == "debug");
    }
    {   // 边界：替换字对拼音不符（那/明）→ 拒绝
        const auto r = ApplyPinyinCorrections("我们那个走。", "那个→明个", "明个再说。");
        assert(r.text == "我们那个走。");
        assert(r.rejected.size() == 1);
    }
    {   // 边界：删除幅度 >60% 整体回退（total-ratio）
        const auto r = ApplyPinyinCorrections("嗯，那个，啊，就这样吧。",
                                              "嗯，\n那个，\n啊，\n就这样吧。", "无关。");
        assert(r.text == "嗯，那个，啊，就这样吧。");  // 回退原文
        bool has_ratio = false;
        for (const auto& x : r.rejected) has_ratio |= (x == "total-ratio");
        assert(has_ratio);
    }
    {   // 边界：空指令直通
        const auto r = ApplyPinyinCorrections("原文。", "", "无关。");
        assert(r.text == "原文。" && r.rejected.empty());
    }
    {   // S2 热词锚点域（划词纠错配套，Doc/Plan/selection-hotword-correction-and-asr-hotword-spike.md）：
        //     热词表整词命中可替代「上文出现过」作锚点——ASR 连续误识别
        //     「语气词」时上文永远无正确写法，用户划词确认的正确词入热词表
        //     后即建立锚点，所有近音变体由逐字近音校验自动容忍。
        const auto r = ApplyPinyinCorrections(
            "我们测试了逾期次过滤。", "逾期次→语气词", "",
            {"语气词"});
        assert(r.text == "我们测试了语气词过滤。");
        assert(r.rejected.empty());
    }
    {   // 近音校验不因热词锚点放宽：dst 与 src 非近音 → 仍拒
        const auto r = ApplyPinyinCorrections(
            "我们测试了逾期次过滤。", "逾期次→蓝牙", "",
            {"语气词"});
        assert(r.text == "我们测试了逾期次过滤。");
        assert(r.rejected.size() == 1);
    }
    {   // 热词整词匹配：dst 仅为热词的子串不算锚点（防意外放行窗口）
        const auto r = ApplyPinyinCorrections(
            "我们测试了逾期次过滤。", "逾期次→语气词", "",
            {"语气词表"});
        assert(r.text == "我们测试了逾期次过滤。");
        assert(r.rejected.size() == 1);
    }
    {   // 回归：无热词无上文仍拒（原锚点语义不变）
        const auto r = ApplyPinyinCorrections(
            "我们测试了逾期次过滤。", "逾期次→语气词", "");
        assert(r.text == "我们测试了逾期次过滤。");
        assert(r.rejected.size() == 1);
    }
    {   // V1 触类旁通：dst 比 src 恰好多 1 字且近音子序列对齐 → 放行。
        //     场景：热词「口水词」的少字变体「口水」（ASR 漏识别尾字）。
        const auto r = ApplyPinyinCorrections(
            "这些口水还没删干净。", "口水→口水词", "", {"口水词"});
        assert(r.text == "这些口水词还没删干净。");
        assert(r.rejected.empty());
    }
    {   // V1 变体含近音错字：水池→口水词（删「口」后「水词」vs「水池」，
        //     水=水、池 chí/词 cí 韵母交集同音——与 C01 渍/词同口径）
        const auto r = ApplyPinyinCorrections(
            "这些水池还没删干净。", "水池→口水词", "", {"口水词"});
        assert(r.text == "这些口水词还没删干净。");
        assert(r.rejected.empty());
    }
    {   // V1 +1 字对齐仍受锚点域约束：dst「水池子」可对齐但不在热词/上文 → 拒
        const auto r = ApplyPinyinCorrections(
            "这些水池还没删干净。", "水池→水池子", "", {"口水词"});
        assert(r.text == "这些水池还没删干净。");
        assert(r.rejected.size() == 1);
    }
    {   // V1 语义反转防护：不→很好（差 1 字但无任何近音对齐路径）→ 拒
        const auto r = ApplyPinyinCorrections(
            "这样不行的。", "不→很好", "", {"很好"});
        assert(r.text == "这样不行的。");
        assert(r.rejected.size() == 1);
    }
    {   // V1 长度差 2 仍拒：水→口水词
        const auto r = ApplyPinyinCorrections(
            "这些水还没删干净。", "水→口水词", "", {"口水词"});
        assert(r.text == "这些水还没删干净。");
        assert(r.rejected.size() == 1);
    }
    {   // V1 只放宽「错词漏字」方向：dst 更短（口水词→口水）→ 仍拒
        const auto r = ApplyPinyinCorrections(
            "这些口水词还没删干净。", "口水词→口水", "", {"口水"});
        assert(r.text == "这些口水词还没删干净。");
        assert(r.rejected.size() == 1);
    }
    {   // V4 热词锚定放宽：等长 dst 命中热词时不再要求逐字全近音，至少一个
        //     位置近音/同字即放行。真机案例「电楼板→洞洞板」（电 diǎn/洞 dòng
        //     韵母无交集），靠尾字「板」同字锚定（2026-09-10 用户实测被误杀）。
        const auto r = ApplyPinyinCorrections(
            "买电楼板了吗？", "电楼板→洞洞板", "", {"洞洞板"});
        assert(r.text == "买洞洞板了吗？");
        assert(r.rejected.empty());
    }
    {   // V4 防幻觉闸：dst 命中热词但与 src 无任何位置近音（今天→洞洞板）→ 拒
        const auto r = ApplyPinyinCorrections(
            "今天天气不错。", "今天→洞洞板", "", {"洞洞板"});
        assert(r.text == "今天天气不错。");
        assert(r.rejected.size() == 1);
    }
    {   // V4 长度闸不变：热词路径仍拒长度差 2（水→口水词，防 find 错位拼接）
        const auto r = ApplyPinyinCorrections(
            "我在喝水。", "水→口水词", "", {"口水词"});
        assert(r.text == "我在喝水。");
        assert(r.rejected.size() == 1);
    }
    {   // V4 上文锚定路径不放宽：dst 在上文但非热词、无近音 → 仍拒
        //     （放宽只给用户显式确认的热词，上文出现≠真值）
        const auto r = ApplyPinyinCorrections(
            "电楼板真不错。", "电楼板→洞楼板", "我说的是洞楼板", {});
        assert(r.text == "电楼板真不错。");
        assert(r.rejected.size() == 1);
    }
    {   // V4 畸形混行容错（4B 真机/冒烟实测输出形态）：同一批指令中
        //     「长句前缀→洞洞板」被长度闸拒、「动作板→洞洞板」正确放行，
        //     逐行独立裁决互不影响。
        const auto r = ApplyPinyinCorrections(
            "那我要到宜家里面去买一些动作板来看看这个效果怎么样。",
            "那我要到宜家里面去买一些动作板→洞洞板\n动作板→洞洞板",
            "", {"洞洞板"});
        assert(r.text ==
               "那我要到宜家里面去买一些洞洞板来看看这个效果怎么样。");
        assert(r.rejected.size() == 1);
    }
    printf("TestApplyPinyinCorrections passed\n");
}

// 划词纠错候选链路纯逻辑（S1，Doc/Plan/selection-hotword-correction-and-asr-hotword-spike.md §1.2）：
// LLM 输出解析 → 近音过滤 → 交给对话框展示。
void TestSelectionCorrection() {
    printf(">> TestSelectionCorrection\n"); fflush(stdout);
    {   // SameOrNearText：等长逐字近音（守卫基线口径）
        assert(SameOrNearText("逾期次", "语气词"));
        assert(SameOrNearText("鱼旗子", "逾期次"));
        assert(!SameOrNearText("逾期次", "蓝牙"));      // 码点不等长
        assert(!SameOrNearText("逾期次", "蓝牙耳机"));  // 等长但非近音
        assert(SameOrNearText("", ""));
    }
    {   // NearVariantText（V1 触类旁通口径）：等长近音 ∨ ±1 字近音子序列
        //     对齐（热词少字变体）；差 1 字但无近音对齐路径、差 2 字均拒
        assert(NearVariantText("逾期次", "语气词"));
        assert(NearVariantText("口水", "口水词"));    // 漏尾字变体
        assert(NearVariantText("水池", "口水词"));    // 漏首字+近音错字变体
        assert(!NearVariantText("逾期次", "蓝牙"));   // 差 1 但对不上
        assert(!NearVariantText("水", "口水词"));     // 差 2
        assert(NearVariantText("", ""));
    }
    {   // ParseCandidateLines：剥序号（1. / 1、/ -）、跳空行、跳「无」类
        //     直答、跳含标点行（候选是词不该有标点）、保序
        const auto lines = ParseCandidateLines(
            "1. 语气词\n"
            "2、鱼旗子\n"
            "\n"
            "无\n"
            "这行有逗号，跳过\n"
            "- 预期刺\n"
            "none\n"
            "3. 语气词\n");
        assert(lines.size() == 4);  // 保序不去重（去重在 FilterCandidates）
        assert(lines[0] == "语气词");
        assert(lines[1] == "鱼旗子");
        assert(lines[2] == "预期刺");
        assert(lines[3] == "语气词");
    }
    {   // FilterCandidates：近音过、非近音拒、去重、去与错词相同项；
        //     V1 起容忍 ±1 字近音子序列（「语气词语」是「逾期次」的
        //     +1 字变体，保留展示，用户点选是最终裁决）
        const auto r = FilterCandidates(
            "逾期次", {"语气词", "鱼旗子", "蓝牙", "语气词", "逾期次", "语气词语"}, {});
        assert(r.size() == 3);
        assert(r[0] == "语气词");
        assert(r[1] == "鱼旗子");
        assert(r[2] == "语气词语");
    }
    {   // FilterCandidates（V1 场景）：划「水池」时候选「口水词」保留
        const auto r = FilterCandidates("水池", {"口水词", "水库", "口水词"}, {});
        assert(r.size() == 1);
        assert(r[0] == "口水词");
    }
    {   // FilterCandidates（V4 热词同口径）：候选命中热词免逐字全近音，
        //     与错词至少一位近音/同字即保留（电楼板→洞洞板 靠尾字「板」）
        const auto r = FilterCandidates("电楼板", {"洞洞板", "电木板"}, {"洞洞板"});
        assert(r.size() == 1);
        assert(r[0] == "洞洞板");
    }
    {   // FilterCandidates（V4）：无热词时同候选仍按近音拒（放宽仅限热词）
        const auto r = FilterCandidates("电楼板", {"洞洞板"}, {});
        assert(r.empty());
    }
    {   // BuildCorrectionCandidatesPrompt：含错词与上下文；空上下文不空行残留；
        //     V3 热词注入——非空加「热词：」行引导从热词出候选，空表不残留
        const auto prompt = BuildCorrectionCandidatesPrompt("逾期次", "我们测了逾期次过滤");
        assert(prompt.find("逾期次") != std::string::npos);
        assert(prompt.find("我们测了逾期次过滤") != std::string::npos);
        const auto bare = BuildCorrectionCandidatesPrompt("逾期次", "");
        assert(bare.find("逾期次") != std::string::npos);
        assert(bare.find("上下文") == std::string::npos);
        assert(bare.find("热词：") == std::string::npos);  // 无热词行（约束句仍提热词）
        const auto with_hotwords =
            BuildCorrectionCandidatesPrompt("水池", "", {"口水词", "语气词"});
        assert(with_hotwords.find("热词：口水词，语气词") != std::string::npos);
    }
    printf("TestSelectionCorrection passed\n");
}

// client 候选生成编排（S1）：引擎输出 → 解析+近音过滤回调；失败给 (false,{})。
// 引擎调用经 engine 互斥与精修串行（同一 llama.cpp 实例非线程安全）。
void TestLocalRefinementGenerateCandidates() {
    printf(">> TestLocalRefinementGenerateCandidates\n"); fflush(stdout);
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;
        bool fail = false;
        std::string last_user;
        std::string last_system;
        bool Chat(const std::string& system_prompt, const std::string& user_text,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            last_system = system_prompt;
            last_user = user_text;
            if (fail) return false;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    struct Out {
        bool ok = false;
        std::vector<std::string> candidates;
        std::string last_user;
    };
    auto run = [](std::unique_ptr<FakeEngine> fake, const std::string& wrong,
                  const std::string& context,
                  std::vector<std::string> hotwords) {
        std::promise<Out> pr;
        auto fut = pr.get_future();
        FakeEngine* observer = fake.get();
        LocalRefinementClient client(std::move(fake));
        client.GenerateCandidates(
            wrong, context, std::move(hotwords),
            [&pr, observer](bool ok, std::vector<std::string> candidates) {
                Out out;
                out.ok = ok;
                out.candidates = std::move(candidates);
                if (observer) out.last_user = observer->last_user;
                pr.set_value(std::move(out));
            });
        return fut.get();
    };
    {   // 1) 多行候选：序号剥除 + 近音过滤 + 去重去原词
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "1. 语气词\n2、鱼旗子\n3. 蓝牙\n语气词";
        const auto r = run(std::move(fake), "逾期次", "我们测了逾期次过滤", {});
        assert(r.ok);
        assert(r.candidates.size() == 2);
        assert(r.candidates[0] == "语气词");
        assert(r.candidates[1] == "鱼旗子");
    }
    {   // 2) 模型直答「无」→ (true, {})
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        const auto r = run(std::move(fake), "逾期次", "", {});
        assert(r.ok);
        assert(r.candidates.empty());
    }
    {   // 3) 引擎失败 → (false, {})
        auto fake = std::make_unique<FakeEngine>();
        fake->fail = true;
        const auto r = run(std::move(fake), "逾期次", "", {});
        assert(!r.ok);
        assert(r.candidates.empty());
    }
    {   // 4) user 形态 = BuildCorrectionCandidatesPrompt(wrong, context)，
        //     system = 候选生成专用（区别于精修 few-shot/纠正指令两种）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        FakeEngine* observer = fake.get();
        std::promise<void> done;
        auto fut = done.get_future();
        LocalRefinementClient client(std::move(fake));
        client.GenerateCandidates(
            "逾期次", "我们测了逾期次过滤", {},
            [&done](bool, std::vector<std::string>) { done.set_value(); });
        fut.get();
        assert(observer->last_user ==
               BuildCorrectionCandidatesPrompt("逾期次", "我们测了逾期次过滤"));
        assert(observer->last_system ==
               BuildCandidatesSystemPrompt());
    }
    {   // 5) V3 热词注入：prompt 含「热词：」行；+1 字候选（口水词）经
        //     NearVariantText 过滤保留（划词「水池」触类旁通到热词）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "1. 口水词";
        const auto r = run(std::move(fake), "水池", "", {"口水词"});
        assert(r.ok);
        assert(r.last_user.find("热词：口水词") != std::string::npos);
        assert(r.candidates.size() == 1);
        assert(r.candidates[0] == "口水词");
    }
    printf("TestLocalRefinementGenerateCandidates passed\n");
}

// 历史缓冲：滑窗 5 轮、2 分钟 TTL 惰性过期、ContextText 拼接、Clear。
void TestRefineHistory() {
    printf(">> TestRefineHistory\n"); fflush(stdout);
    std::int64_t fake_now = 1'000;
    auto now = [&fake_now] { return fake_now; };
    RefineHistory h(/*max_turns=*/5, /*ttl_ms=*/120'000, now);
    {   // 滑窗：7 轮只留最近 5 轮
        for (int i = 1; i <= 7; ++i) {
            h.Add("raw" + std::to_string(i), "refined" + std::to_string(i));
        }
        const auto turns = h.Turns();
        assert(turns.size() == 5);
        assert(turns.front().raw_asr == "raw3");
        assert(turns.back().refined == "refined7");
        assert(h.ContextText() == "refined3。refined4。refined5。refined6。refined7");
    }
    {   // TTL 内不过期
        fake_now += 119'999;
        assert(h.Turns().size() == 5);
    }
    {   // 超时整体过期
        fake_now += 2;
        assert(h.Turns().empty());
        assert(h.ContextText().empty());
    }
    {   // 过期后重新累积，从新轮起算
        fake_now += 1'000;
        h.Add("raw_new", "refined_new");
        const auto turns = h.Turns();
        assert(turns.size() == 1 && turns[0].raw_asr == "raw_new");
        assert(h.ContextText() == "refined_new");
    }
    {   // Clear 立即清空
        h.Clear();
        assert(h.Turns().empty() && h.ContextText().empty());
    }
    {   // 默认时钟构造冒烟（真实 steady_clock，不会立即过期）
        RefineHistory real;
        real.Add("原文", "精修");
        assert(real.Turns().size() == 1);
        assert(real.ContextText() == "精修");
    }
    {   // 三参 Add：instruction 透传读回（KV 续写重放 assistant 侧数据源）；
        // 二参 Add 兼容旧调用（instruction 默认空 = 无指令轮次）
        RefineHistory h2(5, 120'000, now);
        h2.Add("raw1", "refined1", "鱼器渍→语气词");
        h2.Add("raw2", "refined2");
        const auto turns = h2.Turns();
        assert(turns.size() == 2);
        assert(turns[0].instruction == "鱼器渍→语气词");
        assert(turns[1].instruction.empty());
        // ContextText 域不含 instruction（守卫查找域只认 refined）
        assert(h2.ContextText() == "refined1。refined2");
    }
    printf("TestRefineHistory passed\n");
}


// 真模型 smoke：LlamaCppEngine 加载真实 Qwen3-1.7B GGUF 并连发两句（第二句
// 验证 KV 前缀复用延迟收敛）。模型解析与生产同口径（ResolveLocalRefineModelPath，
// env VOICESTICK_REFINE_MODEL 注入）；不在位时 SKIP——不 mock 真实链路。
// 仅 Release（NDEBUG）跑：Debug 无优化下 GGML 推理慢 20~40 倍（实测 prefix
// prefill 700ms/token vs Release ~3ms/token），单句 4 分钟起，全量回归不可接受。
void TestLlamaCppEngineRealModelSmoke() {
    printf(">> TestLlamaCppEngineRealModelSmoke\n"); fflush(stdout);
#ifdef VOICESTICK_LOCAL_REFINE_ENABLED
#ifndef NDEBUG
    printf("TestLlamaCppEngineRealModelSmoke SKIP（Debug 构建推理慢 20~40 倍，"
           "Release 专用）\n");
#else
    const std::string models_dir = ResolveLocalMicModelsDir("", ".");
    const std::string model = ResolveLocalRefineModelPath(models_dir, "");
    if (model.empty()) {
        printf("TestLlamaCppEngineRealModelSmoke SKIP（无精修模型；设 "
               "VOICESTICK_REFINE_MODEL 指向 Qwen3-1.7B GGUF 启用）\n");
        return;
    }
    printf("  model: %s\n", model.c_str()); fflush(stdout);
    auto t0 = std::chrono::steady_clock::now();
    auto engine = LlamaCppEngine::Create(model, 6);
    assert(engine != nullptr);
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    const std::string system = LocalRefinementClient::BuildSystemPrompt();
    const auto chat_once = [&](const std::string& text) {
        const auto start = std::chrono::steady_clock::now();
        std::string out;
        std::string partial;
        const bool ok = engine->Chat(
            system, "输入：" + text + "\n输出：",
            [&partial](std::string piece) {
                partial += piece;
                return true;
            },
            out);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        printf("  chat %lldms ok=%d out=%s\n", static_cast<long long>(ms),
               static_cast<int>(ok), out.substr(0, 60).c_str());
        fflush(stdout);
        return std::make_pair(ok, ms);
    };
    const auto [ok1, ms1] = chat_once("嗯，帮我把这个文件重命名一下。");
    const auto [ok2, ms2] = chat_once("呃，我想问一下今天的会议几点开始。");
    printf("  load=%lldms first=%lldms second=%lldms\n",
           static_cast<long long>(load_ms), static_cast<long long>(ms1),
           static_cast<long long>(ms2));
    assert(ok1 && ok2);
    // 预算宽松（构建机负载波动）：单句 ≤15s；KV 前缀复用后第二句不慢于首句。
    assert(ms1 <= 15000);
    assert(ms2 <= 15000);
    assert(ms2 <= ms1 + 500);
    printf("TestLlamaCppEngineRealModelSmoke passed\n");
#endif  // NDEBUG
#else
    printf("TestLlamaCppEngineRealModelSmoke SKIP（VOICESTICK_ENABLE_LOCAL_REFINE=OFF）\n");
#endif
}

void TestLocalAsrClientStartFailsWhenModelMissing() {
    LocalAsrClient client("Z:/voicestick/不存在的模型目录");
    assert(!client.Start());
    assert(!client.LastStartError().empty());
    printf("TestLocalAsrClientStartFailsWhenModelMissing passed\n");
}

// 跨轮纠错模型解析（M3）：cross_turn 且未显式配 refine_model 时优先 4B
//（M0 spike GO 口径），缺失回退 1.7B；显式配置尊重用户；两档全缺返回空
//（外壳退化为纯规则层）。
void TestResolveLocalRefineModelPathCrossTurn() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "vs_refine_path_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const auto m17 = dir / "Qwen3-1.7B-Q4_K_M" / "Qwen3-1.7B-Q4_K_M.gguf";
    const auto m4 = dir / "Qwen3-4B-Q4_K_M" / "Qwen3-4B-Q4_K_M.gguf";
    fs::create_directories(m17.parent_path(), ec);
    std::ofstream(m17, std::ios::binary) << "x";

    // 只有 1.7B：cross_turn 回退 1.7B，单句口径不受影响
    assert(ResolveLocalRefineModelPath(dir.string(), "", true) == m17.string());
    assert(ResolveLocalRefineModelPath(dir.string(), "", false) == m17.string());

    // 4B 在位：cross_turn 优先 4B；单句口径仍 1.7B（纯删除档维持轻量）
    fs::create_directories(m4.parent_path(), ec);
    std::ofstream(m4, std::ios::binary) << "x";
    assert(ResolveLocalRefineModelPath(dir.string(), "", true) == m4.string());
    assert(ResolveLocalRefineModelPath(dir.string(), "", false) == m17.string());

    // 显式配置（含跨轮开）：尊重用户自管，不按档位覆盖（显式值里的「/」
    // 分隔符被原样保留，用文件系统等价比较而非字符串相等）
    std::ofstream(dir / "Qwen3-1.7B-Q4_K_M" / "x.gguf", std::ios::binary) << "x";
    assert(fs::equivalent(
        fs::path(ResolveLocalRefineModelPath(dir.string(),
                                             "Qwen3-1.7B-Q4_K_M/x.gguf", true)),
        dir / "Qwen3-1.7B-Q4_K_M" / "x.gguf"));
    std::filesystem::remove(dir / "Qwen3-1.7B-Q4_K_M" / "x.gguf");

    // 两档全缺：空（外壳按缺模型退化纯规则）
    fs::remove(m4, ec);
    fs::remove(m17, ec);
    assert(ResolveLocalRefineModelPath(dir.string(), "", true).empty());
    fs::remove_all(dir, ec);
    printf("TestResolveLocalRefineModelPathCrossTurn passed\n");
}

// 4B 真模型 KV 续写 smoke（M2b）：验证 LlamaCppEngine::ChatSessionTurn 的
// 续例链形态（历史轮重放为 [输入：raw\n处理：][instruction]——形态自洽，
// assistant 侧=当轮模型真实指令输出）下——M0 spike 验证的是「上文：」行
// 形态，本测试证明 KV 续写形态效果等价：
// ①跨轮纠错指令仍产生且 ApplyPinyinCorrections 执行后命中期望（C 组案例
// 复刻）；②负例不误改；③第 6 轮触发滑窗重建后仍正常；④延迟收敛（观察值
// 打印）。env VOICESTICK_REFINE_MODEL_4B 或默认 m0/ 路径；不在位 SKIP——
// 不 mock 真实链路。Release only（GGML Debug 慢 20~40 倍）。
void TestLlamaCppEngineSessionTurnSmoke() {
    printf(">> TestLlamaCppEngineSessionTurnSmoke\n"); fflush(stdout);
#ifdef VOICESTICK_LOCAL_REFINE_ENABLED
#ifndef NDEBUG
    printf("TestLlamaCppEngineSessionTurnSmoke SKIP（Debug 构建推理慢，"
           "Release 专用）\n");
#else
    namespace fs = std::filesystem;
    const char* env = std::getenv("VOICESTICK_REFINE_MODEL_4B");
    fs::path model = (env && *env) ? fs::path(env)
                                   : fs::path("m0/models/Qwen3-4B-Q4_K_M")
                                         / "Qwen3-4B-Q4_K_M.gguf";
    if (!fs::exists(model)) {
        printf("TestLlamaCppEngineSessionTurnSmoke SKIP（无 4B 模型；设 "
               "VOICESTICK_REFINE_MODEL_4B 指向 Qwen3-4B GGUF 启用）\n");
        return;
    }
    printf("  model: %s\n", model.string().c_str()); fflush(stdout);
    auto engine = LlamaCppEngine::Create(model.string(), 6);
    if (!engine) {
        printf("   FAIL 引擎加载失败\n"); fflush(stdout);
        std::abort();
    }
    const std::string sys = LocalRefinementClient::BuildCorrectionSystemPrompt();

    // 会话轮（生产形态：轮 k 历史含前 k-1 轮，滑窗由调用方维护——手动推演）。
    // 案例复刻 M0 C 组（鱼器渍/蓝崖/设别/负例/章维）。
    struct Turn {
        const char* raw;        // 本轮 ASR 原文（指令执行前）
        const char* refined;    // 本轮期望精修结果（进守卫域）
        const char* note;
    };
    const Turn turns[] = {
        {"那些鱼器渍已经被过滤掉了。", "那些语气词已经被过滤掉了。", "C01 纠错"},
        {"这个蓝崖遥控器的按键手感不错。", "这个蓝牙遥控器的按键手感不错。", "C02 纠错"},
        {"这个语音设别模型是哪个？", "这个语音识别模型是哪个？", "C08 e/i 纠错"},
        {"帮我把垃圾倒一下。", "帮我把垃圾倒一下。", "负例不误改"},
        {"章维说他会晚点到。", "张伟说他会晚点到。", "C03 人名纠错"},
        {"嗯，帮我把这个文件重命名一下。", "帮我把这个文件重命名一下。",
         "第 6 轮：超 5 轮窗口触发滑窗重建"},
    };
    // 引擎历史（重放 assistant=当轮模型真实输出——指令形态自洽，防形态
    // 漂移为文本输出）与守卫域（各轮 refined 拼接）分开维护。
    // 建立轮（模拟此前口述）assistant=「无」（干净句的真实输出形态）。
    std::vector<std::pair<std::string, std::string>> engine_history = {
        {"我们刚才测了语气词过滤。", "无"},
    };
    std::vector<std::string> refined_history = {"我们刚才测了语气词过滤。"};
    const auto add_seed = [&](const char* text) {
        engine_history.emplace_back(text, "无");
        refined_history.emplace_back(text);
    };
    int corrected = 0;
    for (std::size_t k = 0; k < std::size(turns); ++k) {
        const auto& t = turns[k];
        if (k == 1) add_seed("帮我用蓝牙遥控器测试一下。");   // C02 建立蓝牙
        if (k == 2) add_seed("这个语音识别项目叫 VoiceStick。");  // C08 建立识别
        if (k == 4) add_seed("张伟下午的会议来不了。");        // C03 建立张伟
        const auto t0 = std::chrono::steady_clock::now();
        std::string completion;
        const bool ok = engine->ChatSessionTurn(
            sys, engine_history, "输入：" + std::string(t.raw) + "\n处理：",
            nullptr, completion);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        // Release（NDEBUG）下 assert 是 no-op——smoke 断言必须显式检查，
        // 否则假绿（教训同 TestImaAdpcmDecoderGoldenFixtures）。
        if (!ok || ms > 20000) {
            printf("   FAIL 轮%zu ok=%d ms=%lld\n", k + 1, (int)ok,
                   (long long)ms);
            fflush(stdout);
            std::abort();
        }
        // 守卫域=各轮 refined 拼接（client 语义同款）
        std::string context_all;
        for (const auto& r : refined_history) {
            if (!context_all.empty()) context_all += "。";
            context_all += r;
        }
        const auto stripped =
            LocalRefinementClient::StripReplyTemplate(completion);
        const auto outcome =
            ApplyPinyinCorrections(t.raw, stripped, context_all);
        // 轮 6（滑窗重建轮）目的=引擎存活且不误改：删「嗯，」或原样直通
        // 均算存活（删除指令是否产生不在重建路径验证范围）
        const bool hit = outcome.text == t.refined ||
                         (k == 5 && outcome.text == t.raw);
        printf("  轮%zu %s: %lldms completion=%.40s\n  final=%.30s (%s)\n",
               k + 1, t.note, static_cast<long long>(ms),
               completion.c_str(), outcome.text.c_str(), hit ? "HIT" : "MISS");
        fflush(stdout);
        if (hit) ++corrected;
        // 引擎历史推进：assistant=模型真实输出（形态自洽链）
        engine_history.emplace_back(t.raw, stripped);
        refined_history.push_back(outcome.text);
    }
    // 形态等价门槛：6 轮命中 ≥5（四纠错 + 负例 + 重建轮存活），对齐 M0
    // spike 4B「上文行」形态水平；负例（轮 4）不误改由期望文本断言覆盖
    printf("  命中 %d/6（含负例；门槛 5：四纠错 + 负例 + 重建轮存活）\n",
           corrected);
    if (corrected < 5) {
        fflush(stdout);
        std::abort();
    }
    printf("TestLlamaCppEngineSessionTurnSmoke passed\n");
#endif  // NDEBUG
#else
    printf("TestLlamaCppEngineSessionTurnSmoke SKIP（VOICESTICK_ENABLE_LOCAL_REFINE=OFF）\n");
#endif
}


void TestLocalAsrClientSenseVoiceSmoke() {
    const auto model_dir = DetectSenseVoiceDir();
    if (model_dir.empty()) {
        printf("TestLocalAsrClientSenseVoiceSmoke SKIPPED (模型不在位；"
               "设 VOICESTICK_SENSEVOICE_DIR 指向 SenseVoice 目录后重跑)\n");
        return;
    }
    std::vector<std::int16_t> pcm;
    assert(ReadMonoPcm16Wav(model_dir / "test_wavs" / "zh.wav", pcm));
    assert(pcm.size() >= AudioOpusEncoder::kFrameSamples);

    LocalAsrClient client(model_dir.string());
    assert(client.Start());

    std::mutex mutex;
    std::condition_variable done;
    std::vector<std::string> partial_texts;
    std::string final_text, error_text;
    bool finished = false;
    client.on_partial = [&](std::string text) {
        std::lock_guard<std::mutex> lock(mutex);
        partial_texts.push_back(std::move(text));
    };
    client.on_final = [&](std::string text) {
        std::lock_guard<std::mutex> lock(mutex);
        final_text = std::move(text);
        finished = true;
        done.notify_one();
    };
    client.on_error = [&](std::string error) {
        std::lock_guard<std::mutex> lock(mutex);
        error_text = std::move(error);
        finished = true;
        done.notify_one();
    };

    // PCM → Opus packets → Ogg 字节流，按 ~1s 一块流式发送（块间隔超过节流周期，
    // 复现"边说边识别"形态）：真模型应在录音期间就产出非空 partial。
    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    ByteVector block;
    int frames_in_block = 0;
    std::uint8_t packet[512];
    bool last = false;
    for (size_t off = 0; off < pcm.size() && !last;) {
        const size_t take = std::min<size_t>(AudioOpusEncoder::kFrameSamples,
                                             pcm.size() - off);
        std::vector<std::int16_t> frame(pcm.begin() + off, pcm.begin() + off + take);
        if (frame.size() < AudioOpusEncoder::kFrameSamples) {
            frame.resize(AudioOpusEncoder::kFrameSamples, 0);  // 尾帧补零
            last = true;
        }
        const auto result = encoder.Encode(frame.data(), frame.size(),
                                           packet, sizeof(packet));
        assert(result.encoded_bytes > 0);
        auto page = muxer.Append({packet, static_cast<size_t>(result.encoded_bytes)},
                                 false);
        block.insert(block.end(), page.begin(), page.end());
        off += take;
        if (++frames_in_block >= 25) {   // 25 帧 × 40ms = 1s
            client.SendOggOpusChunk(block, false);
            block.clear();
            frames_in_block = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }
    }
    auto tail = muxer.Finish();
    block.insert(block.end(), tail.begin(), tail.end());
    client.SendOggOpusChunk(block, true);

    std::unique_lock<std::mutex> lock(mutex);
    assert(done.wait_for(lock, std::chrono::seconds(60), [&] { return finished; }));
    assert(error_text.empty());
    assert(!final_text.empty());   // 真模型真推理：非空即链路通（不逐字断言）
    assert(!partial_texts.empty());   // 流式链路：录音期间至少一次非空 partial
    printf("TestLocalAsrClientSenseVoiceSmoke passed: partials=%zu final=%s\n",
           partial_texts.size(), final_text.c_str());
}

// ===== 本地 ASR 流式 partial（滚动重解码调度，假引擎驱动）=====

// 流式调度假引擎：Decode 返回 "n=<样本数>"（输入规模可直接断言），计数调用次数。
class FakeSenseVoiceEngine : public SenseVoiceEngine {
 public:
  std::string Decode(std::span<const std::int16_t> samples) override {
    std::lock_guard<std::mutex> lock(mutex);
    ++decode_calls;
    if (return_empty) return "";
    return "n=" + std::to_string(samples.size());
  }
  int DecodeCalls() const {
    std::lock_guard<std::mutex> lock(mutex);
    return decode_calls;
  }

  mutable std::mutex mutex;
  bool return_empty = false;

 private:
  int decode_calls = 0;
};


// 流式回调测试脚手架：partial 快照 + final/error 完成信号。
struct LocalAsrCallbacks {
    std::mutex mutex;
    std::condition_variable signal;
    int partial_calls = 0;
    std::vector<std::string> partial_texts;
    std::string final_text;
    std::string error_text;
    bool finished = false;

    void Install(LocalAsrClient& client) {
        client.on_partial = [this](std::string text) {
            std::lock_guard<std::mutex> lock(mutex);
            ++partial_calls;
            partial_texts.push_back(std::move(text));
            signal.notify_all();
        };
        client.on_final = [this](std::string text) {
            std::lock_guard<std::mutex> lock(mutex);
            final_text = std::move(text);
            finished = true;
            signal.notify_all();
        };
        client.on_error = [this](std::string error) {
            std::lock_guard<std::mutex> lock(mutex);
            error_text = std::move(error);
            finished = true;
            signal.notify_all();
        };
    }

    template <typename Pred>
    bool Wait(Pred pred, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex);
        return signal.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
    }
};


void TestLocalAsrClientEmitsPartialWhileStreaming() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    auto* fake = engine.get();
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    // 三块 1s 静音，块间隔 700ms > 600ms 节流周期：录音期间应持续产出 partial。
    for (int block = 0; block < 3; ++block) {
        auto chunk = EncodeSilenceFrames(encoder, muxer, 25);
        client.SendOggOpusChunk(chunk, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }
    assert(cb.Wait([&] { return cb.partial_calls >= 2; }, 5000));
    size_t prev_samples = 0;
    for (const auto& text : cb.partial_texts) {
        const size_t samples = SamplesFromFakeText(text);
        assert(samples >= prev_samples);   // 输入单调不减（增量解码不丢不重）
        prev_samples = samples;
    }
    // is_last 附带新音频：最终推理覆盖全量 3s（75 帧 × 640 样本）。
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    assert(cb.Wait([&] { return cb.finished; }, 5000));
    assert(cb.error_text.empty());
    assert(cb.final_text == "n=48000");
    assert(fake->DecodeCalls() >= 3);   // 至少：首块 + 两次周期 + final
    printf("TestLocalAsrClientEmitsPartialWhileStreaming passed: partials=%d\n",
           cb.partial_calls);
}

void TestLocalAsrClientPartialThrottled() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    auto* fake = engine.get();
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    // 五块 0.48s 静音在远小于节流周期的窗口内连发：无节流会逐块解码 5 次。
    for (int block = 0; block < 5; ++block) {
        auto chunk = EncodeSilenceFrames(encoder, muxer, 12);
        client.SendOggOpusChunk(chunk, false);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int calls = fake->DecodeCalls();
    assert(calls >= 1);
    assert(calls <= 3);   // 无节流 = 5
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    assert(cb.Wait([&] { return cb.finished; }, 5000));
    assert(cb.error_text.empty());
    printf("TestLocalAsrClientPartialThrottled passed: decode_calls=%d\n", calls);
}

void TestLocalAsrClientFinalReusesDecodeWhenNoNewAudio() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    auto* fake = engine.get();
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    auto chunk = EncodeSilenceFrames(encoder, muxer, 25);
    client.SendOggOpusChunk(chunk, false);
    assert(cb.Wait([&] { return cb.partial_calls >= 1; }, 5000));
    assert(fake->DecodeCalls() == 1);   // 首块立即解码一次
    // 尾页不含新音频 packet：is_last 应复用上次推理结果，不重复解码（松键秒出 final）。
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    assert(cb.Wait([&] { return cb.finished; }, 5000));
    assert(cb.error_text.empty());
    assert(cb.final_text == cb.partial_texts.back());
    assert(fake->DecodeCalls() == 1);
    printf("TestLocalAsrClientFinalReusesDecodeWhenNoNewAudio passed\n");
}

void TestLocalAsrClientSkipsEmptyPartial() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    engine->return_empty = true;
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    for (int block = 0; block < 2; ++block) {
        auto chunk = EncodeSilenceFrames(encoder, muxer, 25);
        client.SendOggOpusChunk(chunk, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    assert(cb.Wait([&] { return cb.finished; }, 5000));
    assert(cb.partial_calls == 0);   // 空文本 partial 不上报（保持 Listening 显示）
    assert(cb.error_text.empty());
    assert(cb.final_text.empty());   // 引擎返回空 → final 照发（沿用现行为）
    printf("TestLocalAsrClientSkipsEmptyPartial passed\n");
}

void TestLocalAsrClientCancelStopsPartial() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    auto chunk = EncodeSilenceFrames(encoder, muxer, 25);
    client.SendOggOpusChunk(chunk, false);
    assert(cb.Wait([&] { return cb.partial_calls >= 1; }, 5000));

    client.Cancel();
    const int partials_before = cb.partial_calls;
    auto more = EncodeSilenceFrames(encoder, muxer, 25);
    client.SendOggOpusChunk(more, false);
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    assert(cb.partial_calls == partials_before);   // 取消后不再产出 partial
    assert(!cb.finished);                          // 也不再有 final/error
    printf("TestLocalAsrClientCancelStopsPartial passed\n");
}

// ===== 本机麦克风模式（local-mic，迭代二）=====

void TestPushToTalkKeyParsing() {
    assert(*ParsePushToTalkKey("right ctrl") == VK_RCONTROL);
    assert(*ParsePushToTalkKey("Right Ctrl") == VK_RCONTROL);   // 大小写/空白不敏感
    assert(*ParsePushToTalkKey(" left ctrl ") == VK_LCONTROL);
    assert(*ParsePushToTalkKey("right shift") == VK_RSHIFT);
    assert(*ParsePushToTalkKey("left shift") == VK_LSHIFT);
    assert(*ParsePushToTalkKey("right alt") == VK_RMENU);
    assert(*ParsePushToTalkKey("left alt") == VK_LMENU);
    assert(*ParsePushToTalkKey("f8") == VK_F8);
    assert(*ParsePushToTalkKey("F24") == VK_F24);
    assert(*ParsePushToTalkKey("capslock") == VK_CAPITAL);
    assert(!ParsePushToTalkKey("ctrl").has_value());     // 左右歧义，拒绝
    assert(!ParsePushToTalkKey("ctrl+c").has_value());   // 组合键不支持（按住说话=单键）
    assert(!ParsePushToTalkKey("foo").has_value());
    assert(!ParsePushToTalkKey("").has_value());
}

void TestFormatPushToTalkKey() {
    // 哨兵：stub 阶段（恒 nullopt）在此干净失败，避免下方解引用空 optional 的 UB。
    assert(FormatPushToTalkKey(VK_RCONTROL).has_value());
    // 命名键：主名（同义 escape/return 取首见的 esc/enter）。
    assert(*FormatPushToTalkKey(VK_RCONTROL) == "right ctrl");
    assert(*FormatPushToTalkKey(VK_LCONTROL) == "left ctrl");
    assert(*FormatPushToTalkKey(VK_RSHIFT) == "right shift");
    assert(*FormatPushToTalkKey(VK_LSHIFT) == "left shift");
    assert(*FormatPushToTalkKey(VK_RMENU) == "right alt");
    assert(*FormatPushToTalkKey(VK_LMENU) == "left alt");
    assert(*FormatPushToTalkKey(VK_CAPITAL) == "capslock");
    assert(*FormatPushToTalkKey(VK_SCROLL) == "scrolllock");
    assert(*FormatPushToTalkKey(VK_PAUSE) == "pause");
    assert(*FormatPushToTalkKey(VK_ESCAPE) == "esc");
    assert(*FormatPushToTalkKey(VK_SPACE) == "space");
    assert(*FormatPushToTalkKey(VK_TAB) == "tab");
    assert(*FormatPushToTalkKey(VK_RETURN) == "enter");
    assert(*FormatPushToTalkKey(VK_BACK) == "backspace");
    // 功能键边界与单字符键。
    assert(*FormatPushToTalkKey(VK_F1) == "f1");
    assert(*FormatPushToTalkKey(VK_F9) == "f9");
    assert(*FormatPushToTalkKey(VK_F24) == "f24");
    assert(*FormatPushToTalkKey('A') == "a");
    assert(*FormatPushToTalkKey('Z') == "z");
    assert(*FormatPushToTalkKey('0') == "0");
    assert(*FormatPushToTalkKey('9') == "9");
    // Parse 本就不收的键：无键名。
    assert(!FormatPushToTalkKey(VK_LWIN).has_value());
    assert(!FormatPushToTalkKey(VK_UP).has_value());
    assert(!FormatPushToTalkKey(VK_NUMPAD0).has_value());
    assert(!FormatPushToTalkKey(VK_F24 + 1u).has_value());  // F25 起越界（SDK 无 VK_F25 常量）
    // 往返一致性：Format 输出可被 Parse 还原为同一 VK（全支持域）。
    const UINT round_trip_keys[] = {VK_RCONTROL, VK_LCONTROL, VK_RSHIFT, VK_LSHIFT,
                                    VK_RMENU,    VK_LMENU,    VK_CAPITAL, VK_SCROLL,
                                    VK_PAUSE,    VK_ESCAPE,   VK_SPACE,   VK_TAB,
                                    VK_RETURN,   VK_BACK,     VK_F1,      VK_F13,
                                    VK_F24,      'A',         'M',        'Z',
                                    '0',         '5',         '9'};
    for (UINT vk : round_trip_keys) {
        const auto name = FormatPushToTalkKey(vk);
        assert(name.has_value());
        const auto parsed = ParsePushToTalkKey(*name);
        assert(parsed.has_value());
        assert(*parsed == vk);
    }
}

// ===== 语音识别设置合并（本地识别为服务提供方末位虚拟项）=====

void TestProviderComboMapping() {
    // 无 legacy cloud 项：条目序 [Volcengine=0, Tencent=1, 本地=2]。
    assert(ProviderComboLocalIndex(false) == 2);
    assert(ProviderComboCloudAt(0, false) == AsrProvider::kVolcengine);
    assert(ProviderComboCloudAt(1, false) == AsrProvider::kTencent);
    assert(ProviderComboIsLocal(2, false));
    assert(!ProviderComboIsLocal(0, false));
    assert(!ProviderComboIsLocal(1, false));
    assert(ProviderComboCloudIndexOf(AsrProvider::kVolcengine, false) == 0);
    assert(ProviderComboCloudIndexOf(AsrProvider::kTencent, false) == 1);
    // 带 legacy cloud 项（老配置 asr_provider=voicestick_cloud 时 0 号位临时插入）：
    // 条目序 [Cloud=0, Volcengine=1, Tencent=2, 本地=3]。
    assert(ProviderComboLocalIndex(true) == 3);
    assert(ProviderComboCloudAt(0, true) == AsrProvider::kVoiceStickCloud);
    assert(ProviderComboCloudAt(1, true) == AsrProvider::kVolcengine);
    assert(ProviderComboCloudAt(2, true) == AsrProvider::kTencent);
    assert(ProviderComboIsLocal(3, true));
    assert(!ProviderComboIsLocal(2, true));
    assert(ProviderComboCloudIndexOf(AsrProvider::kVoiceStickCloud, true) == 0);
    assert(ProviderComboCloudIndexOf(AsrProvider::kVolcengine, true) == 1);
    assert(ProviderComboCloudIndexOf(AsrProvider::kTencent, true) == 2);
    // 越界防御：CB_ERR(-1) 或超界索引一律判非本地，不误触发模型目录行显隐。
    assert(!ProviderComboIsLocal(-1, false));
    assert(!ProviderComboIsLocal(-1, true));
    assert(!ProviderComboIsLocal(99, false));
}

void TestResolveAndValidateModelsDir() {
    namespace fs = std::filesystem;
    // 解析口径：空 = exe_dir/models；相对路径锚 exe 目录；绝对路径原样。
    // 经 fs::path 比较（path 拼接用反斜杠分隔符，字符串形态不作断言目标）。
    assert(fs::path(ResolveLocalMicModelsDir("", "C:/app")) == fs::path("C:/app/models"));
    assert(fs::path(ResolveLocalMicModelsDir("models", "C:/app")) ==
           fs::path("C:/app/models"));
    assert(fs::path(ResolveLocalMicModelsDir("rel/models", "C:/app")) ==
           fs::path("C:/app/rel/models"));
    assert(fs::path(ResolveLocalMicModelsDir("D:/mymodels", "C:/app")) ==
           fs::path("D:/mymodels"));
    // 校验口径：目录缺任一模型文件即报错，齐全才通过（与 Start 同判定）。
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "voicestick_models_validate_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // 哨兵：stub（恒通过）在此干净失败。
    assert(ValidateSenseVoiceModelsDir(dir.string()).has_value());
    { std::ofstream out(dir / "model.int8.onnx", std::ios::binary); }
    assert(ValidateSenseVoiceModelsDir(dir.string()).has_value());  // 只有一半
    { std::ofstream out(dir / "tokens.txt", std::ios::binary); }
    assert(!ValidateSenseVoiceModelsDir(dir.string()).has_value());  // 齐全 → 通过
    fs::remove_all(dir);
    // 不存在的目录同样报错。
    assert(ValidateSenseVoiceModelsDir("Z:/definitely/not/here").has_value());
}

void TestShortcutCaptureClassifyKey() {
    // 默认（按键映射场景，require_modifier=false）：修饰键累积、主键直接捕获。
    ShortcutCapture::Options single;
    assert(ShortcutCapture::ClassifyKey(VK_RCONTROL, single, false) ==
           ShortcutCapture::KeyAction::kAccumulateModifier);
    assert(ShortcutCapture::ClassifyKey(VK_LSHIFT, single, true) ==
           ShortcutCapture::KeyAction::kAccumulateModifier);
    assert(ShortcutCapture::ClassifyKey(VK_LWIN, single, false) ==
           ShortcutCapture::KeyAction::kAccumulateModifier);
    assert(ShortcutCapture::ClassifyKey('A', single, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_F9, single, false) ==
           ShortcutCapture::KeyAction::kCapture);

    // 全局热键场景（require_modifier=true）：裸主键拒绝，带修饰键捕获。
    ShortcutCapture::Options combo;
    combo.require_modifier = true;
    assert(ShortcutCapture::ClassifyKey('A', combo, false) ==
           ShortcutCapture::KeyAction::kRejectNoModifier);
    assert(ShortcutCapture::ClassifyKey('A', combo, true) ==
           ShortcutCapture::KeyAction::kCapture);

    // 按住说话场景（allow_modifier_as_key）：修饰键左右变体直接作为主键捕获，
    // 不再累积等待（right ctrl 即功能键本身）。
    ShortcutCapture::Options ptt;
    ptt.allow_modifier_as_key = true;
    assert(ShortcutCapture::ClassifyKey(VK_RCONTROL, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_LCONTROL, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_LSHIFT, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_LWIN, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_CAPITAL, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);

    // Esc 恒为取消（任何模式下都不作为主键捕获）。
    assert(ShortcutCapture::ClassifyKey(VK_ESCAPE, single, false) ==
           ShortcutCapture::KeyAction::kCancel);
    assert(ShortcutCapture::ClassifyKey(VK_ESCAPE, combo, true) ==
           ShortcutCapture::KeyAction::kCancel);
    assert(ShortcutCapture::ClassifyKey(VK_ESCAPE, ptt, false) ==
           ShortcutCapture::KeyAction::kCancel);
}

void TestShortcutCapturePollEligibleVk() {
    // 鼠标键与保留区不采纳：避免点击「录入」按钮/切换窗口被误判为按键。
    assert(!ShortcutCapture::IsPollEligibleVk(0x00));
    assert(!ShortcutCapture::IsPollEligibleVk(VK_LBUTTON));
    assert(!ShortcutCapture::IsPollEligibleVk(VK_RBUTTON));
    assert(!ShortcutCapture::IsPollEligibleVk(VK_MBUTTON));
    assert(!ShortcutCapture::IsPollEligibleVk(VK_XBUTTON2));
    assert(!ShortcutCapture::IsPollEligibleVk(0x07));
    assert(!ShortcutCapture::IsPollEligibleVk(0xFF));
    // 键盘区全覆盖：Backspace/字母/方向/功能/媒体键均可经轮询兜底捕获。
    assert(ShortcutCapture::IsPollEligibleVk(VK_BACK));
    assert(ShortcutCapture::IsPollEligibleVk('A'));
    assert(ShortcutCapture::IsPollEligibleVk(VK_UP));
    assert(ShortcutCapture::IsPollEligibleVk(VK_LCONTROL));
    assert(ShortcutCapture::IsPollEligibleVk(VK_F13));
    assert(ShortcutCapture::IsPollEligibleVk(VK_VOLUME_UP));
    assert(ShortcutCapture::IsPollEligibleVk(VK_BROWSER_BACK));
    assert(ShortcutCapture::IsPollEligibleVk(0xFE));
}

void TestAppConfigLocalAsrRoundTrip() {
    assert(!AppConfig::Defaults().local_asr.enabled);
    assert(AppConfig::Defaults().local_asr.models_dir.empty());
    assert(AppConfig::Defaults().local_asr.push_to_talk_key == "right ctrl");
    // 跨轮纠错默认关（M3：改写能力以设置开关观察）
    assert(!AppConfig::Defaults().local_asr.refine_cross_turn);

    auto temp = std::filesystem::temp_directory_path() / "voicestick_local_asr_test.toml";
    std::filesystem::remove(temp);

    AppConfig config = AppConfig::Defaults();
    config.local_asr.enabled = true;
    config.local_asr.models_dir = "C:/models/sensevoice";
    config.local_asr.push_to_talk_key = "f8";
    config.local_asr.refine_prompt = "提示词第一行\n输入：嗯 x\n输出：x";
    config.local_asr.refine_cross_turn = true;
    config.Save(temp);

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.local_asr.enabled);
    assert(loaded.local_asr.models_dir == "C:/models/sensevoice");
    assert(loaded.local_asr.push_to_talk_key == "f8");
    // 多行提示词（含换行与中文）往返保持原样
    assert(loaded.local_asr.refine_prompt == config.local_asr.refine_prompt);
    assert(loaded.local_asr.refine_cross_turn);

    // 默认关不落盘（Save 降噪路径）：重存关闭态后文件中无该键
    loaded.local_asr.refine_cross_turn = false;
    loaded.Save(temp);
    const std::string saved = [&] {
        std::ifstream in(temp, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();
    assert(saved.find("refine_cross_turn") == std::string::npos);

    std::filesystem::remove(temp);
}

// 按住说话全链路：热键按下建立 local-mic 会话并启动采集；PCM 喂入走 Opus 主会话
// 管线；释放后尾帧+END 收尾，音频路由到本地 ASR（云端客户端零触碰），final 文本注入。
void TestWasapiMicCaptureSmoke() {
    WasapiMicCapture capture;
    std::mutex mutex;
    std::condition_variable got_pcm;
    std::size_t total_samples = 0;
    int callbacks = 0;
    capture.on_pcm = [&](std::span<const std::int16_t> pcm) {
        std::lock_guard<std::mutex> lock(mutex);
        total_samples += pcm.size();
        ++callbacks;
        got_pcm.notify_one();
    };
    printf("  wasapi: Start()...\n"); fflush(stdout);
    if (!capture.Start()) {
        printf("TestWasapiMicCaptureSmoke skipped: %s\n", capture.LastStartError().c_str());
        return;
    }
    printf("  wasapi: started, waiting pcm\n"); fflush(stdout);
    bool received = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        received = got_pcm.wait_for(lock, std::chrono::seconds(8),
                                    [&] { return total_samples >= 16000; });
    }
    printf("  wasapi: wait done received=%d samples=%zu callbacks=%d\n",
           received ? 1 : 0, total_samples, callbacks); fflush(stdout);
    assert(received);
    assert(total_samples >= 16000);
    capture.Stop();
    capture.Stop();  // 幂等
    printf("TestWasapiMicCaptureSmoke passed: %d callbacks, %zu samples\n",
           callbacks, total_samples);
}

// ===== 剪贴板 vault（迭代三：完整格式恢复，移植自 P1 clipboard_vault）=====
// 真 Win32 剪贴板，非 mock：字节级快照/恢复是本组件的全部契约。测试动本机
// 剪贴板，开头快照用户当前内容，结尾尽力还原。

namespace {





} // namespace

void TestClipboardVaultMultiFormatRoundTrip() {
    // 快照/恢复用户当前剪贴板，尽力不破坏现场。
    std::optional<ClipboardSnapshot> user_content;
    try {
        user_content = ClipboardVault().Save();
    } catch (const std::runtime_error&) {
    }

    const UINT custom_fmt = RegisterClipboardFormatW(L"VoiceStickVaultTestFmt");
    assert(custom_fmt != 0);
    const std::vector<BYTE> dib(64, 0xAB);  // 伪 DIB：剪贴板不校验内容
    const std::vector<BYTE> custom{0x00, 0x01, 0xFF, 0x00, 0x7F};
    const auto text_bytes = VaultBytesOf(L"原始内容-restore");
    VaultSetClipboard({{CF_UNICODETEXT, text_bytes}, {CF_DIB, dib}, {custom_fmt, custom}});

    ClipboardVault vault;
    const ClipboardSnapshot snapshot = vault.Save();
    const auto* text_entry = snapshot.Find(CF_UNICODETEXT);
    const auto* dib_entry = snapshot.Find(CF_DIB);
    const auto* custom_entry = snapshot.Find(custom_fmt);
    assert(text_entry && text_entry->data == text_bytes);
    assert(dib_entry && dib_entry->data == dib);
    assert(custom_entry && custom_entry->data == custom);

    // 快照后剪贴板被异物覆盖，恢复必须还原快照字节（注入借道剪贴板的核心场景）。
    VaultSetClipboard({{CF_UNICODETEXT, VaultBytesOf(L"覆盖内容")}});
    assert(vault.Restore(snapshot));
    assert(VaultGetBytes(CF_UNICODETEXT) == text_bytes);
    assert(VaultGetBytes(CF_DIB) == dib);
    assert(VaultGetBytes(custom_fmt) == custom);

    if (user_content) ClipboardVault().Restore(*user_content);
}

void TestClipboardVaultSkipsHandleFormats() {
    std::optional<ClipboardSnapshot> user_content;
    try {
        user_content = ClipboardVault().Save();
    } catch (const std::runtime_error&) {
    }

    // CF_BITMAP 是句柄类格式（非 HGLOBAL，GlobalLock 无意义）：Save 必须跳过；
    // 恢复后位图丢失为已知限制（位图场景应用几乎都同时提供 CF_DIB 内存版）。
    assert(VaultOpenClipboardWithRetry());
    EmptyClipboard();
    {
        const wchar_t* text = L"带位图的文本";
        const SIZE_T bytes = (wcslen(text) + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        void* ptr = GlobalLock(memory);
        assert(ptr != nullptr);
        memcpy(ptr, text, bytes);
        GlobalUnlock(memory);
        assert(SetClipboardData(CF_UNICODETEXT, memory));
    }
    const HBITMAP bitmap = CreateBitmap(1, 1, 1, 1, nullptr);
    assert(bitmap != nullptr);
    assert(SetClipboardData(CF_BITMAP, bitmap));  // 句柄移交剪贴板，不得 DeleteObject
    CloseClipboard();

    ClipboardVault vault;
    const ClipboardSnapshot snapshot = vault.Save();
    assert(snapshot.Find(CF_UNICODETEXT) != nullptr);
    assert(snapshot.Find(CF_BITMAP) == nullptr);  // 句柄格式不进快照
    // 布置 CF_BITMAP 时系统枚举会同时给出可从位图合成的 CF_DIB，快照经 CF_DIB
    // 保住图像字节——恢复后系统可再合成位图，图像内容实际不丢（优于“直接丢弃”）。
    const auto* dib_entry = snapshot.Find(CF_DIB);

    VaultSetClipboard({{CF_UNICODETEXT, VaultBytesOf(L"覆盖")}});
    assert(vault.Restore(snapshot));
    assert(VaultGetBytes(CF_UNICODETEXT) == snapshot.Find(CF_UNICODETEXT)->data);
    if (dib_entry != nullptr) {
        assert(VaultGetBytes(CF_DIB) == dib_entry->data);
        assert(IsClipboardFormatAvailable(CF_BITMAP));  // 从 CF_DIB 可再合成位图
    }

    if (user_content) ClipboardVault().Restore(*user_content);
}

void TestClipboardVaultEmptyClipboardSnapshot() {
    std::optional<ClipboardSnapshot> user_content;
    try {
        user_content = ClipboardVault().Save();
    } catch (const std::runtime_error&) {
    }

    assert(VaultOpenClipboardWithRetry());
    EmptyClipboard();
    CloseClipboard();

    ClipboardVault vault;
    const ClipboardSnapshot snapshot = vault.Save();
    assert(snapshot.entries.empty());  // 空快照只代表真空剪贴板

    // 空快照恢复 = 清空剪贴板（区别于“打不开”：那是 Save 抛错的职责，防误清）。
    VaultSetClipboard({{CF_UNICODETEXT, VaultBytesOf(L"x")}});
    assert(vault.Restore(snapshot));
    assert(!IsClipboardFormatAvailable(CF_UNICODETEXT));

    if (user_content) ClipboardVault().Restore(*user_content);
}

void TestClipboardVaultSaveThrowsWhenBusy() {
    // 另一线程持有剪贴板：Save 必须抛错而非返回空快照——空快照会让 Restore
    // 误清用户剪贴板（P1 验证语义）。本线程重试窗口 2×10ms，持有 80ms 必失败。
    std::atomic<bool> held{false};
    std::thread holder([&held] {
        if (OpenClipboard(nullptr)) {
            held = true;
            Sleep(80);
            CloseClipboard();
        }
    });
    bool opened = false;
    for (int i = 0; i < 500 && !held; ++i) {  // 等持有方拿到锁（最多 500ms）
        Sleep(1);
        opened = opened || held;
    }
    if (!held) {
        holder.join();
        printf("TestClipboardVaultSaveThrowsWhenBusy skipped: holder open failed\n");
        return;
    }
    ClipboardVault vault(/*open_retries=*/2, /*retry_delay_ms=*/10);
    bool threw = false;
    try {
        (void)vault.Save();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    holder.join();
}

// ================== 本地模型分发（Doc/Plan/local-model-distribution.md） ==================

namespace {

// 测试内独立 SHA-256：与被测实现各算各的，避免自证。
std::string TestSha256Hex(std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        std::fprintf(stderr, "TestSha256Hex: BCryptOpenAlgorithmProvider failed\n");
        std::abort();
    }
    std::uint8_t digest[32] = {};
    const NTSTATUS status =
        BCryptHash(algorithm, nullptr, 0,
                   reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                   static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) {
        std::fprintf(stderr, "TestSha256Hex: BCryptHash failed\n");
        std::abort();
    }
    char hex[65] = {};
    for (std::size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return hex;
}


// 回环 HTTP 服务（真 Winsock 真 HTTP 报文）：WinHTTP 走真实回环 TCP 连接，
// 不 mock 网络栈（不伪造原则）。单请求/连接，Connection: close。
class LoopbackHttpServer {
 public:
    struct Response {
        int status = 200;
        std::string body;
        bool honor_range = true;   // 支持 Range → 206 + Content-Range
        std::size_t chunk_size = 0;  // 0 = 一次性发送；否则分块
        int chunk_delay_ms = 0;      // 分块间隔（取消测试用）
    };
    // 未注册路径一律 404。range_start 仅当请求带 Range 时有效。
    using Rules = std::map<std::string, Response>;

    explicit LoopbackHttpServer(Rules rules) : rules_(std::move(rules)) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::fprintf(stderr, "LoopbackHttpServer: WSAStartup failed\n");
            std::abort();
        }
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) {
            std::fprintf(stderr, "LoopbackHttpServer: socket failed\n");
            std::abort();
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;  // 随机端口，避免并行会话互踩
        if (bind(listen_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listen_, 8) != 0) {
            std::fprintf(stderr, "LoopbackHttpServer: bind/listen failed\n");
            std::abort();
        }
        sockaddr_in bound{};
        int bound_size = sizeof(bound);
        if (getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
            std::fprintf(stderr, "LoopbackHttpServer: getsockname failed\n");
            std::abort();
        }
        port_ = ntohs(bound.sin_port);
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~LoopbackHttpServer() {
        stopping_.store(true);
        closesocket(listen_);  // 唤醒阻塞中的 accept
        if (accept_thread_.joinable()) accept_thread_.join();
        WSACleanup();
    }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    std::string Url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    // 取走已收到的 Range 头原文（含 "Range:" 前缀），供断言真的发了续传请求。
    std::vector<std::string> TakeRangeHeaders() {
        std::lock_guard<std::mutex> lock(ranges_mutex_);
        return std::exchange(range_headers_, {});
    }

 private:
    void AcceptLoop() {
        while (!stopping_.load()) {
            sockaddr_in peer{};
            int peer_size = sizeof(peer);
            SOCKET client = accept(listen_, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (client == INVALID_SOCKET) break;  // closesocket 触发，正常退出
            std::thread([this, client] { ServeConnection(client); }).detach();
        }
    }

    void ServeConnection(SOCKET client) {
        std::string request;
        char buffer[1024];
        for (;;) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            request.append(buffer, got);
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }
        const auto first_line_end = request.find("\r\n");
        const std::string first_line = request.substr(0, first_line_end);
        const auto path_begin = first_line.find(' ');
        const auto path_end = first_line.rfind(' ');
        if (path_begin == std::string::npos || path_end == std::string::npos ||
            path_end <= path_begin) {
            closesocket(client);
            return;
        }
        const std::string path =
            first_line.substr(path_begin + 1, path_end - path_begin - 1);

        // 找行首 "range:" 头并解析 "bytes=N-"（大小写不敏感）。
        bool has_range = false;
        std::uint64_t range_start = 0;
        std::string lowered;
        lowered.reserve(request.size());
        for (const char ch : request) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        for (std::size_t pos = lowered.find("range:"); pos != std::string::npos;
             pos = lowered.find("range:", pos + 6)) {
            if (pos != 0 && lowered[pos - 1] != '\n') continue;
            std::size_t value_begin = pos + 6;
            while (value_begin < request.size() &&
                   (request[value_begin] == ' ' || request[value_begin] == '\t')) {
                ++value_begin;
            }
            const std::size_t line_end = lowered.find("\r\n", pos);
            const std::string value =
                request.substr(value_begin, line_end == std::string::npos
                                                ? std::string::npos
                                                : line_end - value_begin);
            if (value.rfind("bytes=", 0) == 0) {
                const auto num_end = value.find('-', 6);
                if (num_end != std::string::npos && num_end > 6) {
                    range_start = std::stoull(value.substr(6, num_end - 6));
                    has_range = true;
                    std::lock_guard<std::mutex> lock(ranges_mutex_);
                    range_headers_.push_back(value);
                }
            }
            break;
        }

        Response response;  // 未注册路径 → 404
        if (const auto rule = rules_.find(path); rule != rules_.end()) {
            response = rule->second;
        } else {
            response.status = 404;
        }
        std::string body = response.body;
        int status = response.status;
        if (response.status == 200 && response.honor_range && has_range) {
            status = 206;
            body = range_start >= body.size() ? std::string() : body.substr(range_start);
        }
        std::string headers = "HTTP/1.1 " + std::to_string(status) + "\r\n";
        if (status == 206) {
            headers += "Content-Range: bytes " + std::to_string(range_start) + "-" +
                       std::to_string(response.body.empty() ? 0 : response.body.size() - 1) +
                       "/" + std::to_string(response.body.size()) + "\r\n";
        }
        headers += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        headers += "Connection: close\r\n\r\n";
        send(client, headers.data(), static_cast<int>(headers.size()), 0);
        if (response.chunk_size == 0) {
            send(client, body.data(), static_cast<int>(body.size()), 0);
        } else {
            for (std::size_t offset = 0; offset < body.size() && !stopping_.load();
                 offset += response.chunk_size) {
                const std::size_t n = std::min(response.chunk_size, body.size() - offset);
                send(client, body.data() + offset, static_cast<int>(n), 0);
                Sleep(response.chunk_delay_ms);
            }
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
    }

    Rules rules_;
    SOCKET listen_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
    std::mutex ranges_mutex_;
    std::vector<std::string> range_headers_;
};





}  // namespace

void TestBundledModelManifestWellFormed() {
    int failed = 0;
    const auto& bundled = BundledModelEntries();

    // 内置清单的具体值与 m0/models 权威副本一致（哈希/体积为实测回填）。
    if (bundled.size() != 2) {
        std::printf("FAIL manifest 应有 2 个条目，实际 %zu\n", bundled.size());
        ++failed;
    }
    if (!bundled.empty() && bundled[0].kind != ModelKind::kAsr) {
        std::printf("FAIL 首条目应为 kAsr\n");
        ++failed;
    }
    if (!bundled.empty() && !bundled[0].required) {
        std::printf("FAIL ASR 条目应为必选\n");
        ++failed;
    }
    if (bundled.size() > 0 && bundled[0].files.size() == 2) {
        const auto& onnx = bundled[0].files[0];
        if (onnx.rel_path != "model.int8.onnx" || onnx.bytes != 239233841 ||
            onnx.sha256.rfind("c71f0ce00bec95b07744e116345e33d8", 0) != 0 ||
            onnx.urls.size() != 4) {
            std::printf("FAIL onnx 文件描述与权威副本不符\n");
            ++failed;
        }
        const auto& tokens = bundled[0].files[1];
        if (tokens.rel_path != "tokens.txt" || tokens.bytes != 315894 ||
            tokens.urls.size() != 4) {
            std::printf("FAIL tokens 文件描述与权威副本不符\n");
            ++failed;
        }
    } else if (bundled.size() > 0) {
        std::printf("FAIL ASR 条目应含 2 个文件\n");
        ++failed;
    }
    if (bundled.size() > 1) {
        const auto& refine = bundled[1];
        if (refine.kind != ModelKind::kRefine || refine.required ||
            refine.files.size() != 1 ||
            refine.files[0].rel_path != "Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf" ||
            refine.files[0].bytes != 1107409472 ||
            refine.files[0].sha256.rfind("b139949c5bd74937ad8ed8c8cf3d9ffb", 0) != 0) {
            std::printf("FAIL 精修条目与权威副本不符\n");
            ++failed;
        }
    }
    if (!ModelEntriesWellFormed(bundled)) {
        std::printf("FAIL 内置清单应通过自洽校验\n");
        ++failed;
    }

    // 分发源分布护栏（Doc/Ref/cos-distribution.md）：四源回退——
    // 正式域名直出首位（DNS 未配时快速失败自动回退，无需改清单）、
    // myqcloud 直出、ModelScope 免费分流、GitHub Release 海外回退。
    for (const auto& entry : bundled) {
        for (const auto& file : entry.files) {
            if (file.urls.empty() ||
                file.urls[0].rfind("https://dl.davenger.cloud/models/", 0) != 0) {
                std::printf("FAIL %s 首位源应为正式域名直出\n",
                            file.rel_path.c_str());
                ++failed;
            }
            const auto has_host = [&file](const char* needle) {
                for (const auto& url : file.urls) {
                    if (url.find(needle) != std::string::npos) return true;
                }
                return false;
            };
            if (!has_host(".myqcloud.com/")) {
                std::printf("FAIL %s 应含 myqcloud 直出回退源\n",
                            file.rel_path.c_str());
                ++failed;
            }
            if (!has_host("modelscope.cn/")) {
                std::printf("FAIL %s 应含 ModelScope 分流源\n",
                            file.rel_path.c_str());
                ++failed;
            }
            if (!has_host("github.com/")) {
                std::printf("FAIL %s 应含 GitHub Release 回退源\n",
                            file.rel_path.c_str());
                ++failed;
            }
        }
    }

    // 自洽校验的拒绝分支（构造畸形清单逐字段破坏）。
    const auto valid_entry = [] {
        ModelEntrySpec entry;
        entry.kind = ModelKind::kAsr;
        entry.required = true;
        ModelFileSpec file;
        file.rel_path = "a.onnx";
        file.bytes = 1;
        file.sha256 = std::string(64, 'a');
        file.urls = {"https://host/path"};
        entry.files = {std::move(file)};
        return entry;
    };
    const std::vector<ModelEntrySpec> valid = {valid_entry()};
    if (!ModelEntriesWellFormed(valid)) {
        std::printf("FAIL 合法清单不应被拒\n");
        ++failed;
    }
    if (ModelEntriesWellFormed({})) {
        std::printf("FAIL 空清单应被拒\n");
        ++failed;
    }
    auto reject = [&](const char* why, std::vector<ModelEntrySpec> broken) {
        if (ModelEntriesWellFormed(broken)) {
            std::printf("FAIL %s 应被拒\n", why);
            ++failed;
        }
    };
    auto no_files = valid_entry();
    no_files.files.clear();
    reject("空 files", {no_files});
    auto empty_rel = valid_entry();
    empty_rel.files[0].rel_path.clear();
    reject("空 rel_path", {empty_rel});
    auto backslash_rel = valid_entry();
    backslash_rel.files[0].rel_path = "a\\b.onnx";
    reject("反斜杠 rel_path", {backslash_rel});
    auto zero_bytes = valid_entry();
    zero_bytes.files[0].bytes = 0;
    reject("bytes=0", {zero_bytes});
    auto short_hash = valid_entry();
    short_hash.files[0].sha256 = "abc";
    reject("短 sha256", {short_hash});
    auto upper_hash = valid_entry();
    upper_hash.files[0].sha256 = std::string(64, 'A');
    reject("非小写 sha256", {upper_hash});
    auto no_urls = valid_entry();
    no_urls.files[0].urls.clear();
    reject("空 urls", {no_urls});
    auto ftp_url = valid_entry();
    ftp_url.files[0].urls = {"ftp://host/path"};
    reject("非 http(s) url", {ftp_url});

    AbortIfFailed(failed, "TestBundledModelManifestWellFormed");
}

void TestModelFilePresentAndVerified() {
    namespace fs = std::filesystem;
    // C5：在位判定 = 存在 + 尺寸 + SHA-256 三者齐全；同尺寸损坏/被替换文件必须判不在位。
    const auto dir = fs::temp_directory_path() / "voicestick_c5_present_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto file = dir / "sample.bin";
    const std::string hello_sha =
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";  // "hello"
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "hello";
    }

    ModelFileSpec spec;
    spec.bytes = 5;
    spec.sha256 = hello_sha;
    assert(VerifyFileSha256(file, hello_sha));                // 底层包装直接可用
    assert(ModelFilePresentAndVerified(file, spec));           // 三者齐全 → 在位

    spec.sha256 = "2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824";
    assert(ModelFilePresentAndVerified(file, spec));           // 大小写不敏感

    spec.sha256 = std::string(64, '0');                        // 同尺寸但内容不同
    assert(!ModelFilePresentAndVerified(file, spec));          // 核心回归：不得静默接受

    spec.sha256 = hello_sha;
    spec.bytes = 6;                                            // 尺寸不符
    assert(!ModelFilePresentAndVerified(file, spec));

    spec.bytes = 5;
    assert(!ModelFilePresentAndVerified(dir / "missing.bin", spec));  // 不存在
    spec.sha256 = "abc";                                             // 非法哈希长度
    assert(!ModelFilePresentAndVerified(file, spec));

    fs::remove_all(dir, ec);
}

void TestModelDownloaderPureFunctions() {
    int failed = 0;

    // ParseModelUrl。
    ParsedModelUrl parsed;
    if (!ParseModelUrl("https://modelscope.cn/models/x/resolve/master/a.gguf", parsed) ||
        parsed.host != "modelscope.cn" || !parsed.secure || parsed.port != 443 ||
        parsed.path != "/models/x/resolve/master/a.gguf") {
        std::printf("FAIL https URL 解析不符\n");
        ++failed;
    }
    if (!ParseModelUrl("http://127.0.0.1:8080/p?x=1", parsed) ||
        parsed.host != "127.0.0.1" || parsed.secure || parsed.port != 8080 ||
        parsed.path != "/p?x=1") {
        std::printf("FAIL 带端口 http URL 解析不符\n");
        ++failed;
    }
    if (ParseModelUrl("ftp://host/path", parsed) || ParseModelUrl("", parsed) ||
        ParseModelUrl("http://", parsed) || ParseModelUrl("not-a-url", parsed)) {
        std::printf("FAIL 非法 URL 应被拒\n");
        ++failed;
    }

    // PlanResume。
    if (PlanResume(0, 100) != ResumePlan::kFreshStart ||
        PlanResume(1, 100) != ResumePlan::kResume ||
        PlanResume(99, 100) != ResumePlan::kResume ||
        PlanResume(100, 100) != ResumePlan::kCorruptRestart ||
        PlanResume(101, 100) != ResumePlan::kCorruptRestart) {
        std::printf("FAIL PlanResume 边界不符\n");
        ++failed;
    }

    // InterpretRangeResponse。
    const auto verdict = [](RangeResponseInfo info) {
        return InterpretRangeResponse(info);
    };
    if (verdict({.status = 200, .has_content_length = true, .content_length = 100,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kOverwritePart) {
        std::printf("FAIL 200 全量应 OverwritePart\n");
        ++failed;
    }
    if (verdict({.status = 200, .has_content_length = true, .content_length = 99,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kInvalid) {
        std::printf("FAIL 200 且 CL 不符应 Invalid\n");
        ++failed;
    }
    if (verdict({.status = 200, .has_content_length = false,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kOverwritePart) {
        std::printf("FAIL 200 无 CL 应 OverwritePart（哈希兜底）\n");
        ++failed;
    }
    if (verdict({.status = 200, .has_content_length = false,
                 .range_base = 50, .expected_bytes = 100}) != RangeVerdict::kOverwritePart) {
        std::printf("FAIL 服务器无视 Range 应回 200 重下\n");
        ++failed;
    }
    if (verdict({.status = 206, .has_content_length = true, .content_length = 50,
                 .range_base = 50, .expected_bytes = 100}) != RangeVerdict::kAppendToPart) {
        std::printf("FAIL 206 自洽应 AppendToPart\n");
        ++failed;
    }
    if (verdict({.status = 206, .has_content_length = true, .content_length = 51,
                 .range_base = 50, .expected_bytes = 100}) != RangeVerdict::kInvalid) {
        std::printf("FAIL 206 CL 与基数不符应 Invalid\n");
        ++failed;
    }
    if (verdict({.status = 206, .has_content_length = true, .content_length = 100,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kInvalid) {
        std::printf("FAIL 未带 Range 却回 206 应 Invalid\n");
        ++failed;
    }
    if (verdict({.status = 206, .has_content_length = false,
                 .range_base = 50, .expected_bytes = 100}) != RangeVerdict::kAppendToPart) {
        std::printf("FAIL 206 无 CL 应 AppendToPart（信任连接边界）\n");
        ++failed;
    }
    if (verdict({.status = 404, .has_content_length = false,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kInvalid) {
        std::printf("FAIL 非 200/206 应 Invalid（防御）\n");
        ++failed;
    }

    // RequiredDiskBytes。
    if (RequiredDiskBytes({}) != 0) {
        std::printf("FAIL 空清单磁盘需求应为 0\n");
        ++failed;
    }
    {
        std::vector<ModelFileSpec> files;
        ModelFileSpec a;
        a.bytes = 100;
        ModelFileSpec b;
        b.bytes = 23;
        files = {a, b};
        if (RequiredDiskBytes(files) != 123) {
            std::printf("FAIL 磁盘需求应求和\n");
            ++failed;
        }
    }

    AbortIfFailed(failed, "TestModelDownloaderPureFunctions");
}

void TestFinalizePartFile() {
    int failed = 0;
    const auto dir = MakeTempDir("finalize");
    const auto dest = dir / "model.bin";
    const auto part = dir / "model.bin.part";

    // 正常收尾：哈希匹配 → 原子改名。
    WriteFileBytes(part, "hello world");
    if (FinalizePartFile(dest, TestSha256Hex("hello world")) != DownloadResult::kOk ||
        ReadFileBytes(dest) != "hello world" || std::filesystem::exists(part)) {
        std::printf("FAIL 哈希匹配应改名成功且 .part 消失\n");
        ++failed;
    }

    // 哈希不匹配：删除 .part，dest 保持原样。
    WriteFileBytes(part, "tampered");
    if (FinalizePartFile(dest, TestSha256Hex("hello world")) != DownloadResult::kHashMismatch ||
        std::filesystem::exists(part) || ReadFileBytes(dest) != "hello world") {
        std::printf("FAIL 哈希不匹配应删 .part 且不动 dest\n");
        ++failed;
    }

    // .part 不存在（调用方违约）：按不匹配处理。
    if (FinalizePartFile(dest, TestSha256Hex("hello world")) != DownloadResult::kHashMismatch) {
        std::printf("FAIL 缺 .part 应按不匹配处理\n");
        ++failed;
    }

    // 大写期望哈希：实现侧归一化小写后比较。
    WriteFileBytes(part, "hello world");
    std::string upper = TestSha256Hex("hello world");
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const auto second = dir / "second.bin";
    const auto second_part = dir / "second.bin.part";
    WriteFileBytes(second_part, "hello world");
    if (FinalizePartFile(second, upper) != DownloadResult::kOk ||
        ReadFileBytes(second) != "hello world") {
        std::printf("FAIL 大写期望哈希应归一化比较\n");
        ++failed;
    }

    AbortIfFailed(failed, "TestFinalizePartFile");
}

void TestModelDownloaderLoopback() {
    int failed = 0;
    const std::string kBody = "hello world";

    LoopbackHttpServer::Rules rules;
    rules["/ok"] = {.status = 200, .body = kBody};
    rules["/bad"] = {.status = 200, .body = "hello worlx"};  // 同长度坏内容
    rules["/norange"] = {.status = 200, .body = kBody, .honor_range = false};
    rules["/slow"] = {.status = 200, .body = std::string(8192, 'x'),
                      .chunk_size = 256, .chunk_delay_ms = 50};
    LoopbackHttpServer server(std::move(rules));
    ModelDownloader downloader;

    // 用例 1：全量下载成功，progress 收尾必达 total。
    {
        const auto dir = MakeTempDir("full");
        const auto dest = dir / "file.bin";
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/ok")});
        DownloadProgress last;
        int calls = 0;
        auto outcome = downloader.DownloadFile(
            spec, dest, [&](const DownloadProgress& p) { last = p; ++calls; });
        if (outcome.result != DownloadResult::kOk || !outcome.error.empty() ||
            ReadFileBytes(dest) != kBody ||
            last.downloaded != kBody.size() || last.total != kBody.size() || calls < 1) {
            std::printf("FAIL 全量下载: result=%d url_used=%s\n",
                        static_cast<int>(outcome.result), outcome.url_used.c_str());
            ++failed;
        }
    }

    // 用例 2：断点续传——预置半截 .part，服务器须收到 "bytes=5-"。
    {
        const auto dir = MakeTempDir("resume");
        const auto dest = dir / "file.bin";
        const auto part = dir / "file.bin.part";
        WriteFileBytes(part, kBody.substr(0, 5));
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/ok")});
        auto outcome = downloader.DownloadFile(spec, dest);
        const auto ranges = server.TakeRangeHeaders();
        bool saw_range = false;
        for (const auto& range : ranges) {
            if (range.find("bytes=5-") != std::string::npos) saw_range = true;
        }
        if (outcome.result != DownloadResult::kOk || ReadFileBytes(dest) != kBody ||
            !saw_range) {
            std::printf("FAIL 续传: result=%d range_seen=%d\n",
                        static_cast<int>(outcome.result), saw_range ? 1 : 0);
            ++failed;
        }
    }

    // 用例 3：服务器无视 Range 回 200 全量 → 重下成功（OverwritePart 路径）。
    {
        const auto dir = MakeTempDir("norange");
        const auto dest = dir / "file.bin";
        const auto part = dir / "file.bin.part";
        WriteFileBytes(part, kBody.substr(0, 5));
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/norange")});
        auto outcome = downloader.DownloadFile(spec, dest);
        if (outcome.result != DownloadResult::kOk || ReadFileBytes(dest) != kBody) {
            std::printf("FAIL 服务器无视 Range: result=%d\n",
                        static_cast<int>(outcome.result));
            ++failed;
        }
        server.TakeRangeHeaders();  // 清空本用例记录
    }

    // 用例 4：首源哈希不匹配 → 回退次源成功。
    {
        const auto dir = MakeTempDir("fallback");
        const auto dest = dir / "file.bin";
        const auto spec =
            MakeSpecFromBody(kBody, {server.Url("/bad"), server.Url("/ok")});
        auto outcome = downloader.DownloadFile(spec, dest);
        if (outcome.result != DownloadResult::kOk ||
            outcome.url_used != server.Url("/ok") || ReadFileBytes(dest) != kBody) {
            std::printf("FAIL 哈希不匹配换源: result=%d url=%s\n",
                        static_cast<int>(outcome.result), outcome.url_used.c_str());
            ++failed;
        }
    }

    // 用例 5：单源坏内容 → kHashMismatch，.part 已删、dest 不存在。
    {
        const auto dir = MakeTempDir("mismatch");
        const auto dest = dir / "file.bin";
        const auto part = dir / "file.bin.part";
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/bad")});
        auto outcome = downloader.DownloadFile(spec, dest);
        if (outcome.result != DownloadResult::kHashMismatch ||
            std::filesystem::exists(part) || std::filesystem::exists(dest)) {
            std::printf("FAIL 单源哈希不匹配: result=%d\n",
                        static_cast<int>(outcome.result));
            ++failed;
        }
    }

    // 用例 6：404 → 换源成功；全 404 → kNetworkError 且 error 汇总含状态码。
    {
        const auto dir = MakeTempDir("http404");
        const auto dest = dir / "file.bin";
        const auto spec =
            MakeSpecFromBody(kBody, {server.Url("/missing"), server.Url("/ok")});
        auto outcome = downloader.DownloadFile(spec, dest);
        if (outcome.result != DownloadResult::kOk) {
            std::printf("FAIL 404 换源: result=%d\n", static_cast<int>(outcome.result));
            ++failed;
        }
        const auto spec_all_missing = MakeSpecFromBody(kBody, {server.Url("/missing")});
        auto outcome2 = downloader.DownloadFile(spec_all_missing, dest);
        if (outcome2.result != DownloadResult::kNetworkError ||
            outcome2.error.find("404") == std::string::npos) {
            std::printf("FAIL 全 404: result=%d error=%s\n",
                        static_cast<int>(outcome2.result), outcome2.error.c_str());
            ++failed;
        }
    }

    // 用例 7：取消——慢速分块下载，收到首批进度后置位，.part 保留。
    {
        const auto dir = MakeTempDir("cancel");
        const auto dest = dir / "file.bin";
        const auto part = dir / "file.bin.part";
        const auto spec = MakeSpecFromBody(std::string(8192, 'x'), {server.Url("/slow")});
        std::atomic<bool> cancel_flag{false};
        auto outcome = downloader.DownloadFile(
            spec, dest,
            [&](const DownloadProgress& p) {
                if (p.downloaded > 0) cancel_flag.store(true);
            },
            [&] { return cancel_flag.load(); });
        const bool part_kept = std::filesystem::exists(part) &&
                               std::filesystem::file_size(part) < 8192;
        if (outcome.result != DownloadResult::kCancelled || !part_kept ||
            std::filesystem::exists(dest)) {
            std::printf("FAIL 取消: result=%d part_kept=%d\n",
                        static_cast<int>(outcome.result), part_kept ? 1 : 0);
            ++failed;
        }
    }

    // 用例 8：无效清单（urls 空）→ kInvalidSpec。
    {
        const auto dir = MakeTempDir("invalid");
        auto outcome = downloader.DownloadFile(MakeSpecFromBody(kBody, {}),
                                               dir / "file.bin");
        if (outcome.result != DownloadResult::kInvalidSpec) {
            std::printf("FAIL 空 urls 应 kInvalidSpec: result=%d\n",
                        static_cast<int>(outcome.result));
            ++failed;
        }
    }

    // 用例 9：dest 父目录不存在 → kLocalIoError（本地 I/O，与网络无关）。
    {
        const auto dir = MakeTempDir("baddir");
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/ok")});
        auto outcome = downloader.DownloadFile(spec, dir / "no-such-dir" / "file.bin");
        if (outcome.result != DownloadResult::kLocalIoError) {
            std::printf("FAIL 父目录缺失应 kLocalIoError: result=%d\n",
                        static_cast<int>(outcome.result));
            ++failed;
        }
    }

    AbortIfFailed(failed, "TestModelDownloaderLoopback");
}

void TestModelDownloadSession() {
    int failed = 0;
    const std::string kAsr1(4096, 'a');
    const std::string kAsr2(2048, 'b');
    const std::string kRefine(8192, 'c');

    LoopbackHttpServer::Rules rules;
    rules["/asr1"] = {.status = 200, .body = kAsr1};
    rules["/asr2"] = {.status = 200, .body = kAsr2};
    rules["/refine"] = {.status = 200, .body = kRefine};
    rules["/slow"] = {.status = 200, .body = std::string(8192, 's'),
                      .chunk_size = 256, .chunk_delay_ms = 50};
    LoopbackHttpServer server(std::move(rules));
    ModelDownloader downloader;

    const auto make_item = [&](ModelKind kind, const std::string& url,
                               const std::string& body,
                               const std::filesystem::path& dest) {
        ModelDownloadItem item;
        item.kind = kind;
        item.selected = true;
        item.spec = MakeSpecFromBody(body, {url});
        item.dest = dest;
        return item;
    };

    // 用例 1：全成功——进度聚合终值、三个文件落位、summary 全绿。
    {
        const auto dir = MakeTempDir("session_ok");
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/asr1"), kAsr1, dir / "model.int8.onnx"),
            make_item(ModelKind::kAsr, server.Url("/asr2"), kAsr2, dir / "tokens.txt"),
            make_item(ModelKind::kRefine, server.Url("/refine"), kRefine,
                      dir / "Qwen3-1.7B-Q4_K_M" / "Qwen3-1.7B-Q4_K_M.gguf"),
        };
        ModelSessionProgress last;
        ModelDownloadSession session(std::move(items), &downloader,
                                     [&](const ModelSessionProgress& p) { last = p; });
        const auto summary = session.Run();
        const std::uint64_t total = kAsr1.size() + kAsr2.size() + kRefine.size();
        if (!summary.asr_ok || !summary.refine_ok || summary.refine_skipped ||
            summary.cancelled || !summary.errors.empty()) {
            std::printf("FAIL 全成功 summary: asr=%d refine=%d skip=%d cancel=%d errs=%zu\n",
                        summary.asr_ok ? 1 : 0, summary.refine_ok ? 1 : 0,
                        summary.refine_skipped ? 1 : 0, summary.cancelled ? 1 : 0,
                        summary.errors.size());
            ++failed;
        }
        if (last.total != total || last.downloaded != total) {
            std::printf("FAIL 进度聚合终值: %llu/%llu（期望 %llu）\n",
                        static_cast<unsigned long long>(last.downloaded),
                        static_cast<unsigned long long>(last.total),
                        static_cast<unsigned long long>(total));
            ++failed;
        }
        if (ReadFileBytes(dir / "model.int8.onnx") != kAsr1 ||
            ReadFileBytes(dir / "tokens.txt") != kAsr2 ||
            ReadFileBytes(dir / "Qwen3-1.7B-Q4_K_M" / "Qwen3-1.7B-Q4_K_M.gguf") != kRefine) {
            std::printf("FAIL 三个目标文件内容不符\n");
            ++failed;
        }
    }

    // 用例 2：精修源全 404——ASR 仍成功，精修失败记录一条，不阻塞收尾。
    {
        const auto dir = MakeTempDir("session_refine_fail");
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/asr1"), kAsr1, dir / "model.int8.onnx"),
            make_item(ModelKind::kAsr, server.Url("/asr2"), kAsr2, dir / "tokens.txt"),
            make_item(ModelKind::kRefine, server.Url("/missing"), kRefine, dir / "refine.gguf"),
        };
        ModelDownloadSession session(std::move(items), &downloader);
        const auto summary = session.Run();
        if (!summary.asr_ok || summary.refine_ok || summary.refine_skipped ||
            summary.cancelled || summary.errors.size() != 1) {
            std::printf("FAIL 精修失败语义: asr=%d refine=%d errs=%zu\n",
                        summary.asr_ok ? 1 : 0, summary.refine_ok ? 1 : 0,
                        summary.errors.size());
            ++failed;
        }
    }

    // 用例 3：ASR 首文件 404——整体失败，后续条目（含精修）不再发起。
    {
        const auto dir = MakeTempDir("session_asr_fail");
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/missing"), kAsr1, dir / "model.int8.onnx"),
            make_item(ModelKind::kAsr, server.Url("/asr2"), kAsr2, dir / "tokens.txt"),
            make_item(ModelKind::kRefine, server.Url("/refine"), kRefine, dir / "refine.gguf"),
        };
        ModelDownloadSession session(std::move(items), &downloader);
        const auto summary = session.Run();
        if (summary.asr_ok || summary.refine_ok || summary.cancelled ||
            std::filesystem::exists(dir / "tokens.txt") ||
            std::filesystem::exists(dir / "refine.gguf")) {
            std::printf("FAIL ASR 失败应中断: asr=%d 后续文件不应存在\n",
                        summary.asr_ok ? 1 : 0);
            ++failed;
        }
    }

    // 用例 4：取消——慢速源中途置位，cancelled 且 .part 保留。
    {
        const auto dir = MakeTempDir("session_cancel");
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/slow"), std::string(8192, 's'),
                      dir / "model.int8.onnx"),
        };
        auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
        ModelDownloadSession session(std::move(items), &downloader,
                                     [&](const ModelSessionProgress& p) {
                                         if (p.downloaded > 0) cancel_flag->store(true);
                                     },
                                     cancel_flag);
        const auto summary = session.Run();
        const bool part_kept = std::filesystem::exists(dir / "model.int8.onnx.part") &&
                               std::filesystem::file_size(dir / "model.int8.onnx.part") < 8192;
        if (!summary.cancelled || summary.asr_ok || !part_kept) {
            std::printf("FAIL 取消语义: cancel=%d asr=%d part_kept=%d\n",
                        summary.cancelled ? 1 : 0, summary.asr_ok ? 1 : 0, part_kept ? 1 : 0);
            ++failed;
        }
    }

    // 用例 5：精修未勾选——不发起该条目，refine_skipped 且无错误。
    {
        const auto dir = MakeTempDir("session_skip_refine");
        auto refine_item = make_item(ModelKind::kRefine, server.Url("/refine"), kRefine,
                                     dir / "refine.gguf");
        refine_item.selected = false;
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/asr1"), kAsr1, dir / "model.int8.onnx"),
            make_item(ModelKind::kAsr, server.Url("/asr2"), kAsr2, dir / "tokens.txt"),
            std::move(refine_item),
        };
        ModelDownloadSession session(std::move(items), &downloader);
        const auto summary = session.Run();
        if (!summary.asr_ok || summary.refine_ok || !summary.refine_skipped ||
            !summary.errors.empty() || std::filesystem::exists(dir / "refine.gguf")) {
            std::printf("FAIL 跳过精修语义: asr=%d skip=%d errs=%zu\n",
                        summary.asr_ok ? 1 : 0, summary.refine_skipped ? 1 : 0,
                        summary.errors.size());
            ++failed;
        }
    }

    // 用例 6：BuildModelDownloadItems——条目数与目标路径（含 GGUF 子目录）。
    {
        const auto models_dir = std::filesystem::path("C:/cache/models");
        const auto with_refine = BuildModelDownloadItems(models_dir, true);
        if (with_refine.size() != 3 ||
            with_refine[0].dest != models_dir / "model.int8.onnx" ||
            with_refine[1].dest != models_dir / "tokens.txt" ||
            with_refine[2].dest != models_dir / "Qwen3-1.7B-Q4_K_M" / "Qwen3-1.7B-Q4_K_M.gguf" ||
            with_refine[0].kind != ModelKind::kAsr ||
            with_refine[2].kind != ModelKind::kRefine || !with_refine[2].selected) {
            std::printf("FAIL BuildModelDownloadItems(含精修) 条目不符：%zu 条\n",
                        with_refine.size());
            ++failed;
        }
        const auto without_refine = BuildModelDownloadItems(models_dir, false);
        if (without_refine.size() != 2 ||
            without_refine[0].spec.rel_path != "model.int8.onnx" ||
            without_refine[1].spec.bytes != 315894) {
            std::printf("FAIL BuildModelDownloadItems(不含精修) 条目不符：%zu 条\n",
                        without_refine.size());
            ++failed;
        }
        // spec 与内置清单同源（哈希/URL 不漂移）。
        const auto& bundled = BundledModelEntries();
        if (with_refine.size() == 3 &&
            (with_refine[0].spec.sha256 != bundled[0].files[0].sha256 ||
             with_refine[0].spec.urls != bundled[0].files[0].urls)) {
            std::printf("FAIL 条目 spec 应与内置清单一致\n");
            ++failed;
        }
    }

    // 用例 7：LocalModelCacheModelsDir——非空且尾部三段目录名固定。
    {
        const auto dir = LocalModelCacheModelsDir();
        if (dir.empty() || dir.filename().wstring() != L"sense-voice-int8-2024-07-17" ||
            dir.parent_path().filename().wstring() != L"models" ||
            dir.parent_path().parent_path().filename().wstring() != L"VoiceStick") {
            std::printf("FAIL LocalModelCacheModelsDir 尾部结构不符\n");
            ++failed;
        }
    }

    AbortIfFailed(failed, "TestModelDownloadSession");
}

// B8：UpdateConfig 原子换入与后台读取并发（快照语义压力冒烟）——4 读线程持续走
// 纯配置读路径（WechatSessionUsesDefaultMicDirectly：快照 + firmware_mutex_），
// 主线程 300 次整份换入；无撕裂读、无死锁、读线程全程存活。
void TestCoordinatorConcurrentUpdateConfigStress() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto cloud_asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble),
                                      std::move(cloud_asr), &ui, &input);
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                (void)coordinator.WechatSessionUsesDefaultMicDirectly("RC-0001");
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int i = 0; i < 300; ++i) {
        AppConfig next = AppConfig::Defaults();
        next.ui_language = (i % 2 == 0) ? UiLanguage::kSimplifiedChinese
                                       : UiLanguage::kEnglish;
        coordinator.UpdateConfig(std::move(next));
    }
    stop.store(true);
    for (auto& thread : readers) {
        thread.join();
    }
    int failed = reads.load() > 0 ? 0 : 1;
    if (failed != 0) {
        std::printf("FAIL contract-ish: 读线程零读取（未真正压到快照路径）\n");
        fflush(stdout);
    }
    AbortIfFailed(failed, "TestCoordinatorConcurrentUpdateConfigStress");
}

// B9：任何保存路径都不抹磁盘 [license]（陈旧/空内存副本），且原子写成功不留 .tmp 残迹。
void TestSaveStaleCopyKeepsLicense() {
    namespace fs = std::filesystem;
    int failed = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            ++failed;
            std::printf("FAIL contract-ish: %s\n", what);
            fflush(stdout);
        }
    };
    const auto dir = fs::temp_directory_path() / "vs_config_b9";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const auto path = dir / "config.toml";

    // 1) 建立带 license 的磁盘配置。
    AppConfig on_disk = AppConfig::Defaults();
    on_disk.license.serial = "VS-B9-SERIAL";
    on_disk.license.trial_anchor_days = 100;
    on_disk.license.last_seen_days = 105;
    on_disk.volcengine_api_key = "disk-key";
    on_disk.Save(path);

    // 2) 陈旧副本（license 为空 = 运行期快照）走两条合并保存路径，[license] 必须幸存。
    // plain Save 写**本对象** license（LicenseRuntime 激活/锚点落盘依赖该语义，见
    // TestLicenseConfigRoundTrip 往返），陈旧持有者约定不走 plain Save——B9 定案：
    // 谁的副本谁重取，合并路径自带磁盘 license 保留。
    AppConfig stale = AppConfig::Defaults();
    stale.SavePreservingDiskCredentials(path);
    AppConfig loaded = AppConfig::Load(path);
    expect(loaded.license.serial == "VS-B9-SERIAL",
           "SavePreservingDiskCredentials 后 license.serial 被抹");
    expect(loaded.license.trial_anchor_days.has_value() &&
               *loaded.license.trial_anchor_days == 100,
           "SavePreservingDiskCredentials 后 trial_anchor_days 被抹");
    expect(loaded.license.last_seen_days.has_value() &&
               *loaded.license.last_seen_days == 105,
           "SavePreservingDiskCredentials 后 last_seen_days 被抹");
    expect(loaded.volcengine_api_key == "disk-key",
           "SavePreservingDiskCredentials 凭据保留语义回归");

    stale.SaveSettingsDialog(path);
    loaded = AppConfig::Load(path);
    expect(loaded.license.serial == "VS-B9-SERIAL",
           "SaveSettingsDialog 后 license.serial 被抹");

    // 3) 原子写：成功路径不留 .tmp 残迹，文件完整可回读。
    expect(!fs::exists(fs::path(path.string() + ".tmp")), "成功保存后残留 .tmp");
    expect(loaded.asr_provider == AppConfig::Defaults().asr_provider, "回读内容完整");

    fs::remove_all(dir, ec);
    AbortIfFailed(failed, "TestSaveStaleCopyKeepsLicense");
}

// ===== 跨端契约 fixtures（tests/contract，规格 Doc/Ref/protocol.md）=====
// 黄金字节由 tests/contract/generate_fixtures.py 独立构造（不从实现反推）；本测试
// 用 Windows 解析器/构建器对拍期望。键序不构成契约，control 组比对对象语义；
// expect 只取两端公共字段（单端缺口清单见 tests/contract/README.md）。

static std::vector<std::uint8_t> ContractUnhex(const std::string& hex) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size();) {
        if (std::isspace(static_cast<unsigned char>(hex[i]))) { ++i; continue; }
        if (i + 1 >= hex.size()) return {};
        const int hi = nib(hex[i]);
        const int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        i += 2;
    }
    return out;
}

static std::string ContractHexStr(const std::vector<std::uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

static std::string ContractJsonStr(const cJSON* item) {
    return (item && item->valuestring) ? item->valuestring : std::string();
}

static std::uint32_t ContractJsonU32(const cJSON* item) {
    return item ? static_cast<std::uint32_t>(item->valuedouble) : 0;
}

// control 构建器分发（args → 本端 payload）。两端缺一边的构建器不入公共样本。
static std::optional<ByteVector> ContractBuildControl(const std::string& kind,
                                                      const cJSON* args) {
    auto s = [&](const char* k) {
        return ContractJsonStr(cJSON_GetObjectItemCaseSensitive(args, k));
    };
    if (kind == "ui_state") {
        return BleProtocol::UiStatePayload(s("state"), s("text"));
    }
    if (kind == "interaction_mode") {
        return BleProtocol::InteractionModePayload(s("mode"));
    }
    if (kind == "show_imu_debug") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::ShowImuDebugPayload(cJSON_IsTrue(v));
    }
    if (kind == "imu_wake_sensitivity") {
        return BleProtocol::ImuWakeSensitivityPayload(
            cJSON_GetObjectItemCaseSensitive(args, "threshold")->valueint);
    }
    if (kind == "tap_enabled") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::TapEnabledPayload(cJSON_IsTrue(v));
    }
    if (kind == "tap_sensitivity") {
        return BleProtocol::TapSensitivityPayload(
            cJSON_GetObjectItemCaseSensitive(args, "level")->valueint);
    }
    if (kind == "encoder_led_color") {
        return BleProtocol::EncoderLedColorPayload(s("color"));
    }
    if (kind == "encoder_recording_gate") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::EncoderRecordingGatePayload(cJSON_IsTrue(v));
    }
    if (kind == "gateway_keymap_set") {
        return BleProtocol::GatewayKeymapSetPayload(s("key"), s("route") == "software");
    }
    if (kind == "gateway_target_info") {
        return BleProtocol::GatewayTargetInfoPayload(s("name"));
    }
    if (kind == "air_mouse_enabled") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::AirMouseEnabledPayload(cJSON_IsTrue(v));
    }
    if (kind == "usb_auto_off") {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(args, "enabled");
        return BleProtocol::UsbAutoOffPayload(cJSON_IsTrue(v));
    }
    if (kind == "battery_status_request") {
        return BleProtocol::BatteryStatusRequestPayload();
    }
    if (kind == "remote_button") {
        return BleProtocol::RemoteButtonPayload(
            s("action"), s("button"), s("source"),
            ContractJsonU32(cJSON_GetObjectItemCaseSensitive(args, "request_id")));
    }
    if (kind == "power_log_dump") {
        return BleProtocol::PowerLogDumpPayload(
            ContractJsonU32(cJSON_GetObjectItemCaseSensitive(args, "offset")),
            ContractJsonU32(cJSON_GetObjectItemCaseSensitive(args, "max")));
    }
    if (kind == "power_log_clear") {
        return BleProtocol::PowerLogClearPayload();
    }
    return std::nullopt;
}

static std::optional<ByteVector> ContractBuildOtaControl(const std::string& kind,
                                                         const cJSON* args) {
    auto u32 = [&](const char* k) {
        return ContractJsonU32(cJSON_GetObjectItemCaseSensitive(args, k));
    };
    if (kind == "ota_begin") {
        return BleProtocol::OtaBeginPayload(u32("image_size"), u32("transfer_id"));
    }
    if (kind == "ota_data") {
        const auto chunk = ContractUnhex(ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(args, "chunk_hex")));
        if (chunk.empty()) return std::nullopt;
        return BleProtocol::OtaDataPayload(u32("transfer_id"), u32("offset"), chunk);
    }
    if (kind == "ota_end") {
        return BleProtocol::OtaEndPayload(u32("transfer_id"), u32("image_size"));
    }
    if (kind == "ota_abort") {
        return BleProtocol::OtaAbortPayload(u32("transfer_id"));
    }
    return std::nullopt;
}

void TestContractFixtures() {
    int failed = 0;
    auto fail = [&](const std::string& msg) {
        ++failed;
        std::printf("FAIL contract: %s\n", msg.c_str());
        fflush(stdout);
    };
#ifdef VOICESTICK_REPO_ROOT
    const std::string path = (std::filesystem::path(VOICESTICK_REPO_ROOT) /
                              "tests/contract/fixtures/manifest.json").string();
#else
    const std::string path = "tests/contract/fixtures/manifest.json";
#endif
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail("manifest 打不开: " + path);
        AbortIfFailed(failed, "TestContractFixtures");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    cJSON* manifest = cJSON_Parse(text.c_str());
    if (manifest == nullptr) {
        fail("manifest JSON 解析失败: " + path);
        AbortIfFailed(failed, "TestContractFixtures");
        return;
    }
    auto section = [&](const char* key) -> cJSON* {
        return cJSON_GetObjectItemCaseSensitive(manifest, key);
    };

    // 1) state 事件：黄金字节 → ParseStateEvent → 公共字段期望（线上字段名）。
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, section("state_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const auto bytes = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const auto event = BleProtocol::ParseStateEvent(bytes);
        if (!event.has_value()) {
            fail(name + ": ParseStateEvent 返回空");
            continue;
        }
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        const cJSON* field = nullptr;
        cJSON_ArrayForEach(field, expect) {
            const std::string key = field->string ? field->string : "";
            bool known = true;
            bool ok = false;
            if (key == "event") {
                ok = event->event == ContractJsonStr(field);
            } else if (key == "button") {
                ok = event->button == ContractJsonStr(field);
            } else if (key == "hardware") {
                ok = event->hardware == ContractJsonStr(field);
            } else if (key == "firmware_version") {
                ok = event->firmware_version == ContractJsonStr(field);
            } else if (key == "direction") {
                ok = event->direction == ContractJsonStr(field);
            } else if (key == "source") {
                ok = event->source == ContractJsonStr(field);
            } else if (key == "key") {
                ok = event->gateway_key == ContractJsonStr(field);
            } else if (key == "session_id") {
                ok = event->session_id.has_value() &&
                     *event->session_id == ContractJsonU32(field);
            } else if (key == "duration_ms") {
                ok = event->duration_ms.has_value() &&
                     *event->duration_ms == ContractJsonU32(field);
            } else if (key == "steps") {
                ok = event->steps.has_value() && *event->steps == ContractJsonU32(field);
            } else if (key == "level") {
                ok = event->battery_level.has_value() &&
                     *event->battery_level == field->valueint;
            } else if (key == "present") {
                ok = event->encoder_present.has_value() &&
                     *event->encoder_present == cJSON_IsTrue(field);
            } else if (key == "charging") {
                ok = event->battery_charging.has_value() &&
                     *event->battery_charging == cJSON_IsTrue(field);
            } else if (key == "usb_powered") {
                ok = event->battery_usb_powered.has_value() &&
                     *event->battery_usb_powered == cJSON_IsTrue(field);
            } else if (key == "pressed") {
                ok = event->gateway_pressed.has_value() &&
                     *event->gateway_pressed == cJSON_IsTrue(field);
            } else if (key == "mode") {
                // gateway_status.mode 线上值 "gateway"/"normal" → 本端 bool。
                ok = event->gateway_mode.has_value() &&
                     *event->gateway_mode == (ContractJsonStr(field) == "gateway");
            } else {
                known = false;
            }
            if (!known) {
                fail(name + ": 未映射的 expect 键 " + key);
            } else if (!ok) {
                fail(name + ": 字段 " + key + " 不符");
            }
        }
    }

    // 2) power_mgmt：独立解析器（ParseStateEvent 对其返回空）。
    cJSON_ArrayForEach(item, section("power_mgmt_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const auto bytes = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const auto pm = BleProtocol::ParsePowerMgmtEvent(bytes);
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        const cJSON* want = cJSON_GetObjectItemCaseSensitive(expect, "usb_auto_off");
        if (!pm.has_value() || want == nullptr) {
            fail(name + ": ParsePowerMgmtEvent/expect 失败");
            continue;
        }
        if (*pm != cJSON_IsTrue(want)) {
            fail(name + ": usb_auto_off 不符");
        }
        // ParseStateEvent 必须跳过 power_mgmt（分发链契约）。
        if (BleProtocol::ParseStateEvent(bytes).has_value()) {
            fail(name + ": ParseStateEvent 应跳过 power_mgmt");
        }
    }

    // 3) OTA state 五态。
    cJSON_ArrayForEach(item, section("ota_state_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const auto bytes = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const auto ota = BleProtocol::ParseFirmwareOtaStateEvent(bytes);
        if (!ota.has_value()) {
            fail(name + ": ParseFirmwareOtaStateEvent 返回空");
            continue;
        }
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        const cJSON* field = nullptr;
        cJSON_ArrayForEach(field, expect) {
            const std::string key = field->string ? field->string : "";
            bool ok = false;
            if (key == "event") {
                ok = ota->event == ContractJsonStr(field);
            } else if (key == "code") {
                ok = ota->code == ContractJsonStr(field);
            } else if (key == "transfer_id") {
                ok = ota->transfer_id.has_value() &&
                     *ota->transfer_id == ContractJsonU32(field);
            } else if (key == "written") {
                ok = ota->written.has_value() && *ota->written == ContractJsonU32(field);
            } else if (key == "size") {
                ok = ota->size.has_value() && *ota->size == ContractJsonU32(field);
            } else if (key == "esp_err") {
                ok = ota->esp_err.has_value() && *ota->esp_err == ContractJsonU32(field);
            } else if (key == "reboot_ms") {
                ok = ota->reboot_ms.has_value() && *ota->reboot_ms == ContractJsonU32(field);
            } else {
                fail(name + ": 未映射的 expect 键 " + key);
                continue;
            }
            if (!ok) fail(name + ": 字段 " + key + " 不符");
        }
    }

    // 4) 二进制 audio / motion。
    cJSON_ArrayForEach(item, section("binary_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const std::string kind = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "kind"));
        const auto bytes = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        if (kind == "audio") {
            const auto audio = BleProtocol::ParseAudioFrame(bytes);
            if (!audio.has_value()) {
                fail(name + ": ParseAudioFrame 返回空");
                continue;
            }
            const cJSON* want_session =
                cJSON_GetObjectItemCaseSensitive(expect, "session_id");
            const cJSON* want_seq = cJSON_GetObjectItemCaseSensitive(expect, "seq");
            const cJSON* want_flags = cJSON_GetObjectItemCaseSensitive(expect, "flags");
            const cJSON* want_payload =
                cJSON_GetObjectItemCaseSensitive(expect, "payload_hex");
            if (want_session && audio->session_id != ContractJsonU32(want_session)) {
                fail(name + ": session_id 不符");
            }
            if (want_seq && audio->seq != ContractJsonU32(want_seq)) {
                fail(name + ": seq 不符");
            }
            if (want_flags &&
                audio->flags != static_cast<std::uint8_t>(want_flags->valueint)) {
                fail(name + ": flags 不符");
            }
            if (want_payload &&
                ContractHexStr(audio->payload) != ContractJsonStr(want_payload)) {
                fail(name + ": payload 不符 got=" + ContractHexStr(audio->payload));
            }
            // 帧头 flags 语义位（spec：bit0=start bit1=end）。
            const cJSON* want_u32 = want_flags;
            if (want_u32 && (ContractJsonU32(want_u32) & 0x01) && !audio->IsStart()) {
                fail(name + ": IsStart() 应为真");
            }
            if (want_u32 && (ContractJsonU32(want_u32) & 0x02) && !audio->IsEnd()) {
                fail(name + ": IsEnd() 应为真");
            }
        } else if (kind == "motion") {
            const auto motion = BleProtocol::ParseMotionFrame(bytes);
            if (!motion.has_value()) {
                fail(name + ": ParseMotionFrame 返回空");
                continue;
            }
            const cJSON* want_dx = cJSON_GetObjectItemCaseSensitive(expect, "dx");
            const cJSON* want_dy = cJSON_GetObjectItemCaseSensitive(expect, "dy");
            if (want_dx &&
                motion->dx != static_cast<std::int16_t>(want_dx->valueint)) {
                fail(name + ": dx 不符");
            }
            if (want_dy &&
                motion->dy != static_cast<std::int16_t>(want_dy->valueint)) {
                fail(name + ": dy 不符");
            }
        } else {
            fail(name + ": 未知 binary kind " + kind);
        }
    }

    // 5) control 构建 → 对象语义比对（键序无关，cJSON_Compare 大小写敏感）。
    cJSON_ArrayForEach(item, section("control_payloads")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const std::string kind = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "kind"));
        const cJSON* args = cJSON_GetObjectItemCaseSensitive(item, "args");
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        const auto built = ContractBuildControl(kind, args);
        if (!built.has_value()) {
            fail(name + ": 构建器缺失/构建失败 (kind=" + kind + ")");
            continue;
        }
        const std::string built_text(built->begin(), built->end());
        cJSON* actual = cJSON_Parse(built_text.c_str());
        if (actual == nullptr) {
            fail(name + ": 构建输出不是合法 JSON: " + built_text);
            continue;
        }
        if (!cJSON_Compare(actual, expect, /*case_sensitive=*/TRUE)) {
            fail(name + ": 构建输出与期望不符 got=" + built_text);
        }
        cJSON_Delete(actual);
    }

    // 6) OTA 控制二进制帧：整帧字节相等。
    cJSON_ArrayForEach(item, section("ota_control_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const std::string kind = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "kind"));
        const cJSON* args = cJSON_GetObjectItemCaseSensitive(item, "args");
        const auto want = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const auto built = ContractBuildOtaControl(kind, args);
        if (!built.has_value()) {
            fail(name + ": 构建器缺失/构建失败 (kind=" + kind + ")");
            continue;
        }
        if (*built != want) {
            fail(name + ": 字节不符 got=" + ContractHexStr(*built) +
                 " want=" + ContractHexStr(want));
        }
    }

    cJSON_Delete(manifest);
    AbortIfFailed(failed, "TestContractFixtures");
}

// CI 诊断（2026-10-07）：进程级未处理异常探针——任何线程的硬异常（AV/栈溢出/
// fail-fast）在默认终止前把错误码与地址打进日志，用于 ctest SEGFAULT 定位。
//（曾试 AddVectoredExceptionHandlerFirst：SDK 头未声明、且 kernel32 导入库无此符号，
// 改用声明与导出俱在的 SetUnhandledExceptionFilter。）
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
    TestPowerLogMonitor();
    TestTextRefinerRules();
    TestRefineGuardSafety();
    TestLocalRefinementClientOrchestration();
    TestLocalRefinementCustomPrompt();
    TestLocalRefinementDiagnosticsLogs();
    TestLocalRefinementCrossTurnOrchestration();
    TestLocalRefinementCrossTurnDiagnosticsLogs();
    TestPinyinSameOrNear();
    TestApplyPinyinCorrections();
    TestSelectionCorrection();
    TestLocalRefinementGenerateCandidates();
    TestRefineHistory();
    TestLocalAsrClientStartFailsWhenModelMissing();
    TestLlamaCppEngineSessionTurnSmoke();
    TestLocalAsrClientSenseVoiceSmoke();
    TestLocalAsrClientEmitsPartialWhileStreaming();
    TestLocalAsrClientPartialThrottled();
    TestLocalAsrClientFinalReusesDecodeWhenNoNewAudio();
    TestLocalAsrClientSkipsEmptyPartial();
    TestLocalAsrClientCancelStopsPartial();
    TestPushToTalkKeyParsing();
    TestFormatPushToTalkKey();
    TestProviderComboMapping();
    TestShortcutCaptureClassifyKey();
    TestShortcutCapturePollEligibleVk();
    TestResolveAndValidateModelsDir();
    TestAppConfigLocalAsrRoundTrip();
    RunCoordinatorBatch6Tests();  // N8 cut9: suite in core_tests_coordinator6.cc
    TestResolveLocalRefineModelPathCrossTurn();
    TestLlamaCppEngineRealModelSmoke();
    printf(">> TestCoordinatorDeviceSessionRoutesToLocalAsrWhenEnabled\n"); fflush(stdout);
    printf(">> TestCoordinatorLocalMicShortPressDiscards\n"); fflush(stdout);
    printf(">> TestCoordinatorLocalMicDisabledDoesNothing\n"); fflush(stdout);
    printf(">> TestCoordinatorLocalMicCaptureStartFailureCancelsSession\n"); fflush(stdout);
    printf(">> TestWasapiMicCaptureSmoke\n"); fflush(stdout);
    TestWasapiMicCaptureSmoke();
    printf(">> wasapi smoke done\n"); fflush(stdout);
    printf(">> TestClipboardVaultMultiFormatRoundTrip\n"); fflush(stdout);
    TestClipboardVaultMultiFormatRoundTrip();
    printf(">> TestClipboardVaultSkipsHandleFormats\n"); fflush(stdout);
    TestClipboardVaultSkipsHandleFormats();
    printf(">> TestClipboardVaultEmptyClipboardSnapshot\n"); fflush(stdout);
    TestClipboardVaultEmptyClipboardSnapshot();
    printf(">> TestClipboardVaultSaveThrowsWhenBusy\n"); fflush(stdout);
    TestClipboardVaultSaveThrowsWhenBusy();
    printf(">> vault tests all done\n"); fflush(stdout);
    TestAudioFrameParsing();
    TestBleControlPayloads();
    RunProtocolContractTests();  // N8: suite in core_tests_protocol.cc
    TestContractFixtures();
    TestEncoderRotateStateParsing();
    TestStateEventSourceParsing();
    TestEncoderStatusParsing();
    // N8 cut14: orphan registration (pre-check: def was never called)
    RunLicenseImaAtvvBatchTests();  // N8 cut14: suite in core_tests_license_ima.cc
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
    TestSaveStaleCopyKeepsLicense();
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
    TestCoordinatorConcurrentUpdateConfigStress();
    RunXiaomiAtvvBatchTests();  // N8 cut7: suite in core_tests_xiaomi_atvv.cc
    RunXiaomiUsageTapBatchTests();  // N8 cut10: suite in core_tests_xiaomi_usage_tap.cc
    printf(">> cluster: B2 usage tap Stop bounded\n"); fflush(stdout);
    RunCoordinatorBatch4Tests();  // N8 cut6: suite in core_tests_coordinator4.cc
    RunCoordinatorBatch3Tests();  // N8 cut5: suite in core_tests_coordinator3.cc
    RunCoordinatorBatch5Tests();  // N8 cut8: suite in core_tests_coordinator5.cc
    TestBundledModelManifestWellFormed();
    printf(">> cluster: C5 model present sha256 verify\n"); fflush(stdout);
    TestModelFilePresentAndVerified();
    TestModelDownloaderPureFunctions();
    TestFinalizePartFile();
    TestModelDownloaderLoopback();
    TestModelDownloadSession();
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
