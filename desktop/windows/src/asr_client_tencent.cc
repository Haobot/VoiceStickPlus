#include "asr_client_tencent.h"

#include "cJSON.h"
#include "log.h"
#include "tencent_asr_vocab_client.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <optional>
#include <sstream>
#include <string>

namespace voicestick {

namespace {

// ---- WinHTTP 基础工具 ----

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

bool StartsWithScheme(std::string_view text, std::string_view scheme) {
    return text.size() >= scheme.size() &&
           std::equal(scheme.begin(), scheme.end(), text.begin(), [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs)) ==
                      std::tolower(static_cast<unsigned char>(rhs));
           });
}

std::optional<std::wstring> WinHttpUrlFromWebSocketUrl(std::string_view websocket_url) {
    std::string http_url;
    if (StartsWithScheme(websocket_url, "wss://")) {
        http_url = "https://";
        http_url.append(websocket_url.substr(6));
    } else if (StartsWithScheme(websocket_url, "ws://")) {
        http_url = "http://";
        http_url.append(websocket_url.substr(5));
    } else {
        http_url = std::string(websocket_url);
    }
    auto wide = Utf16FromUtf8(http_url);
    if (wide.empty()) return std::nullopt;
    return wide;
}

constexpr int kResolveTimeoutMs = 5000;
constexpr int kConnectTimeoutMs = 5000;
constexpr int kSendTimeoutMs = 5000;
constexpr int kReceiveTimeoutMs = 15000;

void SetWinHttpTimeouts(HINTERNET handle) {
    if (!handle) return;
    WinHttpSetTimeouts(handle,
                       kResolveTimeoutMs,
                       kConnectTimeoutMs,
                       kSendTimeoutMs,
                       kReceiveTimeoutMs);
}

void CloseHandles(HINTERNET session, HINTERNET connect,
                  HINTERNET request, HINTERNET websocket) {
    if (websocket) WinHttpCloseHandle(websocket);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
}

std::string QueryStatusCode(HINTERNET request) {
    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status_code,
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    return std::to_string(status_code);
}

// ---- JSON 工具 ----

std::unique_ptr<cJSON, decltype(&cJSON_Delete)> ParseJson(std::string_view text) {
    return {cJSON_ParseWithLength(text.data(), text.size()), cJSON_Delete};
}

std::optional<std::string> JsonString(const cJSON* obj, const char* key) {
    if (!obj) return std::nullopt;
    const auto* child = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!child || !cJSON_IsString(child)) return std::nullopt;
    return std::string(child->valuestring);
}

std::optional<int> JsonInt(const cJSON* obj, const char* key) {
    if (!obj) return std::nullopt;
    const auto* child = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!child || !cJSON_IsNumber(child)) return std::nullopt;
    return child->valueint;
}

// ---- 字符转义 ----

std::string JsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

} // namespace

AsrClientTencent::AsrClientTencent(AppConfig config) : config_(std::move(config)) {}

AsrClientTencent::~AsrClientTencent() {
    ShutdownConnection();
}

// ============================================================
// 公开接口
// ============================================================

bool AsrClientTencent::Start(AsrSessionOptions options) {
    LogApp("TencentAsr::Start called appid=" + config_.ActiveTencentAppid() + " secret_id=" + config_.ActiveTencentSecretId().substr(0, 8) + "...");
    SetLastStartError({});
    if (config_.ActiveTencentSecretId().empty() || config_.ActiveTencentSecretKey().empty()) {
        SetLastStartError("缺少腾讯云 SecretId/SecretKey");
        return false;
    }
    if (config_.ActiveTencentAppid().empty()) {
        SetLastStartError("缺少腾讯云 AppId");
        return false;
    }
    session_options_ = std::move(options);
    if (session_options_.hotwords.empty()) {
        session_options_.hotwords = config_.asr_hotwords;
    }
    emitted_definite_segment_keys_.clear();
    latest_transcript_.clear();

    // 热词自动同步：在主线程执行（worker 线程产生前），避免跨线程 HTTP 请求死锁。
    // 仅首次调用时触发（cached_vocab_id_ 为空且配置未指定 hotword_id）。
    if (config_.tencent_hotword_id.empty() &&
        cached_vocab_id_.empty() &&
        !session_options_.hotwords.empty()) {
        TencentAsrVocabClient vocab_client(config_);
        auto synced_id = vocab_client.SyncHotwords(session_options_.hotwords);
        if (!synced_id.empty()) {
            cached_vocab_id_ = synced_id;
        }
    }

    {
        std::lock_guard lock(mutex_);
        if (session_state_ != SessionState::kIdle) {
            last_start_error_ = "ASR 会话已在进行中";
            return false;
        }
    }

    // 先彻底关闭可能存在的旧 worker 会话，避免它把我们刚设的 voice_id/state 清掉。
    ShutdownConnection();

    {
        std::lock_guard lock(mutex_);
        current_voice_id_ = GenerateVoiceId();
        queued_audio_chunks_.clear();
        latest_transcript_.clear();
        accumulated_final_text_.clear();
        emitted_definite_segment_keys_.clear();
        final_emitted_ = false;
        session_state_ = SessionState::kStarting;
        connection_state_ = ConnectionState::kConnecting;
        cancelled_ = false;
    }

    worker_ = std::thread([this] { RunWebSocket(); });
    return true;
}

