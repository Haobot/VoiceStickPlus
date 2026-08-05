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

} // namespace

void Log(std::string_view category, std::string_view message) {
    std::lock_guard lock(g_log_mutex);
    try {
        const auto path = LogFilePath();
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::app);
        output << "[" << category << " " << CurrentTimestamp() << "] " << message << "\n";
    } catch (...) {
    }
}

} // namespace voicestick
