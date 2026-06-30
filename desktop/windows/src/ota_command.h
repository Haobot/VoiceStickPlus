#pragma once

#include "app_config.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

enum class OtaWaitMode {
    kSuccess,
    kHealthy,
};

struct OtaPullCommand {
    std::string request_id;
    std::string reply_pipe;
    std::string device_id;
    std::string url;
    std::string sha256_hex;
    OtaWaitMode wait_mode = OtaWaitMode::kHealthy;
    int timeout_sec = 180;
    bool json_output = false;
    bool save_config = false;
};

struct OtaHealthyDecisionInput {
    bool saw_success = false;
    bool saw_disconnect_after_success = false;
    bool saw_reconnect_after_success = false;
    bool wifi_status_after_reconnect = false;
    bool ota_pending_verify = true;
    std::string running_partition;
};

bool ShouldCompleteOtaHealthy(const OtaHealthyDecisionInput& input);
bool ShouldSendOtaCommit(const OtaHealthyDecisionInput& input, bool commit_sent);

std::string OtaWaitModeName(OtaWaitMode mode);
OtaWaitMode OtaWaitModeFromName(std::string_view name);
std::string NormalizeOtaDeviceId(std::string_view device_id);

std::optional<OtaPullCommand> ParseOtaCommandLine(const std::vector<std::string>& args,
                                                  std::string* error);
bool ResolveOtaPullCommandFromConfig(const AppConfig& config,
                                     OtaPullCommand* command,
                                     std::string* error);
bool ValidateOtaPullCommand(const OtaPullCommand& command, std::string* error);

std::string SerializeOtaIpcRequest(const OtaPullCommand& command);
std::optional<OtaPullCommand> ParseOtaIpcRequest(std::string_view json, std::string* error);

} // namespace voicestick