void AsrClientTencent::SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) {
    SendAudio(data, is_last);
}

void AsrClientTencent::Cancel() {
    std::lock_guard lock(mutex_);
    queued_audio_chunks_.clear();
    latest_transcript_.clear();
    emitted_definite_segment_keys_.clear();
    if (session_state_ == SessionState::kStarting ||
        session_state_ == SessionState::kStreaming ||
        session_state_ == SessionState::kFinishing) {
        if (websocket_) {
            auto end_msg = MakeEndMessage();
            SendTextFrame(websocket_, end_msg);
        }
    }
    current_voice_id_.clear();
    session_state_ = SessionState::kIdle;
}

std::string AsrClientTencent::LastStartError() const {
    std::lock_guard lock(mutex_);
    return last_start_error_;
}

// ============================================================
// URL 签名
// ============================================================

std::string AsrClientTencent::HmacSha1(std::string_view key, std::string_view message) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    std::string result;

    // 打开 SHA1 算法提供者，指定 HMAC 模式
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return {};
    }

    // 通过 pbSecret 参数传入密钥，创建 HMAC 哈希对象
    auto key_bytes = const_cast<PUCHAR>(reinterpret_cast<const std::uint8_t*>(key.data()));
    if (BCryptCreateHash(alg, &hash_handle, nullptr, 0,
                         key_bytes, static_cast<ULONG>(key.size()), 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    // 传入消息数据
    auto msg_bytes = const_cast<PUCHAR>(reinterpret_cast<const std::uint8_t*>(message.data()));
    if (BCryptHashData(hash_handle, msg_bytes, static_cast<ULONG>(message.size()), 0) != 0) {
        BCryptDestroyHash(hash_handle);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    // 获取 HMAC 结果长度
    DWORD hash_size = 0;
    DWORD cb_data = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                      sizeof(hash_size), &cb_data, 0);

    // 获取 HMAC 结果
    std::vector<std::uint8_t> hash(hash_size);
    if (BCryptFinishHash(hash_handle, hash.data(), static_cast<ULONG>(hash.size()), 0) == 0) {
        result = std::string(reinterpret_cast<const char*>(hash.data()), hash.size());
    }

    BCryptDestroyHash(hash_handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

std::string AsrClientTencent::Base64Encode(std::span<const std::uint8_t> data) {
    // Base64 编码表
    static const char kBase64Table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    for (std::size_t i = 0; i < data.size(); i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<std::uint32_t>(data[i + 2]);

        out.push_back(kBase64Table[(n >> 18) & 0x3F]);
        out.push_back(kBase64Table[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size()) ? kBase64Table[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < data.size()) ? kBase64Table[n & 0x3F] : '=');
    }
    return out;
}

std::string AsrClientTencent::UrlEncode(std::string_view value) {
    std::string out;
    out.reserve(value.size() * 3);
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(ch);
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(ch));
            out += buf;
        }
    }
    return out;
}

