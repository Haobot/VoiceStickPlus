#pragma once

#include <optional>
#include <string>

namespace voicestick {

// C7：云/ASR 链路 URL **只接受 TLS**——wss:// 映射为 https://、https:// 原样接受；
// ws:// 与 http://（明文）以及其余 scheme 一律拒绝（返回 nullopt）。api_key 与试用
// 凭据经此链路传输，明文等于把凭据放到网络上。配套的「设备证明」仍是待办（backlog C7b）。
// 头文件内联实现：core 单测可直接覆盖该策略，无需链接外壳层 .cc。
inline std::optional<std::string> SecureHttpUrlFromWebSocketUrl(const std::string& url) {
    if (url.compare(0, 6, "wss://") == 0) return "https://" + url.substr(6);
    if (url.compare(0, 8, "https://") == 0) return url;
    return std::nullopt;
}

// C7：响应里的结果链接随后被 ShellExecute 打开——只放行 https://，
// 防止明文链接或非 http scheme（file:// 等）在响应被篡改时被执行。
inline bool IsHttpsUrl(const std::string& url) {
    return url.compare(0, 8, "https://") == 0;
}

struct VoiceStickCloudApplyResult {
    std::string api_key;
    std::string url;
    std::string error;

    bool ok() const { return !api_key.empty() || !url.empty(); }
};

VoiceStickCloudApplyResult ApplyVoiceStickCloudTrialApiKey(
    const std::string& cloud_websocket_url,
    const std::string& device_id);

} // namespace voicestick
