#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace voicestick {

void Log(std::string_view category, std::string_view message);
// B15：低级键盘钩子路径专用——try_lock 落盘，抢不到锁立即丢弃该条诊断。
// 钩子线程绝不等写锁（锁被普通日志持有+磁盘慢时会超 LowLevelHooksTimeout 致钩子被系统静默移除）。
void LogNonBlocking(std::string_view category, std::string_view message);

// C8：日志轮转——文件超过 max_bytes 时把当前日志改名为 <path>.old（覆盖上一份），
// 新写入从空文件重新开始；只保留一代历史，防止单文件无限增长撑爆磁盘。
// 轮转失败一律静默（保留原文件继续追加：宁可不轮转也不丢日志）。
// 由 Log() 持写锁调用；单独暴露以便单测覆盖。
void RotateLogIfTooLarge(const std::filesystem::path& path, std::uintmax_t max_bytes);

inline void LogApp(std::string_view message) { Log("APP", message); }
inline void LogAppNonBlocking(std::string_view message) { LogNonBlocking("APP", message); }
inline void LogBle(std::string_view message) { Log("BLE", message); }
inline void LogCoordinator(std::string_view message) { Log("CRD", message); }
inline void LogHotkey(std::string_view message) { Log("HKY", message); }

} // namespace voicestick