std::string AsrClientTencent::BuildSignedUrl(const AppConfig& config,
                                              std::string_view voice_id) {
    // 收集签名参数（除 signature 外）
    struct Param { std::string key; std::string value; };
    std::vector<Param> params;

    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    auto expired = timestamp + 86400; // 24 小时后过期

    params.push_back({"secretid", config.ActiveTencentSecretId()});
    params.push_back({"timestamp", std::to_string(timestamp)});
    params.push_back({"expired", std::to_string(expired)});

    // nonce: 随机整数
    std::array<std::uint8_t, 4> nonce_bytes{};
    BCryptGenRandom(nullptr, nonce_bytes.data(), static_cast<ULONG>(nonce_bytes.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::uint32_t nonce = (static_cast<std::uint32_t>(nonce_bytes[0]) << 24) |
                          (static_cast<std::uint32_t>(nonce_bytes[1]) << 16) |
                          (static_cast<std::uint32_t>(nonce_bytes[2]) << 8) |
                          static_cast<std::uint32_t>(nonce_bytes[3]);
    params.push_back({"nonce", std::to_string(nonce)});

    params.push_back({"engine_model_type", config.tencent_engine_model_type});
    params.push_back({"voice_format", "10"});  // Opus
    params.push_back({"needvad", "1"});
    params.push_back({"voice_id", std::string(voice_id)});

    // 热词表 ID
    if (!config.tencent_hotword_id.empty()) {
        params.push_back({"hotword_id", config.tencent_hotword_id});
    }

    // 按 key 字典序排序
    std::sort(params.begin(), params.end(),
              [](const Param& a, const Param& b) { return a.key < b.key; });

    // 拼接签名原文：asr.cloud.tencent.com/asr/v2/<appid>?<sorted_query_params>
    // 注意：不包含 wss:// 协议头。
    std::ostringstream sign_str;
    sign_str << "asr.cloud.tencent.com/asr/v2/" << config.ActiveTencentAppid() << "?";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i != 0) sign_str << "&";
        sign_str << params[i].key << "=" << params[i].value;
    }

    // HMAC-SHA1 → Base64 → URL Encode
    auto hmac_result = HmacSha1(config.ActiveTencentSecretKey(), sign_str.str());
    auto hmac_bytes = std::span(reinterpret_cast<const std::uint8_t*>(hmac_result.data()),
                                 hmac_result.size());
    auto signature = UrlEncode(Base64Encode(hmac_bytes));

    Log("TASR", "sign_str=" + sign_str.str());
    Log("TASR", "signature=" + signature);

    // 构建完整 URL
    std::ostringstream url;
    url << "wss://asr.cloud.tencent.com/asr/v2/" << config.ActiveTencentAppid() << "?";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i != 0) url << "&";
        url << params[i].key << "=" << params[i].value;
    }
    url << "&signature=" << signature;

    return url.str();
}

std::string AsrClientTencent::GenerateVoiceId() {
    std::array<std::uint8_t, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        static std::atomic_uint counter = 0;
        const auto value = ++counter;
        char fallback[37]{};
        snprintf(fallback, sizeof(fallback), "vs-tencent-%08x", value);
        return fallback;
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    char out[37]{};
    snprintf(out, sizeof(out),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return out;
}

// ============================================================
// 协议消息构造
// ============================================================

std::string AsrClientTencent::MakeEndMessage() {
    // 腾讯云实时 ASR 结束消息固定为 {"type":"end"}（小写，不带 voice_id）。
    return R"({"type":"end"})";
}

// ============================================================
// WebSocket 帧发送
// ============================================================

bool AsrClientTencent::SendTextFrame(HINTERNET websocket, std::string_view text) {
    return WinHttpWebSocketSend(websocket,
                                WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                const_cast<char*>(text.data()),
                                static_cast<DWORD>(text.size())) == ERROR_SUCCESS;
}

bool AsrClientTencent::SendBinaryFrame(HINTERNET websocket, std::span<const std::uint8_t> data) {
    return WinHttpWebSocketSend(websocket,
                                WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                const_cast<std::uint8_t*>(data.data()),
                                static_cast<DWORD>(data.size())) == ERROR_SUCCESS;
}

// ============================================================
// 结果解析
// ============================================================

void AsrClientTencent::HandleTextResponse(std::string_view json, HINTERNET websocket) {
    Log("TASR", "received: " + std::string(json).substr(0, 200));
    auto root = ParseJson(json);
    if (!root) return;

    int code = JsonInt(root.get(), "code").value_or(-1);
    if (code != 0) {
        auto msg = ExtractErrorMessage(json);
        if (msg.empty()) {
            msg = "腾讯云 ASR 错误 (code=" + std::to_string(code) + ")";
        }
        // 某些错误码可以恢复（如资源包耗尽），通过 on_upgrade_url 通知
        if (code == 4004 || code == 4005) {
            std::string upgrade_url;
            if (on_upgrade_url) {
                on_upgrade_url("https://console.cloud.tencent.com/asr", msg);
            }
        }
        FailSession(msg);
        return;
    }

    // 整段识别结束信号：顶层 final=1（无 result 字段）。
    // 腾讯云实时 ASR 在音频流全部识别完成后返回 {"code":0,...,"final":1} 并断开连接。
    // 这才是整段最终结果，触发 on_final 输出累积全文。
    if (JsonInt(root.get(), "final").value_or(0) == 1) {
        Log("TASR", "received final=1, emitting accumulated final text");
        EmitFinalText();
        return;
    }

    // 识别结果（优先检查 result 字段，因为消息可能同时包含 message_id 和 result）
    const auto* result_obj = cJSON_GetObjectItemCaseSensitive(root.get(), "result");
    if (result_obj) {
        int slice_type = JsonInt(result_obj, "slice_type").value_or(-1);
        auto voice_text = JsonString(result_obj, "voice_text_str");

        if (slice_type == 0 || slice_type == 1) {
            // 中间结果（非稳态）
            {
                std::lock_guard lock(mutex_);
                if (voice_text.has_value() && !voice_text->empty()) {
                    latest_transcript_ = *voice_text;
                }
            }
            if (on_partial && voice_text.has_value() && !voice_text->empty()) {
                on_partial(*voice_text);
            }
            auto segments = ExtractWordListSegments(json, &emitted_definite_segment_keys_);
            for (const auto& seg : segments) {
                if (on_segment) on_segment(seg);
            }
        } else if (slice_type == 2) {
            // 单句稳态结果（VAD 切句，index 逐句递增）。
            // 注意：slice_type=2 是“一句话”的稳态结束，不是整段录音结束。
            // 整段结束由顶层 final=1 标识。此处仅累积该句文本，不触发 on_final、
            // 不重置会话——保持 kStreaming 以便继续接收后续句子与 button_up 的 end。
            std::vector<AsrSegment> segments;
            {
                std::lock_guard lock(mutex_);
                const std::string sentence =
                    (voice_text.has_value() && !voice_text->empty()) ? *voice_text : latest_transcript_;
                accumulated_final_text_ = AccumulateSentence(accumulated_final_text_, sentence);
                segments = ExtractWordListSegments(json, &emitted_definite_segment_keys_);
                // 当前句已稳态，清空中间结果缓冲，准备下一句；会话状态与累积文本保留。
                latest_transcript_.clear();
            }
            for (const auto& seg : segments) {
                if (on_segment) on_segment(seg);
            }
        }
        return;
    }

    // 握手确认或控制消息确认（无 result 字段）：
    // 腾讯云实时 ASR 握手成功后会返回 {"code":0,"message":"success","voice_id":"..."}，
    // 收到该消息后从 kStarting 进入 kStreaming 并刷新缓冲的音频。
    bool should_flush = false;
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::kStarting) {
            session_state_ = SessionState::kStreaming;
            should_flush = true;
        }
    }
    if (should_flush) {
        Log("TASR", "handshake confirmed, flushing queued audio");
        FlushQueuedAudioChunks();
    } else {
        Log("TASR", "ignored non-result message (session not in kStarting)");
    }
}

