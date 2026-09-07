#pragma once

#include "byte_utils.h"

#include <functional>
#include <optional>
#include <string>

namespace voicestick {

struct FirmwareManifest {
    std::string hardware;
    std::string version;
    // 最低兼容固件版本（manifest 可选字段）：低于此版本强制升级。
    // 旧 Release 不下发时为空，由 EffectiveMinimumFirmwareVersion 回退本地常量。
    std::string min_version;
    std::string ota_url;
    std::string ota_sha256;
    std::uint32_t ota_size = 0;
    std::string merged_url;
    std::string merged_sha256;
    std::uint32_t merged_size = 0;
};

struct DeviceFirmwareInfo {
    std::string hardware;
    std::string current_version;
    std::string latest_version;
    bool update_available = false;
    bool is_checking = false;
    std::string error_message;
};

class FirmwareManifestClient {
public:
    using ManifestCallback = std::function<void(std::optional<FirmwareManifest>, std::string)>;

    explicit FirmwareManifestClient(std::string manifest_url = DefaultManifestUrl());

    void FetchManifest(ManifestCallback callback) const;
    std::optional<FirmwareManifest> FetchManifestSync(std::string& error) const;
    std::optional<ByteVector> DownloadOtaSync(const FirmwareManifest& manifest, std::string& error) const;

    static std::string DefaultManifestUrl();

private:
    std::string manifest_url_;
};

class FirmwareVersion {
public:
    static bool IsOlderThan(std::string_view current, std::string_view latest);
};

std::optional<FirmwareManifest> ParseFirmwareManifest(std::string_view json);
bool IsFirmwareHardwareCompatible(std::string_view device_hardware,
                                  std::string_view current_version,
                                  std::string_view manifest_hardware);

// manifest 下发的最低兼容版本；缺失/为空时回退 fallback（AppConfig 本地常量）。
std::string EffectiveMinimumFirmwareVersion(const FirmwareManifest& manifest,
                                            std::string_view fallback);

// 设备固件升级紧急度：低于 min_version 强制（kRequired），介于 min_version 与
// version 之间可选（kOptional），已是最新则无需更新（kUpToDate）。
enum class FirmwareUpdateUrgency {
    kUpToDate,
    kOptional,
    kRequired,
};

FirmwareUpdateUrgency ClassifyFirmwareUpdateUrgency(std::string_view current_version,
                                                    const FirmwareManifest& manifest,
                                                    std::string_view fallback_minimum_version);

} // namespace voicestick
