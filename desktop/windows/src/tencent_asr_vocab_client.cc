#include "tencent_asr_vocab_client.h"

#include "cJSON.h"

#include <Windows.h>
#include <Winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace voicestick {

namespace {

// ---- WinHTTP 同步请求工具 ----

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

std::string Utf8FromUtf16(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return {};
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        narrow.data(), length, nullptr, nullptr);
    return narrow;
}

// 字符串转小写
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// ---- BCrypt SHA256 基础 ----

BCRYPT_ALG_HANDLE OpenSha256Alg() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    return alg;
}

std::string DoHash(BCRYPT_ALG_HANDLE alg, std::string_view data) {
    if (!alg) return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) return {};

    auto bytes = const_cast<PUCHAR>(reinterpret_cast<const std::uint8_t*>(data.data()));
    if (BCryptHashData(hash, bytes, static_cast<ULONG>(data.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        return {};
    }

    DWORD hash_size = 0;
    DWORD cb = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                      sizeof(hash_size), &cb, 0);
    std::vector<std::uint8_t> buf(hash_size);
    if (BCryptFinishHash(hash, buf.data(), static_cast<ULONG>(buf.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        return {};
    }
    BCryptDestroyHash(hash);
    return std::string(reinterpret_cast<const char*>(buf.data()), buf.size());
}

std::string BytesToHex(std::string_view data) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char c : data) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

} // namespace

// ============================================================
// TC3-HMAC-SHA256 签名
// ============================================================

