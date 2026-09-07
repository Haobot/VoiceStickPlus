#include "pair_device_helper.h"

#include "ble_protocol.h"

#include <algorithm>

namespace voicestick {

std::optional<std::string> ParseManualPairDeviceId(std::string_view input) {
    const auto normalized = BleProtocol::NormalizeDeviceId(input);
    if (normalized.empty()) return std::nullopt;
    return normalized;
}

namespace {

int HexDigitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::optional<std::uint64_t> ParseBluetoothAddressString(std::string_view text) {
    // Windows 设备接口地址属性（"AA:BB:CC:DD:EE:FF"）转 48 位地址，供「忘记设备」
    // 清 OS bond 时按地址匹配 DeviceInformation。仅容错首尾空白、大小写与
    // : / - 分隔差异；段长、段数、段内空白从严（解析面越宽，误匹配面越大）。
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (text.size() != 17) return std::nullopt;
    std::uint64_t address = 0;
    for (int group = 0; group < 6; ++group) {
        const std::size_t base = static_cast<std::size_t>(group) * 3;
        if (group > 0) {
            const char separator = text[base - 1];
            if (separator != ':' && separator != '-') return std::nullopt;
        }
        for (int i = 0; i < 2; ++i) {
            const int digit = HexDigitValue(text[base + i]);
            if (digit < 0) return std::nullopt;
            address = (address << 4) | static_cast<std::uint64_t>(digit);
        }
    }
    return address;
}

std::optional<PairingAdvertisementMatch> ClassifyPairingAdvertisement(
    std::string_view local_name,
    bool has_voice_stick_service,
    bool has_xiaomi_atvv_service,
    std::uint64_t bluetooth_address) {
    PairingAdvertisementMatch match;
    auto device_id = BleProtocol::DeviceIdFromName(local_name);
    if (device_id.has_value()) {
        match.device_id = *device_id;
        match.device_class =
            BleProtocol::DeviceClassFromName(local_name).value_or(DeviceClass::kStickS3);
        match.id_source = PairingCandidateIdSource::kName;
        return match;
    }
    if (has_voice_stick_service) {
        // 固件把名称放 SCAN_RSP：ADV 仅有 service UUID 时为同一地址生成临时候选。
        match.device_id = BleProtocol::DeviceIdFromBluetoothAddress(bluetooth_address);
        match.device_class = DeviceClass::kStickS3;
        match.id_source = PairingCandidateIdSource::kAddressFallback;
        match.is_temporary = true;
        return match;
    }
    if (BleProtocol::IsXiaomiRemoteName(local_name) || has_xiaomi_atvv_service) {
        // 小米遥控器无名称内嵌 ID：统一用蓝牙地址低 16 位（展示为 RC-XXXX）。
        match.device_id = BleProtocol::DeviceIdFromBluetoothAddress(bluetooth_address);
        match.device_class = DeviceClass::kXiaomiRemote2Pro;
        match.id_source = PairingCandidateIdSource::kAddressFallback;
        return match;
    }
    return std::nullopt;
}

bool MatchesPendingPairAddress(std::uint64_t message_address, std::uint64_t pending_address) {
    return pending_address != 0 && message_address == pending_address;
}

std::string CandidateDisplayTitle(const PairingCandidate& candidate) {
    if (candidate.is_temporary_candidate) {
        return "VoiceStick (waiting for name)";
    }

    // 空名兜底实际不可达（HandleAdvertisement 已按类别合成非空 display_name），
    // 仅作防御：前缀按 device_class 取，避免对小米候选拼出 "VS-" 的错误假设。
    std::string title = candidate.display_name.empty()
                            ? std::string(candidate.device_class == DeviceClass::kXiaomiRemote2Pro
                                              ? "RC-" : "VS-") +
                                  candidate.device_id
                            : candidate.display_name;
    if (candidate.is_existing_device) title += " (paired)";
    return title;
}

bool CanPairCandidate(const PairingCandidate& candidate) {
    return !candidate.is_existing_device &&
           !candidate.is_temporary_candidate &&
           !candidate.device_id.empty();
}

void MergePairingCandidate(std::vector<PairingCandidate>* candidates, const PairingCandidate& candidate) {
    auto address_it = std::find_if(candidates->begin(), candidates->end(), [&](const PairingCandidate& existing) {
        return existing.bluetooth_address == candidate.bluetooth_address;
    });
    if (address_it != candidates->end()) {
        // 同一物理地址的候选可能交替到达：固件把名称放 SCAN_RSP、ADV 只带
        // service UUID（见 voice_ble.c），Windows 部分广告包收不到名称，会为
        // 同一地址生成临时候选（按 MAC 低位推断 ID）。命名候选（广播名）优先，
        // 后到的临时包不得覆盖——否则用户看到列表显示 VS-XXXX、点配对取到的
        // 却是临时候选，配对对话框命中 "waiting for name" 不发起连接。
        if (address_it->is_temporary_candidate && !candidate.is_temporary_candidate) {
            *address_it = candidate;
        }
        return;
    }

    auto device_id_it = std::find_if(candidates->begin(), candidates->end(), [&](const PairingCandidate& existing) {
        return !existing.device_id.empty() && existing.device_id == candidate.device_id;
    });
    if (device_id_it == candidates->end()) {
        candidates->push_back(candidate);
        return;
    }

    if (!candidate.is_temporary_candidate || device_id_it->is_temporary_candidate) {
        *device_id_it = candidate;
    }
}

void RetainNamedPairingCandidate(std::vector<RetainedPairingCandidate>* retained,
                                 const PairingCandidate& candidate,
                                 std::uint64_t now_ms) {
    if (candidate.is_temporary_candidate) return;
    auto it = std::find_if(retained->begin(), retained->end(), [&](const RetainedPairingCandidate& existing) {
        return existing.candidate.device_id == candidate.device_id;
    });
    if (it == retained->end()) {
        retained->push_back(RetainedPairingCandidate{candidate, now_ms});
        return;
    }
    it->candidate = candidate;
    it->last_named_seen_ms = now_ms;
}

std::vector<PairingCandidate> VisiblePairingCandidates(
    const std::vector<PairingCandidate>& candidates,
    const std::vector<RetainedPairingCandidate>& retained,
    std::uint64_t now_ms,
    std::uint64_t retain_window_ms) {
    std::vector<PairingCandidate> visible;
    visible.reserve(candidates.size() + retained.size());
    for (const auto& retained_candidate : retained) {
        if (now_ms - retained_candidate.last_named_seen_ms <= retain_window_ms) {
            visible.push_back(retained_candidate.candidate);
        }
    }
    for (const auto& candidate : candidates) {
        if (candidate.is_temporary_candidate) continue;
        const auto it = std::find_if(visible.begin(), visible.end(), [&](const PairingCandidate& existing) {
            return existing.device_id == candidate.device_id;
        });
        if (it == visible.end()) visible.push_back(candidate);
    }
    return visible;
}

} // namespace voicestick
