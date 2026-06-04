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
    bool is_existing_device = false;
    bool is_temporary_candidate = false;
};

struct RetainedPairingCandidate {
    PairingCandidate candidate;
    std::uint64_t last_named_seen_ms = 0;
};

std::optional<std::string> ParseManualPairDeviceId(std::string_view input);
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
