#include "llm_chat_client.h"

#include "cJSON.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <thread>

namespace voicestick {

namespace {

std::string Trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

bool StartsWithScheme(std::string_view text, std::string_view scheme) {
    return text.size() >= scheme.size() &&
           std::equal(scheme.begin(), scheme.end(), text.begin(), [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs)) ==
                      std::tolower(static_cast<unsigned char>(rhs));
           });
}

void AddHeader(HINTERNET request, const std::string& header) {
    const auto wide = LLMChatClient::Utf16FromUtf8(header + "\r\n");
    WinHttpAddRequestHeaders(request, wide.c_str(), static_cast<DWORD>(wide.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
}

} // namespace

std::string LLMChatClient::ChatSync(const std::string& system_prompt,
                                    const std::string& user_text,
                                    std::string* error) const {
    const auto api_key = config_.ActiveLlmApiKey();
    if (api_key.empty()) {
        *error = "Missing LLM API key";
        return {};
    }

    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    const auto path = ChatCompletionsPathAndQuery(&host, &port, &secure, error);
    if (!error->empty()) return {};

    HINTERNET session = WinHttpOpen(L"VoiceStick/Windows",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        *error = "Failed to start LLM network session: " + LastErrorText();
        return {};
    }
    // 非流式一次性请求：DeepSeek 等推理模型 TTFT 偶发超过 10s，
    // receive 超时给 30s 避免慢响应被误杀（提炼/精修回退都走这条路）。
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 30000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        *error = "Failed to connect LLM host: " + LastErrorText();
        return {};
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    const auto path_w = Utf16FromUtf8(path);
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path_w.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        *error = "Failed to create LLM request: " + LastErrorText();
        return {};
    }
    WinHttpSetTimeouts(request, 5000, 5000, 10000, 30000);
    AddHeader(request, "Authorization: Bearer " + api_key);
    AddHeader(request, "Content-Type: application/json");

    const auto payload = BuildChatPayload(config_.ActiveLlmModel(), system_prompt, user_text,
                                          /*stream=*/false, config_.llm_disable_thinking);

    const BOOL sent = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        const_cast<char*>(payload.data()),
        static_cast<DWORD>(payload.size()),
        static_cast<DWORD>(payload.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        *error = "LLM request failed: " + LastErrorText();
        return {};
    }

    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        chunk.resize(read);
        body += chunk;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    auto* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!root) {
        *error = "Invalid LLM response";
        return {};
    }
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    auto* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    auto* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    auto* message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : nullptr;
    auto* content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : nullptr;
    if (!cJSON_IsString(content) || content->valuestring == nullptr) {
        *error = "Invalid LLM response";
        return {};
    }
    return Trim(content->valuestring);
}

void LLMChatClient::ChatAsync(std::string system_prompt,
                              std::string user_text,
                              std::function<void(bool, std::string)> completion) const {
    auto config = config_;
    std::thread([config = std::move(config),
                 system_prompt = std::move(system_prompt),
                 user_text = std::move(user_text),
                 completion = std::move(completion)]() mutable {
        LLMChatClient client(std::move(config));
        std::string error;
        auto result = client.ChatSync(system_prompt, user_text, &error);
        if (!error.empty()) {
            completion(false, error);
        } else {
            completion(true, result);
        }
    }).detach();
}

