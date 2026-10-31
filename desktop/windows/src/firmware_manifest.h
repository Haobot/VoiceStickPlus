#pragma once

#include "byte_utils.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace voicestick {

struct FirmwareManifest {
    std::string hardware;
    std::string version;
    // 最低兼容固件版本（manifest 可选字段）：低于此版本强制升级。
    // 旧 Release 不下发时为空，由 EffectiveMinimumFirmwareVersion 回退本地常量。
    std::string min_version;
    std::string ota_url;
    // GitHub Release 回退下载源（COS 分发面可选字段）：主源不可达或内容损坏时依序重试。
    // 旧 manifest 不下发时为空，OtaDownloadUrls 仅返回主源。
    std::string ota_url_fallback;
    std::string ota_sha256;
    std::uint32_t ota_size = 0;
    std::string merged_url;
    std::string merged_url_fallback;
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

    // 默认构造：国内 COS 主源，拉取失败回退 GitHub Release（见 Doc/Rfc/tencent-cos-domestic-distribution-2026-09-09.md）。
    FirmwareManifestClient();
    // 单源构造：测试或自定义 manifest 源注入，不做回退。
    explicit FirmwareManifestClient(std::string manifest_url);

    void FetchManifest(ManifestCallback callback) const;
    std::optional<FirmwareManifest> FetchManifestSync(std::string& error) const;
    std::optional<ByteVector> DownloadOtaSync(const FirmwareManifest& manifest, std::string& error) const;

    static std::string DefaultManifestUrl();
    static std::string FallbackManifestUrl();

private:
    FirmwareManifestClient(std::string manifest_url, std::string fallback_manifest_url);

    std::string manifest_url_;
    std::string fallback_manifest_url_;
};

class FirmwareVersion {
public:
    static bool IsOlderThan(std::string_view current, std::string_view latest);
};

std::optional<FirmwareManifest> ParseFirmwareManifest(std::string_view json);
// OTA 固件的下载源序列：主源在前，manifest 带 ota_url_fallback 时回退源在后。
std::vector<std::string> OtaDownloadUrls(const FirmwareManifest& manifest);
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
