#include "pair_device_helper.h"

#include "ble_protocol.h"

#include <algorithm>

namespace voicestick {

std::optional<std::string> ParseManualPairDeviceId(std::string_view input) {
    const auto normalized = BleProtocol::NormalizeDeviceId(input);
    if (normalized.empty()) return std::nullopt;
    return normalized;
}

std::string CandidateDisplayTitle(const PairingCandidate& candidate) {
    if (candidate.is_temporary_candidate) {
        return "VoiceStick (waiting for name)";
    }

    std::string title = candidate.display_name.empty()
                            ? "VS-" + candidate.device_id
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