void LLMChatClient::ChatStream(std::string system_prompt,
                               std::string user_text,
                               StreamCallbacks callbacks,
                               std::shared_ptr<std::atomic_bool> cancel) const {
    const auto api_key = config_.ActiveLlmApiKey();
    if (api_key.empty()) {
        if (callbacks.on_error) callbacks.on_error("Missing LLM API key");
        return;
    }

    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    std::string error;
    const auto path = ChatCompletionsPathAndQuery(&host, &port, &secure, &error);
    if (!error.empty()) {
        if (callbacks.on_error) callbacks.on_error(error);
        return;
    }

    OutputDebugStringA(("[ChatStream] connecting to " + Utf8FromUtf16(host) + ":" + std::to_string(port) + path).c_str());

    HINTERNET session = WinHttpOpen(L"VoiceStick/Windows",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        if (callbacks.on_error) callbacks.on_error("Failed to start LLM network session: " + LastErrorText());
        return;
    }
    // 流式连接超时更宽松：resolve 5s, connect 5s, send 30s, receive 60s
    WinHttpSetTimeouts(session, 5000, 5000, 30000, 60000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        if (callbacks.on_error) callbacks.on_error("Failed to connect LLM host: " + LastErrorText());
        return;
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    const auto path_w = Utf16FromUtf8(path);
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path_w.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (callbacks.on_error) callbacks.on_error("Failed to create LLM request: " + LastErrorText());
        return;
    }
    WinHttpSetTimeouts(request, 5000, 5000, 30000, 60000);
    AddHeader(request, "Authorization: Bearer " + api_key);
    AddHeader(request, "Content-Type: application/json");

    const auto payload = BuildChatPayload(config_.ActiveLlmModel(), system_prompt, user_text,
                                          /*stream=*/true, config_.llm_disable_thinking);

    OutputDebugStringA(("[ChatStream] sending stream request, model=" + config_.ActiveLlmModel() + ", payload=" + std::to_string(payload.size()) + " bytes").c_str());

    const BOOL sent = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        const_cast<char*>(payload.data()),
        static_cast<DWORD>(payload.size()),
        static_cast<DWORD>(payload.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        const auto err = LastErrorText();
        OutputDebugStringA(("[ChatStream] WinHttpSendRequest/ReceiveResponse failed: " + err).c_str());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (callbacks.on_error) callbacks.on_error("LLM stream request failed: " + err);
        return;
    }

    // 验证 HTTP 状态码
    DWORD status_code = 0;
    DWORD status_code_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_code_size,
                             WINHTTP_NO_HEADER_INDEX) ||
        status_code != 200) {
        // 非 200 时读取错误响应体
        std::string error_body;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            chunk.resize(read);
            error_body += chunk;
        }
        OutputDebugStringA(("[ChatStream] HTTP " + std::to_string(status_code) + ": " + error_body).c_str());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (callbacks.on_error) {
            callbacks.on_error("LLM stream HTTP " + std::to_string(status_code) +
                               (error_body.empty() ? std::string() : ": " + error_body));
        }
        return;
    }

    OutputDebugStringA("[ChatStream] HTTP 200, reading SSE stream...");

    // 增量读取 SSE 事件流
    std::string line_buffer;
    std::string full_text;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        if (cancel && cancel->load()) {
            OutputDebugStringA("[ChatStream] cancelled");
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        chunk.resize(read);
        line_buffer += chunk;

        // 按 \n 拆行处理
        std::size_t pos = 0;
        while (pos < line_buffer.size()) {
            const auto nl = line_buffer.find('\n', pos);
            if (nl == std::string::npos) {
                if (pos > 0) line_buffer.erase(0, pos);
                break;
            }
            std::string line = line_buffer.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pos = nl + 1;

            if (line.empty()) continue;
            if (line[0] == ':') continue;

            bool is_done = false;
            std::string token = ParseSseLine(line, &is_done);
            if (is_done) {
                OutputDebugStringA(("[ChatStream] [DONE] received, total tokens=" + std::to_string(full_text.size())).c_str());
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                if (callbacks.on_done) callbacks.on_done(full_text);
                return;
            }
            if (!token.empty()) {
                full_text += token;
                if (callbacks.on_token) callbacks.on_token(token);
            }
        }
        if (pos >= line_buffer.size()) line_buffer.clear();
    }

    // 循环退出：可能超时或连接关闭
    const auto exit_err = LastErrorText();
    OutputDebugStringA(("[ChatStream] stream loop exited, full_text=" + std::to_string(full_text.size()) +
                        " bytes, WinHttp err=" + exit_err).c_str());
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!full_text.empty()) {
        if (callbacks.on_done) callbacks.on_done(full_text);
    } else {
        if (callbacks.on_error) callbacks.on_error("LLM stream returned no content (err=" + exit_err + ")");
    }
}