void AsrClientTencent::EmitFinalText() {
    std::string text;
    {
        std::lock_guard lock(mutex_);
        // 主动取消不 paste；本会话已触发过则不重复。
        if (cancelled_.load() || final_emitted_) return;
        final_emitted_ = true;
        text = accumulated_final_text_;
        if (text.empty()) text = latest_transcript_;  // 兜底：仅有中间结果未稳态
        accumulated_final_text_.clear();
        latest_transcript_.clear();
        current_voice_id_.clear();
        queued_audio_chunks_.clear();
        emitted_definite_segment_keys_.clear();
        session_state_ = SessionState::kIdle;
    }
    if (on_final) on_final(text);
}

std::string AsrClientTencent::ExtractVoiceText(std::string_view json) {
    auto root = ParseJson(json);
    if (!root) return {};
    const auto* result = cJSON_GetObjectItemCaseSensitive(root.get(), "result");
    if (!result) return {};
    return JsonString(result, "voice_text_str").value_or("");
}

int AsrClientTencent::ExtractSliceType(std::string_view json) {
    auto root = ParseJson(json);
    if (!root) return -1;
    const auto* result = cJSON_GetObjectItemCaseSensitive(root.get(), "result");
    if (!result) return -1;
    return JsonInt(result, "slice_type").value_or(-1);
}

int AsrClientTencent::ExtractFinalFlag(std::string_view json) {
    auto root = ParseJson(json);
    if (!root) return 0;
    // final 是顶层字段（与 code/message/voice_id 同级），返回 1 表示整段音频识别结束。
    return JsonInt(root.get(), "final").value_or(0);
}

std::string AsrClientTencent::AccumulateSentence(std::string_view current,
                                                 std::string_view sentence) {
    if (sentence.empty()) return std::string(current);
    if (current.empty()) return std::string(sentence);
    return std::string(current) + std::string(sentence);
}

int AsrClientTencent::ExtractErrorCode(std::string_view json) {
    auto root = ParseJson(json);
    if (!root) return -1;
    return JsonInt(root.get(), "code").value_or(-1);
}

std::string AsrClientTencent::ExtractErrorMessage(std::string_view json) {
    auto root = ParseJson(json);
    if (!root) return {};
    return JsonString(root.get(), "message").value_or("");
}

