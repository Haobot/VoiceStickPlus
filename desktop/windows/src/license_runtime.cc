#include "license_runtime.h"

#include "app_config.h"
#include "ble_protocol.h"
#include "log.h"
#include "machine_guid_win.h"
#include "voice_stick_coordinator.h"

#include <exception>
#include <string>
#include <utility>

namespace voicestick {

LicenseRuntime::LicenseRuntime(AppConfig* config, VoiceStickCoordinator* coordinator)
    : config_(config), coordinator_(coordinator) {}

std::vector<std::string> LicenseRuntime::NormalizedDeviceIds() const {
    std::vector<std::string> devices;
    if (coordinator_ == nullptr) return devices;
    for (const auto& id : coordinator_->ConnectedDeviceIds()) {
        const auto normalized = BleProtocol::NormalizeDeviceId(id);
        if (!normalized.empty()) devices.push_back(normalized);
    }
    return devices;
}

LicenseStatus LicenseRuntime::CurrentStatus() const {
    LicenseConfig cfg;
    cfg.serial = config_->license.serial;
    cfg.trial_anchor_days = config_->license.trial_anchor_days;
    cfg.last_seen_days = config_->license.last_seen_days;
    const auto guid = ReadMachineGuid();
    if (!guid.has_value()) {
        LogApp("license: read MachineGuid failed; binding falls back to device id only");
    }
    return EvaluateLicense(cfg, NormalizedDeviceIds(), guid.value_or(std::string()),
                           DaysSinceEpochTodayUtc());
}

void LicenseRuntime::EnsureTrialAnchor() {
    if (!config_->local_asr.enabled || config_->license.trial_anchor_days.has_value()) return;
    config_->license.trial_anchor_days = DaysSinceEpochTodayUtc();
    SaveConfigQuietly("trial anchor");
    LogApp("license: trial anchor written days=" +
           std::to_string(*config_->license.trial_anchor_days));
}

void LicenseRuntime::AdvanceLastSeen() {
    const auto now = DaysSinceEpochTodayUtc();
    if (config_->license.last_seen_days.has_value() &&
        *config_->license.last_seen_days >= now) {
        return;
    }
    config_->license.last_seen_days = now;
    SaveConfigQuietly("last_seen");
}

bool LicenseRuntime::LocalAsrAllowed() const {
    const auto status = CurrentStatus();
    return status.state == LicenseState::kTrial || status.state == LicenseState::kActive;
}

void LicenseRuntime::LogStatus(const char* reason) const {
    const auto status = CurrentStatus();
    const char* state = status.state == LicenseState::kTrial    ? "trial"
                        : status.state == LicenseState::kActive ? "active"
                                                                : "expired";
    LogApp(std::string("license: status (") + reason + ") state=" + state +
           " days_remaining=" + std::to_string(status.days_remaining) +
           " perpetual=" + (status.perpetual ? "true" : "false") +
           " rollback=" + (status.clock_rollback ? "true" : "false") +
           " devices=" + std::to_string(NormalizedDeviceIds().size()));
}

void LicenseRuntime::SaveConfigQuietly(const char* what) {
    try {
        config_->Save();
    } catch (const std::exception& error) {
        LogApp(std::string("license: save ") + what + " failed: " + error.what());
    }
}

}  // namespace voicestick
