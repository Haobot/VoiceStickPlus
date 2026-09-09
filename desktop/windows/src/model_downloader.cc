// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// model_downloader.h 的实现：WinHTTP 流式下载 + Range 断点续传 + BCrypt
// SHA-256 校验 + 原子改名，多源顺序回退。WinHTTP/BCrypt 配方沿用
// firmware_manifest.cc 已验证做法；差异在于大文件必须流式落盘（.part），
// 不能整包进内存。

#include "model_downloader.h"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include "log.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

namespace voicestick {

namespace {

// 磁盘预检余量：文件系统元数据与瞬时峰值的保守预留。
constexpr std::uint64_t kDiskMarginBytes = 64ull * 1024 * 1024;
// 单次 WinHttpReadData 的最大块（progress 粒度与内存占界的折中）。
constexpr DWORD kReadChunkBytes = 64 * 1024;
// progress 中间回调的最小间隔（首块与结束必发，不受节流）。
constexpr auto kProgressInterval = std::chrono::milliseconds(100);
// 超时：域名解析/连接 10s，发送/接收 30s（接收超时按块计，不约束总时长）。
constexpr int kResolveTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 30000;
constexpr int kReceiveTimeoutMs = 30000;

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

// URL 的 host/path 均为 ASCII（三个分发源都不是 IDN），直接窄化。
std::string NarrowAscii(std::wstring_view text) {
    std::string narrow;
    narrow.reserve(text.size());
    for (const wchar_t ch : text) {
        narrow.push_back(static_cast<char>(ch));
    }
    return narrow;
}

std::string ToLowerAscii(std::string text) {
    for (auto& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

// HINTERNET 的 RAII 包装：三个句柄（session/connect/request）在长下载循环
// 里有多个提前退出分支，手动 close 极易漏。
struct WinHttpHandle {
    HINTERNET handle = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : handle(h) {}
    ~WinHttpHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return handle; }
};

// 对整个文件流式 SHA-256（分块喂 BCrypt，不整读进内存）。失败时 ok=false。
std::string Sha256HexOfFile(const std::filesystem::path& path, bool& ok) {
    ok = false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<std::uint8_t, 32> digest{};
    bool good = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (good) {
        std::string buffer(kReadChunkBytes, '\0');
        for (;;) {
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = stream.gcount();
            if (got > 0 && BCryptHashData(hash,
                                          reinterpret_cast<PUCHAR>(buffer.data()),
                                          static_cast<ULONG>(got), 0) != 0) {
                good = false;
            }
            if (!stream || !good) break;  // eof（正常）或读坏/哈希坏
        }
        good = good && !stream.bad();
        if (good) good = BCryptFinishHash(hash, digest.data(), digest.size(), 0) == 0;
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!good) return {};
    char hex[65] = {};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    ok = true;
    return hex;
}

void DeletePartFile(const std::filesystem::path& part) {
    DeleteFileW(part.wstring().c_str());
}

// 单源尝试结果：主循环据此决定回退还是终止。
enum class AttemptStatus {
    kFinalized,      // 已哈希校验并改名到 dest
    kCancelled,
    kHashMismatch,   // .part 已删，主循环置零续传基数换源
    kNetworkFail,    // .part 保留（前缀仍有效），主循环重查大小换源续传
    kLocalIoError,   // 与源无关，直接终止
    kDiskFull,       // .part 已删，直接终止
};

struct AttemptResult {
    AttemptStatus status = AttemptStatus::kNetworkFail;
    std::string error;
};

// 从单个 URL 下载到 <dest>.part（可续传追加），完成后 finalize。host 为回环
// 地址时强制直连：机器上有系统代理时 WinHTTP 默认代理会把 127.0.0.1 也
// 带进代理，回环测试与本地缓存源会全挂。
AttemptResult AttemptSource(const std::string& url, const ModelFileSpec& spec,
                            const std::filesystem::path& dest,
                            const std::filesystem::path& part,
                            std::uint64_t& part_bytes,
                            const DownloadProgressFn& progress,
                            const DownloadCancelFn& cancelled) {
    AttemptResult result;
    ParsedModelUrl parsed;
    if (!ParseModelUrl(url, parsed)) {
        result.error = "invalid URL";
        return result;
    }
    const bool loopback = parsed.host == "127.0.0.1" || parsed.host == "localhost" ||
                          parsed.host == "::1";
    WinHttpHandle session(WinHttpOpen(
        L"VoiceStick/1.0",
        loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle) {
        result.error = "HTTP session open failed";
        return result;
    }
    WinHttpSetTimeouts(session.handle, kResolveTimeoutMs, kConnectTimeoutMs,
                       kSendTimeoutMs, kReceiveTimeoutMs);
    WinHttpHandle connect(WinHttpConnect(session.handle,
                                         Utf16FromUtf8(parsed.host).c_str(),
                                         parsed.port, 0));
    if (!connect.handle) {
        result.error = "connect failed";
        return result;
    }
    WinHttpHandle request(WinHttpOpenRequest(
        connect.handle, L"GET", Utf16FromUtf8(parsed.path).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parsed.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.handle) {
        result.error = "request open failed";
        return result;
    }

    const ResumePlan plan = PlanResume(part_bytes, spec.bytes);
    if (plan == ResumePlan::kResume) {
        const std::wstring range_header =
            L"Range: bytes=" + std::to_wstring(part_bytes) + L"-\r\n";
        WinHttpAddRequestHeaders(request.handle, range_header.c_str(),
                                 static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        result.error = "network request failed";
        return result;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        result.error = "no HTTP status";
        return result;
    }
    if (status != 200 && status != 206) {
        result.error = "HTTP " + std::to_string(status);
        return result;
    }

    bool has_content_length = false;
    std::uint64_t content_length = 0;
    wchar_t length_text[32] = {};
    DWORD length_size = sizeof(length_text);
    if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size,
                            WINHTTP_NO_HEADER_INDEX)) {
        wchar_t* parse_end = nullptr;
        const unsigned long long parsed_length =
            std::wcstoull(length_text, &parse_end, 10);
        if (parse_end != length_text) {
            has_content_length = true;
            content_length = parsed_length;
        }
    }

    RangeResponseInfo range_info;
    range_info.status = static_cast<int>(status);
    range_info.has_content_length = has_content_length;
    range_info.content_length = content_length;
    range_info.range_base = plan == ResumePlan::kResume ? part_bytes : 0;
    range_info.expected_bytes = spec.bytes;
    const RangeVerdict verdict = InterpretRangeResponse(range_info);
    if (verdict == RangeVerdict::kInvalid) {
        result.error = "inconsistent HTTP response";
        return result;
    }

    const bool append = verdict == RangeVerdict::kAppendToPart;
    FILE* part_file = _wfopen(part.wstring().c_str(), append ? L"ab" : L"wb");
    if (part_file == nullptr) {
        result.status = AttemptStatus::kLocalIoError;
        result.error = "cannot open part file";
        return result;
    }

    const std::uint64_t written_base = append ? part_bytes : 0;
    std::uint64_t received = 0;
    auto last_emit = std::chrono::steady_clock::time_point::min();
    const auto emit_progress = [&](bool force) {
        if (!progress) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_emit < kProgressInterval) return;
        last_emit = now;
        DownloadProgress p;
        p.downloaded = written_base + received;
        p.total = spec.bytes;
        progress(p);
    };
    emit_progress(true);  // 下载开始即反馈（含续传基数）

    bool network_ok = true;
    bool disk_full = false;
    bool io_ok = true;
    while (network_ok) {
        if (cancelled && cancelled()) {
            fclose(part_file);
            result.status = AttemptStatus::kCancelled;
            result.error = "cancelled";
            return result;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available)) {
            result.error = "network read failed";
            network_ok = false;
            break;
        }
        if (available == 0) break;  // 正文结束
        const DWORD want = available < kReadChunkBytes ? available : kReadChunkBytes;
        // 服务器给的比清单多：前缀可信但必然哈希不过，提前止损换源。
        if (written_base + received + want > spec.bytes) {
            result.error = "response body oversized";
            network_ok = false;
            break;
        }
        std::string buffer(want, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(request.handle, buffer.data(), want, &got)) {
            result.error = "network read failed";
            network_ok = false;
            break;
        }
        if (got == 0) break;
        if (fwrite(buffer.data(), 1, got, part_file) != got) {
            disk_full = errno == ENOSPC;
            io_ok = false;
            break;
        }
        received += got;
        emit_progress(false);
    }
    fclose(part_file);

    if (!io_ok) {
        if (disk_full) {
            DeletePartFile(part);
            result.status = AttemptStatus::kDiskFull;
        } else {
            result.status = AttemptStatus::kLocalIoError;
        }
        result.error = disk_full ? "disk full while writing" : "part write failed";
        return result;
    }
    if (!network_ok) {
        return result;  // .part 前缀保留，主循环换源续传
    }
    if (written_base + received != spec.bytes) {
        // 截断：无续传价值，删掉换源整下。
        DeletePartFile(part);
        part_bytes = 0;
        result.error = "truncated body";
        return result;
    }
    emit_progress(true);  // 结束必发

    const DownloadResult finalize = FinalizePartFile(dest, spec.sha256);
    if (finalize == DownloadResult::kOk) {
        result.status = AttemptStatus::kFinalized;
        return result;
    }
    if (finalize == DownloadResult::kLocalIoError) {
        result.status = AttemptStatus::kLocalIoError;
        result.error = "finalize rename failed";
        return result;
    }
    result.status = AttemptStatus::kHashMismatch;
    result.error = "checksum mismatch";
    return result;
}

}  // namespace

bool ParseModelUrl(const std::string& url, ParsedModelUrl& out) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    const std::wstring wide = Utf16FromUtf8(url);
    if (wide.empty() || url.find("://") == std::string::npos) return false;
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)) {
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP &&
        parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }
    if (parts.dwHostNameLength == 0) return false;
    out.host = NarrowAscii(std::wstring_view(parts.lpszHostName, parts.dwHostNameLength));
    out.path = NarrowAscii(std::wstring_view(parts.lpszUrlPath, parts.dwUrlPathLength));
    out.path.append(NarrowAscii(std::wstring_view(parts.lpszExtraInfo, parts.dwExtraInfoLength)));
    out.port = static_cast<std::uint16_t>(parts.nPort);
    out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