std::vector<AsrSegment> AsrClientTencent::ExtractWordListSegments(
    std::string_view json,
    std::set<std::string>* emitted_keys) {
    std::vector<AsrSegment> segments;
    auto root = ParseJson(json);
    if (!root) return segments;

    const auto* result = cJSON_GetObjectItemCaseSensitive(root.get(), "result");
    if (!result) return segments;

    const auto* word_list = cJSON_GetObjectItemCaseSensitive(result, "word_list");
    if (!word_list || !cJSON_IsArray(word_list)) return segments;

    // 聚合所有稳态词 (stable_flag == 1)
    std::string accumulated_text;
    int segment_start = -1;
    int segment_end = -1;

    const auto* array = word_list;
    for (int i = 0; i < cJSON_GetArraySize(array); ++i) {
        const auto* word_obj = cJSON_GetArrayItem(array, i);
        if (!word_obj) continue;

        auto word = JsonString(word_obj, "word");
        auto start_ms = JsonInt(word_obj, "start_time");
        auto end_ms = JsonInt(word_obj, "end_time");
        auto stable = JsonInt(word_obj, "stable_flag");

        if (!word.has_value()) continue;

        bool is_stable = stable.value_or(0) == 1;
        int start = start_ms.value_or(0);
        int end = end_ms.value_or(0);

        if (is_stable && !word->empty()) {
            accumulated_text += *word;
            if (segment_start < 0) segment_start = start;
            segment_end = end;
        }
    }

    if (!accumulated_text.empty()) {
        AsrSegment seg;
        seg.text = accumulated_text;
        seg.definite = true;
        seg.start_time = segment_start >= 0 ? std::optional<int>(segment_start) : std::nullopt;
        seg.end_time = segment_end >= 0 ? std::optional<int>(segment_end) : std::nullopt;

        auto key = SegmentKey(seg.start_time.value_or(-1),
                              seg.end_time.value_or(-1), seg.text);
        if (!emitted_keys || !emitted_keys->contains(key)) {
            if (emitted_keys) emitted_keys->insert(std::move(key));
            segments.push_back(std::move(seg));
        }
    }

    return segments;
}

std::string AsrClientTencent::SegmentKey(int start_ms, int end_ms, std::string_view text) {
    return std::to_string(start_ms) + ":" + std::to_string(end_ms) + ":" + std::string(text);
}

// ============================================================
// WebSocket 主循环
// ============================================================

