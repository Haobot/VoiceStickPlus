#include "log.h"

#include "app_config.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace voicestick {

namespace {

// 日志写入互斥锁：Log 被多线程并发调用（主线程、ASR 回调线程、精修/热词后台线程、
// BLE 线程等），ofstream 无锁并发写同一文件是未定义行为，曾致集成测试 SegFault。
// 进程内一把锁即可，日志量小不成为瓶颈。
std::mutex g_log_mutex;

std::string CurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count() % 1000;
    std::tm tm_buf{};
    localtime_s(&tm_buf, &time_t_now);
    std::ostringstream out;
    out << std::put_time(&tm_buf, "%H:%M:%S") << "."
        << std::setw(3) << std::setfill('0') << millis;
    return out.str();
}

std::filesystem::path LogFilePath() {
    return AppConfig::DefaultDebugAudioDirectory().parent_path() / "VoiceStickApp.log";
}

// C8：单文件上限 8MB，超限轮转为 <path>.old（只留一代历史）。
constexpr std::uintmax_t kMaxLogBytes = 8ull * 1024 * 1024;

} // namespace

void RotateLogIfTooLarge(const std::filesystem::path& path, std::uintmax_t max_bytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size <= max_bytes) return;
    std::filesystem::path old_path = path;
    old_path += ".old";
    // 先清上一代（rename 对已存在目标的行为随实现而异），再改名；任一步失败都
    // 不动原文件，下一次写入继续追加。
    std::error_code remove_ec;
    std::filesystem::remove(old_path, remove_ec);
    std::error_code rename_ec;
    std::filesystem::rename(path, old_path, rename_ec);
}

namespace {
void LogUnlocked(std::string_view category, std::string_view message) {
    try {
        const auto path = LogFilePath();
        std::filesystem::create_directories(path.parent_path());
        // C8：超限先轮转再追加（在写锁内，避免与并发写交错）。
        RotateLogIfTooLarge(path, kMaxLogBytes);
        std::ofstream output(path, std::ios::app);
        output << "[" << category << " " << CurrentTimestamp() << "] " << message << "\n";
    } catch (...) {
    }
}
} // namespace

void Log(std::string_view category, std::string_view message) {
    std::lock_guard lock(g_log_mutex);
    LogUnlocked(category, message);
}

void LogNonBlocking(std::string_view category, std::string_view message) {
    std::unique_lock<std::mutex> lock(g_log_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;  // B15：钩子路径宁丢一条不等锁。
    LogUnlocked(category, message);
}

} // namespace voicestick