ResumePlan PlanResume(std::uint64_t part_bytes, std::uint64_t expected_bytes) {
    if (part_bytes == 0) return ResumePlan::kFreshStart;
    if (part_bytes < expected_bytes) return ResumePlan::kResume;
    return ResumePlan::kCorruptRestart;
}

RangeVerdict InterpretRangeResponse(const RangeResponseInfo& info) {
    if (info.status == 200) {
        if (info.has_content_length && info.content_length != info.expected_bytes) {
            return RangeVerdict::kInvalid;
        }
        return RangeVerdict::kOverwritePart;
    }
    if (info.status == 206) {
        if (info.range_base == 0 || info.range_base >= info.expected_bytes) {
            return RangeVerdict::kInvalid;
        }
        if (info.has_content_length &&
            info.content_length != info.expected_bytes - info.range_base) {
            return RangeVerdict::kInvalid;
        }
        return RangeVerdict::kAppendToPart;
    }
    return RangeVerdict::kInvalid;
}

DownloadResult FinalizePartFile(const std::filesystem::path& dest,
                                const std::string& expected_sha256) {
    std::filesystem::path part = dest;
    part += ".part";
    std::error_code ec;
    if (!std::filesystem::exists(part, ec)) {
        return DownloadResult::kHashMismatch;
    }
    bool hash_ok = false;
    const std::string digest = Sha256HexOfFile(part, hash_ok);
    if (!hash_ok || digest != ToLowerAscii(expected_sha256)) {
        DeletePartFile(part);
        return DownloadResult::kHashMismatch;
    }
    if (!MoveFileExW(part.wstring().c_str(), dest.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return DownloadResult::kLocalIoError;
    }
    return DownloadResult::kOk;
}

std::uint64_t RequiredDiskBytes(const std::vector<ModelFileSpec>& files) {
    std::uint64_t total = 0;
    for (const auto& file : files) {
        total += file.bytes;
    }
    return total;
}

DownloadOutcome ModelDownloader::DownloadFile(
    const ModelFileSpec& spec, const std::filesystem::path& dest,
    const DownloadProgressFn& progress, const DownloadCancelFn& cancelled) {
    DownloadOutcome outcome;
    if (spec.bytes == 0 || spec.sha256.size() != 64 || spec.urls.empty()) {
        outcome.result = DownloadResult::kInvalidSpec;
        outcome.error = "invalid model spec";
        return outcome;
    }
    const std::filesystem::path parent = dest.parent_path();
    std::error_code ec;
    if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
        outcome.result = DownloadResult::kLocalIoError;
        outcome.error = "destination directory does not exist";
        return outcome;
    }
    std::filesystem::path part = dest;
    part += ".part";

    std::uint64_t part_bytes =
        std::filesystem::exists(part, ec) ? std::filesystem::file_size(part, ec) : 0;
    if (ec) part_bytes = 0;
    // 预检需求 = 尚缺字节 + 余量（已有 .part 可抵扣）。
    const std::uint64_t need = spec.bytes -
                                   std::min<std::uint64_t>(part_bytes, spec.bytes) +
                               kDiskMarginBytes;
    ULARGE_INTEGER free_bytes{};
    const std::wstring space_dir = parent.empty() ? L"." : parent.wstring();
    if (GetDiskFreeSpaceExW(space_dir.c_str(), &free_bytes, nullptr, nullptr) &&
        free_bytes.QuadPart < need) {
        outcome.result = DownloadResult::kDiskSpaceInsufficient;
        outcome.error = "insufficient disk space";
        return outcome;
    }

    std::vector<std::string> failures;
    bool saw_hash_mismatch = false;
    for (const auto& url : spec.urls) {
        if (cancelled && cancelled()) {
            outcome.result = DownloadResult::kCancelled;
            return outcome;
        }
        AttemptResult attempt =
            AttemptSource(url, spec, dest, part, part_bytes, progress, cancelled);
        switch (attempt.status) {
            case AttemptStatus::kFinalized:
                Log("MDL", "model file verified: " + spec.rel_path +
                              " (" + std::to_string(spec.bytes) + " bytes) via " + url);
                outcome.result = DownloadResult::kOk;
                outcome.url_used = url;
                return outcome;
            case AttemptStatus::kCancelled:
                outcome.result = DownloadResult::kCancelled;
                return outcome;
            case AttemptStatus::kLocalIoError:
            case AttemptStatus::kDiskFull:
                // 与源无关的本地失败：换源无意义，直接终止。
                outcome.result = attempt.status == AttemptStatus::kDiskFull
                                     ? DownloadResult::kDiskFull
                                     : DownloadResult::kLocalIoError;
                outcome.error = attempt.error;
                return outcome;
            case AttemptStatus::kHashMismatch:
                part_bytes = 0;  // .part 已删
                saw_hash_mismatch = true;
                break;
            case AttemptStatus::kNetworkFail:
                // .part 可能追加了有效前缀，重查大小供下一源续传。
                part_bytes = std::filesystem::exists(part, ec)
                                 ? std::filesystem::file_size(part, ec)
                                 : 0;
                if (ec) part_bytes = 0;
                break;
        }
        failures.push_back(url + ": " + attempt.error);
        Log("MDL", "source failed, trying next: " + url + " (" + attempt.error + ")");
    }

    outcome.result = saw_hash_mismatch ? DownloadResult::kHashMismatch
                                       : DownloadResult::kNetworkError;
    for (const auto& failure : failures) {
        if (!outcome.error.empty()) outcome.error += "\n";
        outcome.error += failure;
    }
    return outcome;
}

}  // namespace voicestick