void AsrClientTencent::RunWebSocket() {
    // RAII scope guard：无论 RunWebSocket 从哪个 return 退出（包括
    // FailSession 触发的提前返回），都会在析构时触发延迟的 on_error 回调。
    // 这避免了在 worker 线程内同步调用 on_error 导致与主线程的死锁。
    struct FirePendingError {
        AsrClientTencent* self;
        ~FirePendingError() {
            if (!self) return;
            std::string msg;
            {
                std::lock_guard lock(self->mutex_);
                msg = std::move(self->pending_error_message_);
                self->pending_error_message_.clear();
            }
            if (!msg.empty() && self->on_error) {
                Log("TASR", "firing pending error: " + msg);
                self->on_error(msg);
            }
        }
    } fire_guard{this};

    // 热词表 ID：优先使用配置的 VocabId，否则使用本次会话缓存（Start 中同步获取）
    std::string hotword_id = config_.tencent_hotword_id;
    if (hotword_id.empty() && !cached_vocab_id_.empty()) {
        hotword_id = cached_vocab_id_;
    }

    // 复制本次会话的 voice_id（在锁外使用）
    std::string voice_id;
    {
        std::lock_guard lock(mutex_);
        voice_id = current_voice_id_;
    }

    // 临时修改 hotword_id 用于本次签名（不修改 config_，因为在锁外）
    AppConfig signed_config = config_;
    if (!hotword_id.empty()) {
        signed_config.tencent_hotword_id = hotword_id;
    }
    auto signed_url = BuildSignedUrl(signed_config, voice_id);
    Log("TASR", "connecting to: " + signed_url.substr(0, 150) + "...");

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    const auto url_wide = WinHttpUrlFromWebSocketUrl(signed_url);
    if (!url_wide.has_value() || !WinHttpCrackUrl(url_wide->c_str(), 0, 0, &components)) {
        FailSession("无效的腾讯云 ASR URL");
        return;
    }

    HINTERNET session = WinHttpOpen(L"VoiceStick/Windows", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        FailSession("腾讯云 ASR 网络初始化失败: " + LastErrorText());
        return;
    }
    SetWinHttpTimeouts(session);

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connect) {
        CloseHandles(session, nullptr, nullptr, nullptr);
        FailSession("腾讯云 ASR 连接失败: " + LastErrorText());
        return;
    }

    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    std::wstring path_and_query;
    if (components.lpszUrlPath && components.dwUrlPathLength > 0) {
        path_and_query.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
        path_and_query.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path_and_query.empty()) path_and_query = L"/";

    // WebSocket 握手：偶发网络抖动或休眠唤醒后 WinHTTP 栈未就绪时，WinHttpSendRequest 可能
    // 瞬时失败（GetLastError()=0，已观测到第二次尝试成功）。session/connect 句柄复用，
    // 仅重建 request 重试，避免用户感知到"握手失败"。
    constexpr int kHandshakeMaxAttempts = 3;
    constexpr int kHandshakeRetryDelayMs = 500;
    HINTERNET request = nullptr;
    DWORD last_err = 0;
    for (int attempt = 1; attempt <= kHandshakeMaxAttempts; ++attempt) {
        request = WinHttpOpenRequest(connect, L"GET", path_and_query.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) {
            last_err = GetLastError();
            CloseHandles(session, connect, nullptr, nullptr);
            FailSession("腾讯云 ASR 请求创建失败: " + std::to_string(last_err));
            return;
        }
        SetWinHttpTimeouts(request);
        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            last_err = GetLastError();
            WinHttpCloseHandle(request);
            CloseHandles(session, connect, nullptr, nullptr);
            FailSession("腾讯云 ASR WebSocket 升级准备失败: " + std::to_string(last_err));
            return;
        }
        Log("TASR", "sending WebSocket handshake request (attempt " +
                     std::to_string(attempt) + "/" + std::to_string(kHandshakeMaxAttempts) + ")");
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            break;  // 握手成功
        }
        // 失败：在 CloseHandle 前取错误码（CloseHandle 会覆盖 GetLastError），重建 request 重试
        last_err = GetLastError();
        WinHttpCloseHandle(request);
        request = nullptr;
        if (attempt < kHandshakeMaxAttempts) {
            Log("TASR", "WebSocket handshake attempt " + std::to_string(attempt) +
                         " failed (err=" + std::to_string(last_err) + "); retrying in " +
                         std::to_string(kHandshakeRetryDelayMs) + "ms");
            Sleep(kHandshakeRetryDelayMs);
        }
    }
    if (!request) {
        CloseHandles(session, connect, nullptr, nullptr);
        FailSession("腾讯云 ASR WebSocket 握手失败: " + std::to_string(last_err));
        return;
    }
    const auto handshake_status = QueryStatusCode(request);
    Log("TASR", "WebSocket handshake response received, HTTP status=" + handshake_status);

    HINTERNET websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!websocket) {
        const auto status_code = QueryStatusCode(request);
        CloseHandles(session, connect, request, nullptr);
        FailSession(status_code.empty()
                    ? "腾讯云 ASR WebSocket 升级失败: " + LastErrorText()
                    : "腾讯云 ASR WebSocket 升级失败: HTTP " + status_code);
        return;
    }
    SetWinHttpTimeouts(websocket);
    WinHttpCloseHandle(request);
    request = nullptr;

    {
        std::lock_guard lock(mutex_);
        websocket_ = websocket;
        connection_state_ = ConnectionState::kReady;
        // 腾讯云实时 ASR 官方文档：握手成功后客户端直接上传音频数据。
        // 不等待服务端握手确认文本帧，避免服务端不发送确认时无限阻塞。
        if (session_state_ == SessionState::kStarting) {
            session_state_ = SessionState::kStreaming;
        }
    }

    Log("TASR", "WebSocket connected, voice_id=" + voice_id + ", flushing queued audio");
    FlushQueuedAudioChunks();

    // 接收循环
    int receive_timeouts = 0;
    while (!cancelled_) {
        std::array<std::uint8_t, 64 * 1024> buffer{};
        DWORD bytes_read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD result = WinHttpWebSocketReceive(websocket, buffer.data(),
                                                     static_cast<DWORD>(buffer.size()),
                                                     &bytes_read, &type);
        if (result == ERROR_WINHTTP_TIMEOUT) {
            ++receive_timeouts;
            Log("TASR", "websocket receive timeout #" + std::to_string(receive_timeouts));
            continue;
        }
        if (result != ERROR_SUCCESS) {
            if (cancelled_.load()) break;
            FailSession("腾讯云 ASR WebSocket 接收失败: " + std::to_string(result));
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            if (cancelled_.load()) break;
            Log("TASR", "websocket received close frame");
            break;
        }
        if (bytes_read == 0) {
            Log("TASR", "websocket received empty frame");
            continue;
        }
        Log("TASR", "websocket received " + std::to_string(bytes_read) + " bytes type=" +
                        std::to_string(type));
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            std::string_view text(reinterpret_cast<const char*>(buffer.data()), bytes_read);
            HandleTextResponse(text, websocket);
        }
        // 忽略二进制帧（腾讯云不发送二进制帧）
    }

    // 连接关闭兜底：正常流程服务端先发 final=1 再断开（EmitFinalText 已触发 on_final）。
    // 若服务端异常只断开未发 final=1，且本会话已发 end（kFinishing）并有累积文本，
    // 则补触发一次 on_final，避免 button_up 后丢文本。主动取消（cancelled_）不补。
    if (!cancelled_.load()) {
        bool need_fallback = false;
        {
            std::lock_guard lock(mutex_);
            need_fallback = !final_emitted_ &&
                            session_state_ == SessionState::kFinishing &&
                            !accumulated_final_text_.empty();
        }
        if (need_fallback) {
            Log("TASR", "connection closed without final=1, emitting accumulated text as fallback");
            EmitFinalText();
        }
    }

    {
        std::lock_guard lock(mutex_);
        if (websocket_ == websocket) websocket_ = nullptr;
        connection_state_ = ConnectionState::kDisconnected;
    }
    CloseHandles(session, connect, request, websocket);
}