std::string LLMChatClient::ChatCompletionsPathAndQuery(std::wstring* host,
                                                       INTERNET_PORT* port,
                                                       bool* secure,
                                                       std::string* error) const {
    auto base = config_.ActiveLlmBaseUrl();
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) {
        *error = "Invalid LLM base URL";
        return {};
    }
    const auto url = base.ends_with("/chat/completions") ? base : base + "/chat/completions";
    const auto http_url = StartsWithScheme(url, "http://") || StartsWithScheme(url, "https://")
                              ? url
                              : "https://" + url;
    const auto wide = Utf16FromUtf8(http_url);
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wide.empty() || !WinHttpCrackUrl(wide.c_str(), 0, 0, &components)) {
        *error = "Invalid LLM base URL";
        return {};
    }
    *host = std::wstring(components.lpszHostName, components.dwHostNameLength);
    *port = components.nPort;
    *secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring path;
    if (components.lpszUrlPath && components.dwUrlPathLength > 0) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo && components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/chat/completions";
    return Utf8FromUtf16(path);
}

std::string LLMChatClient::BuildChatPayload(const std::string& model,
                                            const std::string& system_prompt,
                                            const std::string& user_text,
                                            bool stream,
                                            bool disable_thinking) {
    std::string payload = "{\"model\":\"" + JsonEscape(model) + "\","
                          "\"temperature\":0";
    if (disable_thinking) {
        // 两种风格都发：顶层 enable_thinking（DashScope/Qwen 兼容模式）+
        // chat_template_kwargs.enable_thinking（vLLM/SGLang 自建端点），关闭深度思考以快速输出。
        payload += ",\"enable_thinking\":false,"
                   "\"chat_template_kwargs\":{\"enable_thinking\":false}";
    }
    // DeepSeek V4 系列（deepseek-v4-flash / deepseek-v4-pro）思考模式默认开启且
    // effort 默认为 high，精修/翻译/热词提炼等后处理任务不需要思维链，开启会显著
    // 拖慢 TTFT。检测到 deepseek 模型名时显式关闭思考模式；OpenAI 兼容协议对
    // 未知字段通常静默忽略，非 DeepSeek 端点不受影响。大小写不敏感匹配，兼容
    // "DeepSeek-V4-Flash" / "deepseek-v4-pro" 等不同写法。
    std::string model_lower = model;
    std::transform(model_lower.begin(), model_lower.end(), model_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (model_lower.find("deepseek") != std::string::npos) {
        payload += ",\"thinking\":{\"type\":\"disabled\"}";
    }
    if (stream) {
        payload += ",\"stream\":true";
    }
    payload += ",\"messages\":["
               "{\"role\":\"system\",\"content\":\"" + JsonEscape(system_prompt) + "\"},"
               "{\"role\":\"user\",\"content\":\"" + JsonEscape(user_text) + "\"}"
               "]}";
    return payload;
}

std::string LLMChatClient::JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::wstring LLMChatClient::Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string LLMChatClient::Utf8FromUtf16(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::string LLMChatClient::ParseSseLine(const std::string& line, bool* is_done) {
    *is_done = false;
    // 仅处理 "data: " 前缀的行
    if (line.size() < 6 || line[0] != 'd' || line[1] != 'a' ||
        line[2] != 't' || line[3] != 'a' || line[4] != ':') {
        return {};
    }
    std::string data = line.substr(5);
    // 跳过 data: 后的前导空格
    while (!data.empty() && data[0] == ' ') data.erase(0, 1);

    if (data == "[DONE]") {
        *is_done = true;
        return {};
    }

    // 解析 JSON: {"choices":[{"delta":{"content":"..."},"finish_reason":...}]}
    auto* root = cJSON_ParseWithLength(data.data(), data.size());
    if (!root) return {};
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);

    auto* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices)) return {};
    auto* first = cJSON_GetArrayItem(choices, 0);
    if (!first) return {};
    auto* delta = cJSON_GetObjectItemCaseSensitive(first, "delta");
    if (!delta) return {};
    auto* content = cJSON_GetObjectItemCaseSensitive(delta, "content");
    if (!cJSON_IsString(content) || content->valuestring == nullptr) return {};

    return content->valuestring;
}

std::string LLMChatClient::LastErrorText() {
    return std::to_string(GetLastError());
}

} // namespace voicestick
