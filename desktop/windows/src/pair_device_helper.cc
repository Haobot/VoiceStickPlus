#include "pair_device_helper.h"

#include "ble_protocol.h"

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

} // namespace voicestick