void AsrClientTencent::ShutdownConnection() {
    cancelled_ = true;
    const bool has_worker = worker_.joinable();
    {
        std::lock_guard lock(mutex_);
        if (websocket_) {
            WinHttpWebSocketClose(websocket_,
                                  WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                  nullptr, 0);
            if (!has_worker) {
                WinHttpCloseHandle(websocket_);
                websocket_ = nullptr;
            }
        }
        queued_audio_chunks_.clear();
        current_voice_id_.clear();
        latest_transcript_.clear();
        emitted_definite_segment_keys_.clear();
        session_state_ = SessionState::kIdle;
        connection_state_ = ConnectionState::kDisconnected;
    }
    if (has_worker) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
        } else {
            worker_.join();
        }
    }
}

void AsrClientTencent::FailSession(const std::string& message) {
    const bool was_cancelled = cancelled_.load();
    bool had_active_session = false;
    cancelled_ = true;
    {
        std::lock_guard lock(mutex_);
        had_active_session = session_state_ != SessionState::kIdle;
        queued_audio_chunks_.clear();
        current_voice_id_.clear();
        latest_transcript_.clear();
        session_state_ = SessionState::kIdle;
        connection_state_ = ConnectionState::kDisconnected;
        websocket_ = nullptr;
    }
    // 重要：不在持有锁或跨线程时同步回调 on_error。
    // FinishWithAsrError 会回调 asr_->Cancel()，如果本函数被 worker 线程调用，
    // 可能与主线程的 SendAudio/Cancel 形成锁序死锁。
    // 因此仅记录 pending 错误，由 RunWebSocket 退出后在安全上下文触发。
    if (!was_cancelled && had_active_session) {
        std::lock_guard lock(mutex_);
        if (pending_error_message_.empty()) {
            pending_error_message_ = message;
        }
    }
}

bool AsrClientTencent::SendFrameOrFail(HINTERNET /*websocket*/,
                                        const ByteVector& frame,
                                        WINHTTP_WEB_SOCKET_BUFFER_TYPE type,
                                        const std::string& context) {
    const auto message = "失败: " + context;
    bool should_notify = false;
    {
        std::lock_guard lock(mutex_);
        if (websocket_ && WinHttpWebSocketSend(websocket_, type,
                                 const_cast<std::uint8_t*>(frame.data()),
                                 static_cast<DWORD>(frame.size())) == ERROR_SUCCESS) {
            Log("TASR", context + " succeeded, bytes=" + std::to_string(frame.size()));
            return true;
        }
        const bool was_cancelled = cancelled_.load();
        const bool had_active_session = session_state_ != SessionState::kIdle;
        last_start_error_ = message;
        cancelled_ = true;
        queued_audio_chunks_.clear();
        current_voice_id_.clear();
        latest_transcript_.clear();
        session_state_ = SessionState::kIdle;
        connection_state_ = ConnectionState::kDisconnected;
        websocket_ = nullptr;
        should_notify = !was_cancelled && had_active_session;
    }
    // 延后回调，避免跨线程锁竞争导致死锁
    if (should_notify) {
        std::lock_guard lock(mutex_);
        if (pending_error_message_.empty()) {
            pending_error_message_ = message;
        }
    }
    return false;
}

