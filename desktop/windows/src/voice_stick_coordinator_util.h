#pragma once
// N7 cut2：voice_stick_coordinator.cc 拆分共享层——尾段（热词/看门狗/粘贴域）
// 方法所用的文件级助手迁入本头（匿名 ns 每 TU 内部链接零 ODR）；preamble 逐字
// 沿用原文件顶部 include 块。
#include "voice_stick_coordinator.h"

#include "encoder_speed.h"
#include "localization.h"
#include "log.h"
#include "xiaomi_buttons.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <tuple>
#include <utility>

namespace voicestick {
namespace {
void LogCoordinatorLine(const std::string& message) {
    LogCoordinator(message);
}

bool IsFirmwareManifestCompatible(const DeviceFirmwareInfo& info, const FirmwareManifest& manifest) {
    return IsFirmwareHardwareCompatible(info.hardware, info.current_version, manifest.hardware);
}

// steady_clock 毫秒时间戳（watchdog 活动时间用，进程内自洽即可，不需要绝对 epoch）。
std::int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}
} // namespace voicestick