std::string TencentAsrVocabClient::HmacSha256(std::string_view key, std::string_view message) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    std::string result;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return {};
    }

    auto key_bytes = const_cast<PUCHAR>(reinterpret_cast<const std::uint8_t*>(key.data()));
    if (BCryptCreateHash(alg, &hash_handle, nullptr, 0,
                         key_bytes, static_cast<ULONG>(key.size()), 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    auto msg_bytes = const_cast<PUCHAR>(reinterpret_cast<const std::uint8_t*>(message.data()));
    if (BCryptHashData(hash_handle, msg_bytes, static_cast<ULONG>(message.size()), 0) != 0) {
        BCryptDestroyHash(hash_handle);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    DWORD hash_size = 0;
    DWORD cb = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                      sizeof(hash_size), &cb, 0);
    std::vector<std::uint8_t> buf(hash_size);
    if (BCryptFinishHash(hash_handle, buf.data(), static_cast<ULONG>(buf.size()), 0) == 0) {
        result = std::string(reinterpret_cast<const char*>(buf.data()), buf.size());
    }

    BCryptDestroyHash(hash_handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

std::string TencentAsrVocabClient::Sha256Hex(std::string_view message) {
    auto alg = OpenSha256Alg();
    auto hash = DoHash(alg, message);
    BCryptCloseAlgorithmProvider(alg, 0);
    return BytesToHex(hash);
}

std::string TencentAsrVocabClient::Tc3Signature(const std::string& secret_id,
                                                  const std::string& secret_key,
                                                  const std::string& service,
                                                  const std::string& host,
                                                  const std::string& action,
                                                  const std::string& payload,
                                                  const std::string& timestamp_str) {
    // 从 timestamp_str (Unix 秒数) 推导日期 YYYY-MM-DD
    auto ts = std::stoll(timestamp_str);
    std::time_t tt = static_cast<std::time_t>(ts);
    std::tm tm_buf{};
    gmtime_s(&tm_buf, &tt);

    char date_buf[16]{};
    snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
    std::string date(date_buf);

    std::string credential_scope = date + "/" + service + "/tc3_request";

    // ---- 规范请求 ----
    std::string http_method = "POST";
    std::string canonical_uri = "/";
    std::string canonical_querystring = "";
    std::string action_lower = Lower(action);
    std::string canonical_headers =
        "content-type:application/json; charset=utf-8\n"
        "host:" + host + "\n"
        "x-tc-action:" + action_lower + "\n";
    std::string signed_headers = "content-type;host;x-tc-action";
    std::string hashed_payload = Sha256Hex(payload);

    std::string canonical_request =
        http_method + "\n" +
        canonical_uri + "\n" +
        canonical_querystring + "\n" +
        canonical_headers + "\n" +
        signed_headers + "\n" +
        hashed_payload;

    // ---- 待签字符串 ----
    std::string algorithm = "TC3-HMAC-SHA256";
    std::string hashed_canonical_request = Sha256Hex(canonical_request);

    std::string string_to_sign =
        algorithm + "\n" +
        timestamp_str + "\n" +
        credential_scope + "\n" +
        hashed_canonical_request;

    // ---- 计算签名 ----
    auto secret_date = HmacSha256("TC3" + secret_key, date);
    auto secret_service = HmacSha256(secret_date, service);
    auto secret_signing = HmacSha256(secret_service, "tc3_request");
    auto signature_bytes = HmacSha256(secret_signing, string_to_sign);
    auto signature = BytesToHex(signature_bytes);

    // ---- 组装 Authorization ----
    return algorithm + " Credential=" + secret_id + "/" + credential_scope +
           ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
}

// ============================================================
// REST API 调用
// ============================================================

TencentAsrVocabClient::TencentAsrVocabClient(const AppConfig& config) : config_(config) {}

std::string TencentAsrVocabClient::CallApi(const std::string& action,
                                            const std::string& payload) {
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    auto timestamp_str = std::to_string(ts);

    auto auth = Tc3Signature(config_.tencent_secret_id, config_.tencent_secret_key,
                              kService, kHost, action, payload, timestamp_str);

    // 构建 HTTP 请求头
    std::string action_lower = Lower(action);
    std::ostringstream headers;
    headers << "Authorization: " << auth << "\r\n";
    headers << "Content-Type: application/json; charset=utf-8\r\n";
    headers << "Host: " << kHost << "\r\n";
    headers << "X-TC-Action: " << action_lower << "\r\n";
    headers << "X-TC-Timestamp: " << timestamp_str << "\r\n";
    headers << "X-TC-Version: " << kVersion << "\r\n";
    headers << "X-TC-Region: ap-guangzhou\r\n";

    auto headers_wide = Utf16FromUtf8(headers.str());

    // WinHTTP 同步请求
    HINTERNET session = WinHttpOpen(L"VoiceStick/Windows",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {};

    DWORD timeout = 10000;
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    auto host_wide = Utf16FromUtf8(kHost);
    HINTERNET connect = WinHttpConnect(session, host_wide.c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return {};
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", L"/", nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return {};
    }

    if (!WinHttpAddRequestHeaders(request, headers_wide.c_str(),
                                  static_cast<DWORD>(headers_wide.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return {};
    }

    auto payload_wide = Utf16FromUtf8(payload);
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            const_cast<wchar_t*>(payload_wide.c_str()),
                            static_cast<DWORD>(payload_wide.size()),
                            static_cast<DWORD>(payload_wide.size()), 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return {};
    }

    // 读取响应
    std::string response;
    DWORD bytes_available = 0;
    while (WinHttpQueryDataAvailable(request, &bytes_available) && bytes_available > 0) {
        std::vector<char> buf(bytes_available);
        DWORD bytes_read = 0;
        if (WinHttpReadData(request, buf.data(), bytes_available, &bytes_read)) {
            response.append(buf.data(), bytes_read);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    return response;
}

std::string TencentAsrVocabClient::CreateVocab(const std::string& name,
                                                const std::vector<HotWordEntry>& words,
                                                const std::string& description) {
    std::ostringstream payload;
    payload << "{";
    payload << "\"Name\":\"" << name << "\"";
    if (!description.empty()) {
        // JSON 转义
        std::string escaped;
        for (char c : description) {
            if (c == '"') escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else escaped.push_back(c);
        }
        payload << ",\"Description\":\"" << escaped << "\"";
    }
    if (!words.empty()) {
        payload << ",\"WordWeights\":[";
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (i != 0) payload << ",";
            payload << "{\"Word\":\"" << words[i].word
                    << "\",\"Weight\":" << words[i].weight << "}";
        }
        payload << "]";
    }
    payload << "}";

    return CallApi("CreateAsrVocab", payload.str());
}

std::string TencentAsrVocabClient::UpdateVocab(const std::string& vocab_id,
                                                const std::vector<HotWordEntry>& words) {
    std::ostringstream payload;
    payload << "{";
    payload << "\"VocabId\":\"" << vocab_id << "\"";
    if (!words.empty()) {
        payload << ",\"WordWeights\":[";
        for (std::size_t i = 0; i < words.size(); ++i) {
            if (i != 0) payload << ",";
            payload << "{\"Word\":\"" << words[i].word
                    << "\",\"Weight\":" << words[i].weight << "}";
        }
        payload << "]";
    }
    payload << "}";

    return CallApi("UpdateAsrVocab", payload.str());
}

std::string TencentAsrVocabClient::FindVocabId(const std::string& name) {
    std::string payload = "{\"Limit\":30}";
    auto response = CallApi("GetAsrVocabList", payload);
    if (response.empty()) return {};

    auto root = cJSON_ParseWithLength(response.data(), response.size());
    if (!root) return {};

    std::string vocab_id;
    const auto* resp = cJSON_GetObjectItemCaseSensitive(root, "Response");
    if (resp) {
        const auto* list = cJSON_GetObjectItemCaseSensitive(resp, "VocabList");
        if (list && cJSON_IsArray(list)) {
            for (int i = 0; i < cJSON_GetArraySize(list); ++i) {
                const auto* item = cJSON_GetArrayItem(list, i);
                if (!item) continue;
                const auto* name_item = cJSON_GetObjectItemCaseSensitive(item, "Name");
                const auto* id_item = cJSON_GetObjectItemCaseSensitive(item, "VocabId");
                if (name_item && cJSON_IsString(name_item) && name == name_item->valuestring &&
                    id_item && cJSON_IsString(id_item)) {
                    vocab_id = id_item->valuestring;
                    break;
                }
            }
        }
    }
    cJSON_Delete(root);
    return vocab_id;
}

std::string TencentAsrVocabClient::SyncHotwords(const std::vector<std::string>& hotwords) {
    if (hotwords.empty()) return {};

    if (config_.tencent_secret_id.empty() || config_.tencent_secret_key.empty()) return {};

    // 构建 HotWordEntry 列表（权重默认 10）
    std::vector<HotWordEntry> entries;
    for (const auto& word : hotwords) {
        auto trimmed = word;
        // 去除首尾空格
        auto start = trimmed.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) continue;
        auto end = trimmed.find_last_not_of(" \t\n\r");
        trimmed = trimmed.substr(start, end - start + 1);
        if (trimmed.empty()) continue;
        // 限制词长
        if (trimmed.size() > 30) continue;
        entries.push_back({trimmed, 10});
    }
    if (entries.empty()) return {};

    // 1. 查找已有的 "VoiceStick-Hotwords" 热词表
    std::string vocab_id = FindVocabId(kDefaultVocabName);

    std::string response;
    if (!vocab_id.empty()) {
        // 热词表已存在，更新
        response = UpdateVocab(vocab_id, entries);
    } else {
        // 新建热词表
        response = CreateVocab(kDefaultVocabName, entries, "Voice Stick 自动管理热词表");
    }

    // 解析响应获取 VocabId
    if (!response.empty()) {
        auto root = cJSON_ParseWithLength(response.data(), response.size());
        if (root) {
            const auto* resp = cJSON_GetObjectItemCaseSensitive(root, "Response");
            if (resp) {
                const auto* id_item = cJSON_GetObjectItemCaseSensitive(resp, "VocabId");
                if (id_item && cJSON_IsString(id_item)) {
                    vocab_id = id_item->valuestring;
                }
                // UpdateAsrVocab 不返回 VocabId，使用请求中的 vocab_id
                const auto* req_id = cJSON_GetObjectItemCaseSensitive(resp, "RequestId");
                if (req_id && cJSON_IsString(req_id) && !vocab_id.empty()) {
                    // 更新成功，VocabId 不变
                }
            }
            cJSON_Delete(root);
        }
    }

    return vocab_id;
}

} // namespace voicestick
