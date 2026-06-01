#pragma once

#include "voice_stick_coordinator.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

std::optional<std::string> ParseManualPairDeviceId(std::string_view input);
std::string CandidateDisplayTitle(const PairingCandidate& candidate);
bool CanPairCandidate(const PairingCandidate& candidate);

} // namespace voicestick
