#pragma once

#include "voice_stick_coordinator.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

enum class PairingCandidateIdSource {
    kName,
    kAddressFallback,
    kManual,
};

struct PairingCandidate {
    std::uint64_t bluetooth_address = 0;
    BluetoothAddressKind address_kind = BluetoothAddressKind::kUnspecified;
    std::string display_name;
    std::string device_id;
    int rssi = 0;
    PairingCandidateIdSource id_source = PairingCandidateIdSource::kName;
    // 设备类别：StickS3 或小米遥控器 2 Pro（识别逻辑见 ClassifyPairingAdvertisement）。
    DeviceClass device_class = DeviceClass::kStickS3;
    bool is_existing_device = false;
    bool is_temporary_candidate = false;
};

// 双模配对扫描的候选识别结果（纯逻辑，可单测）。
struct PairingAdvertisementMatch {
    std::string device_id;                // 去前缀 4 位大写 hex
    DeviceClass device_class = DeviceClass::kStickS3;
    PairingCandidateIdSource id_source = PairingCandidateIdSource::kName;
    bool is_temporary = false;            // StickS3 无名称仅有 service UUID 时的临时候选
};

// 广告包识别（与 BleCentralWin::HandleAdvertisement 同一规则）：名称命中（VS-/RC- 前缀
// 或小米名称白名单）→ 按名取 ID 与类别；否则 VoiceStick service UUID → 地址兜底临时
// 候选；否则小米名称白名单/ATVV service UUID → 地址低 16 位 RC 候选；都不命中返回
// nullopt（非目标设备）。
std::optional<PairingAdvertisementMatch> ClassifyPairingAdvertisement(
    std::string_view local_name,
    bool has_voice_stick_service,
    bool has_xiaomi_atvv_service,
    std::uint64_t bluetooth_address);

struct RetainedPairingCandidate {
    PairingCandidate candidate;
    std::uint64_t last_named_seen_ms = 0;
};

std::optional<std::string> ParseManualPairDeviceId(std::string_view input);
// 异步配对消息与 pending 候选的地址匹配判定：陈旧消息（上一目标的迟到回调）
// 地址不符即丢弃，防止错位命中新配对目标。
bool MatchesPendingPairAddress(std::uint64_t message_address, std::uint64_t pending_address);
std::string CandidateDisplayTitle(const PairingCandidate& candidate);
bool CanPairCandidate(const PairingCandidate& candidate);
void MergePairingCandidate(std::vector<PairingCandidate>* candidates, const PairingCandidate& candidate);
void RetainNamedPairingCandidate(std::vector<RetainedPairingCandidate>* retained,
                                 const PairingCandidate& candidate,
                                 std::uint64_t now_ms);
std::vector<PairingCandidate> VisiblePairingCandidates(
    const std::vector<PairingCandidate>& candidates,
    const std::vector<RetainedPairingCandidate>& retained,
    std::uint64_t now_ms,
    std::uint64_t retain_window_ms);

} // namespace voicestick
