// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 本地模型下载器（Doc/Plan/local-model-distribution.md §5）：多源顺序回退 +
// WinHTTP 流式落盘 + Range 断点续传 + BCrypt SHA-256 校验 + 原子改名。
// 纯函数（ParseModelUrl / PlanResume / InterpretRangeResponse /
// FinalizePartFile / RequiredDiskBytes）与 WinHTTP 实现共用本头文件，
// 纯函数不碰网络，core_tests.cc 直接驱动。

#ifndef VOICESTICK_MODEL_DOWNLOADER_H_
#define VOICESTICK_MODEL_DOWNLOADER_H_

#include "model_manifest.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace voicestick {

// 下载进度：downloaded 含续传基数，total = 期望总字节。
struct DownloadProgress {
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
};
using DownloadProgressFn = std::function<void(const DownloadProgress&)>;
// 取消轮询：返回 true 时下载在当前块后停止，.part 保留供续传。
using DownloadCancelFn = std::function<bool()>;

enum class DownloadResult {
    kOk,
    kCancelled,
    // 全部源失败：网络错误 / HTTP 非 2xx / 响应体与清单不自洽。
    kNetworkError,
    // 全部源哈希不匹配（每次失败即删 .part，防占盘防污染）。
    kHashMismatch,
    kDiskFull,
    // 预检不足：期望体积 - 已有 .part + 64MB 余量。
    kDiskSpaceInsufficient,
    // 清单字段缺失/畸形。
    kInvalidSpec,
    // 本地 I/O 失败：目录不存在、写盘错误（非磁盘满）、改名失败。
    kLocalIoError,
};

struct DownloadOutcome {
    DownloadResult result = DownloadResult::kOk;
    // 失败时为各源失败原因汇总；成功时空。
    std::string error;
    // 成功时记录实际命中的源 URL（诊断/统计用）。
    std::string url_used;
};

// WinHttpCrackUrl 的轻量包装结果（host 保持 ASCII；三个源均非 IDN）。
struct ParsedModelUrl {
    std::string host;
    std::string path;  // 含 query
    std::uint16_t port = 0;
    bool secure = false;
};

// 解析下载 URL：非 http(s) / 非法格式返回 false。
bool ParseModelUrl(const std::string& url, ParsedModelUrl& out);

// .part 续传决策：part == 0 全新下载；0 < part < expected 续传；
// part >= expected 视为损坏（哈希必不匹配）从头重下。
enum class ResumePlan { kFreshStart, kResume, kCorruptRestart };
ResumePlan PlanResume(std::uint64_t part_bytes, std::uint64_t expected_bytes);

// HTTP 响应体处理模式判定的输入。
struct RangeResponseInfo {
    int status = 0;  // 实际只应为 200 / 206，其余由调用方换源
    bool has_content_length = false;
    std::uint64_t content_length = 0;
    std::uint64_t range_base = 0;  // 请求携带的 Range 基数，0 = 未携带
    std::uint64_t expected_bytes = 0;
};
enum class RangeVerdict { kAppendToPart, kOverwritePart, kInvalid };
// 206 须与 Range 基数自洽（CL 已知时 == expected - base，未知则信任连接
// 边界）；200 表示服务器无视 Range 须整文件重下（CL 已知时须 == expected）。
// 不自洽返回 kInvalid（换源）；完整性最终由哈希兜底。
RangeVerdict InterpretRangeResponse(const RangeResponseInfo& info);

// .part 收尾：对 <dest>.part 全量流式哈希，匹配则原子改名到 dest 返回 kOk；
// 不匹配或 .part 不存在则删除 .part 返回 kHashMismatch；改名失败
// kLocalIoError（.part 保留）。
DownloadResult FinalizePartFile(const std::filesystem::path& dest,
                                const std::string& expected_sha256);

// 向导磁盘预检：文件期望体积合计。
std::uint64_t RequiredDiskBytes(const std::vector<ModelFileSpec>& files);

class ModelDownloader {
 public:
    // 多源顺序下载单文件到 dest（先写 <dest>.part，逐源回退）。progress 每块
    // 回调（首块与结束必发，中间 >=100ms 节流）；cancelled 每块轮询。任一源
    // 完整下载且哈希匹配即成功返回。单线程调用（向导后台线程）。
    DownloadOutcome DownloadFile(const ModelFileSpec& spec,
                                 const std::filesystem::path& dest,
                                 const DownloadProgressFn& progress = {},
                                 const DownloadCancelFn& cancelled = {});
};

}  // namespace voicestick

#endif  // VOICESTICK_MODEL_DOWNLOADER_H_