// ============================================================
// 音频格式转换
// ============================================================

// 将 Ogg Opus 页面中的单个 Opus 包转换为腾讯云实时 ASR 要求的封装格式：
// "Opus"（4 字节）+ 帧数据长度（2 字节，大端）+ Opus 一帧压缩数据。
// 返回空 vector 表示该数据不是可发送的音频帧（如 OpusHead/OpusTags）。
ByteVector AsrClientTencent::ExtractTencentOpusFrame(std::span<const std::uint8_t> data) {
    if (data.size() < 27) return {};
    if (data[0] != 'O' || data[1] != 'g' || data[2] != 'g' || data[3] != 'S') return {};
    const auto page_segments = data[26];
    const auto header_size = static_cast<std::size_t>(27) + page_segments;
    if (header_size >= data.size()) return {};
    const auto raw = data.subspan(header_size);
    // OpusHead/OpusTags 头包以 "Opus" 开头（4字节），但不是音频帧
    if (raw.size() >= 4 && raw[0] == 'O' && raw[1] == 'p' &&
        raw[2] == 'u' && raw[3] == 's') {
        return {};
    }
    // 当前 OggOpusMuxer 每页只放一个 Opus 包（packet <= 255），所以 raw 即为一帧。
    ByteVector frame;
    frame.reserve(4 + 2 + raw.size());
    frame.insert(frame.end(), {'o', 'p', 'u', 's'});
    frame.push_back(static_cast<std::uint8_t>(raw.size() & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((raw.size() >> 8) & 0xFF));
    frame.insert(frame.end(), raw.begin(), raw.end());
    return frame;
}

// ============================================================
// 音频发送
// ============================================================

void AsrClientTencent::SendAudio(std::span<const std::uint8_t> data, bool is_last) {
    SessionState state = SessionState::kIdle;
    {
        std::lock_guard lock(mutex_);
        state = session_state_;
        if (state == SessionState::kStarting) {
            queued_audio_chunks_.push_back(QueuedAudioChunk{ByteVector(data.begin(), data.end()), is_last});
            if (is_last) {
                Log("TASR", "audio chunk queued (session still starting, marked as last)");
            }
            return;
        }
        if (state == SessionState::kIdle || state == SessionState::kFinishing) return;
    }

    auto tencent_frame = ExtractTencentOpusFrame(data);
    if (!tencent_frame.empty()) {
        if (!SendFrameOrFail(websocket_,
                             tencent_frame,
                             WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                             "发送音频数据")) {
            return;
        }
    }
    if (is_last) {
        FinishSessionIfNeeded();
    }
}

void AsrClientTencent::FlushQueuedAudioChunks() {
    std::vector<QueuedAudioChunk> chunks;
    {
        std::lock_guard lock(mutex_);
        chunks.swap(queued_audio_chunks_);
    }
    for (const auto& chunk : chunks) {
        auto tencent_frame = ExtractTencentOpusFrame(std::span(chunk.data));
        if (!tencent_frame.empty()) {
            if (!SendFrameOrFail(websocket_,
                                 tencent_frame,
                                 WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                 "发送缓冲音频")) {
                return;
            }
        }
        if (chunk.is_last) {
            FinishSessionIfNeeded();
        }
    }
}

void AsrClientTencent::FinishSessionIfNeeded() {
    {
        std::lock_guard lock(mutex_);
        if (session_state_ == SessionState::kStarting) {
            // 音频已到达但握手尚未确认，将 end 标记加入缓冲
            const auto has_last = std::any_of(
                queued_audio_chunks_.begin(), queued_audio_chunks_.end(),
                [](const QueuedAudioChunk& chunk) { return chunk.is_last; });
            if (!has_last) {
                queued_audio_chunks_.push_back(QueuedAudioChunk{{}, true});
                Log("TASR", "queueing end marker until handshake confirmation");
            }
            return;
        }
        if (session_state_ != SessionState::kStreaming || current_voice_id_.empty()) {
            Log("TASR", "FinishSessionIfNeeded skipped: state=" +
                            std::to_string(static_cast<int>(session_state_)));
            return;
        }
        session_state_ = SessionState::kFinishing;
    }
    Log("TASR", "sending end message");
    auto end_msg = MakeEndMessage();
    SendFrameOrFail(websocket_,
                    ByteVector(end_msg.begin(), end_msg.end()),
                    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                    "发送 end 消息");
}

void AsrClientTencent::SetLastStartError(std::string message) {
    std::lock_guard lock(mutex_);
    last_start_error_ = std::move(message);
}

std::string AsrClientTencent::LastErrorText() {
    return std::to_string(GetLastError());
}

} // namespace voicestick
