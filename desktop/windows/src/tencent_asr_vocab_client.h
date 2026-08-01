#pragma once

#include "app_config.h"

#include <functional>
#include <string>
#include <vector>

namespace voicestick {

/// 腾讯云 ASR 热词表管理 REST API 客户端。
///
/// 通过 CreateAsrVocab / UpdateAsrVocab / GetAsrVocabList 接口
/// 自动管理热词表，返回 VocabId 供 WebSocket ASR 引用。
///
/// 使用 TC3-HMAC-SHA256 签名（腾讯云 API 3.0 标准）。
class TencentAsrVocabClient {
public:
    explicit TencentAsrVocabClient(const AppConfig& config);

    /// 同步热词表：用给定的热词列表创建或更新名为 "VoiceStick-Hotwords" 的热词表。
    /// 返回 VocabId；失败时返回空字符串。
    /// 调用方应在后台线程中调用此方法以避免阻塞 UI。
    std::string SyncHotwords(const std::vector<std::string>& hotwords);

    /// 腾讯热词词表 API 只接受中英文/数字/连字符/下划线；含 '.' 等字符的词会
    /// 让整个请求被拒之门外（InvalidParameterValue.InvalidWordWeight，2026-08-01
    /// 热词评测实测）。同步前必须逐词过滤，否则一个非法词会让整表同步失败。
    /// 判定宽松放行的字节：ASCII 字母数字与 '_' '-'，以及所有 >= 0x80 的
    /// UTF-8 多字节序列（CJK 等）。
    static bool IsValidHotwordChars(std::string_view word);

private:
    struct HotWordEntry {
        std::string word;
        int weight = 10;  // 默认权重 10
    };

    // ---- TC3 签名 ----
    static std::string HmacSha256(std::string_view key, std::string_view message);
    static std::string Sha256Hex(std::string_view message);
    static std::string Tc3Signature(const std::string& secret_id,
                                     const std::string& secret_key,
                                     const std::string& service,
                                     const std::string& host,
                                     const std::string& action,
                                     const std::string& payload,
                                     const std::string& timestamp);

    // ---- REST API ----
    std::string CallApi(const std::string& action, const std::string& payload);
    std::string CreateVocab(const std::string& name,
                            const std::vector<HotWordEntry>& words,
                            const std::string& description = {});
    std::string UpdateVocab(const std::string& vocab_id,
                            const std::vector<HotWordEntry>& words);
    std::string FindVocabId(const std::string& name);

    AppConfig config_;

    static constexpr const char* kHost = "asr.tencentcloudapi.com";
    static constexpr const char* kService = "asr";
    static constexpr const char* kVersion = "2019-06-14";
    static constexpr const char* kDefaultVocabName = "VoiceStick-Hotwords";
};

} // namespace voicestick
