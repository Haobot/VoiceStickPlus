#include "asr_client_win.h"

#include "asr_protocol.h"

#include <optional>
#include <string>

namespace voicestick {

namespace {

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

} // namespace

AsrClientWin::AsrClientWin(AppConfig config) : config_(std::move(config)) {}

AsrClientWin::~AsrClientWin() {
    Cancel();
}

bool AsrClientWin::Start() {
    if (config_.ActiveApiKey().empty()) {
        if (on_error) on_error("Missing ASR API key");
        return false;
    }
    Cancel();
    cancelled_ = false;
    worker_ = std::thread([this] { RunWebSocket(); });
    return true;
}

void AsrClientWin::SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) {
    auto frame = AsrProtocol::MakeAudioFrame(data, is_last);
    std::lock_guard lock(mutex_);
    if (websocket_) {
        SendFrame(websocket_, frame);
    } else {
        queued_frames_.push_back(std::move(frame));
    }
}

void AsrClientWin::Cancel() {
    cancelled_ = true;
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
        } else {
            worker_.join();
        }
    }
    std::lock_guard lock(mutex_);
    queued_frames_.clear();
    websocket_ = nullptr;
}

void AsrClientWin::RunWebSocket() {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    const auto url = WinHttpUrlFromWebSocketUrl(config_.ActiveWebsocketUrl());
    if (!url.has_value() || !WinHttpCrackUrl(url->c_str(), 0, 0, &components)) {
        if (on_error) on_error("Invalid ASR URL");
        return;
    }

    HINTERNET session = WinHttpOpen(L"VoiceStick/Windows", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (on_error) on_error("Failed to start ASR network session: " + LastErrorText());
        return;
    }
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connect) {
        CloseHandles(session, nullptr, nullptr, nullptr);
        if (on_error) on_error("Failed to connect ASR host: " + LastErrorText());
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
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path_and_query.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        CloseHandles(session, connect, request, nullptr);
        if (on_error) on_error("Failed to create ASR request: " + LastErrorText());
        return;
    }

    AddHeader(request, "X-Api-Key", config_.ActiveApiKey());
    AddHeader(request, "X-Api-Request-Id", "voice-stick-windows");
    AddHeader(request, "X-Api-Sequence", "-1");
    if (config_.asr_provider == AsrProvider::kVolcengine) {
        AddHeader(request, "X-Api-Resource-Id", config_.resource_id);
    }

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        CloseHandles(session, connect, request, nullptr);
        if (on_error) on_error("Failed to prepare ASR WebSocket upgrade");
        return;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        CloseHandles(session, connect, request, nullptr);
        if (on_error) on_error("ASR WebSocket handshake failed");
        return;
    }

    HINTERNET websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!websocket) {
        const auto status_code = QueryStatusCode(request);
        CloseHandles(session, connect, request, nullptr);
        if (on_error) on_error(status_code.empty()
                               ? "ASR WebSocket upgrade failed"
                               : "ASR WebSocket upgrade failed: HTTP " + status_code);
        return;
    }
    WinHttpCloseHandle(request);
    request = nullptr;

    auto client_frame = AsrProtocol::MakeClientRequestFrame(config_);
    SendFrame(websocket, client_frame);
    {
        std::lock_guard lock(mutex_);
        websocket_ = websocket;
    }
    FlushQueuedFrames(websocket);
    while (!cancelled_) {
        ReceiveOne(websocket);
    }
    {
        std::lock_guard lock(mutex_);
        if (websocket_ == websocket) websocket_ = nullptr;
    }
    CloseHandles(session, connect, request, websocket);
}

void AsrClientWin::FlushQueuedFrames(HINTERNET websocket) {
    std::vector<ByteVector> frames;
    {
        std::lock_guard lock(mutex_);
        frames.swap(queued_frames_);
    }
    for (const auto& frame : frames) {
        SendFrame(websocket, frame);
    }
}

bool AsrClientWin::SendFrame(HINTERNET websocket, const ByteVector& frame) {
    return WinHttpWebSocketSend(websocket,
                                WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                const_cast<std::uint8_t*>(frame.data()),
                                static_cast<DWORD>(frame.size())) == ERROR_SUCCESS;
}

void AsrClientWin::ReceiveOne(HINTERNET websocket) {
    std::array<std::uint8_t, 64 * 1024> buffer{};
    DWORD bytes_read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
    const DWORD result = WinHttpWebSocketReceive(websocket, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()),
                                                 &bytes_read, &type);
    if (result != ERROR_SUCCESS || bytes_read == 0) return;
    if (type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE &&
        type != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
        return;
    }
    auto response = AsrProtocol::ParseResponse(std::span(buffer.data(), bytes_read));
    if (!response.has_value()) return;
    if (response->is_error) {
        if (on_error) on_error(response->text);
        if (response->upgrade_url && on_upgrade_url) on_upgrade_url(*response->upgrade_url);
    } else if (response->is_final) {
        if (on_final) on_final(response->text);
    } else if (!response->text.empty()) {
        if (on_partial) on_partial(response->text);
    }
}

void AsrClientWin::AddHeader(HINTERNET request, std::string_view name, std::string_view value) {
    const auto header = Utf16FromUtf8(std::string(name) + ": " + std::string(value) + "\r\n");
    WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(header.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
}

std::string AsrClientWin::QueryStatusCode(HINTERNET request) {
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

std::string AsrClientWin::LastErrorText() {
    return std::to_string(GetLastError());
}

void AsrClientWin::CloseHandles(HINTERNET session, HINTERNET connect,
                                HINTERNET request, HINTERNET websocket) {
    if (websocket) WinHttpCloseHandle(websocket);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
}

} // namespace voicestick
