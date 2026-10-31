#pragma once

#include "license.h"

#include <string>
#include <vector>

namespace voicestick {

class AppConfig;
class VoiceStickCoordinator;

// 离线授权运行时（app shell，Doc/Plan/offline-license-activation.md）：
// 装配 device_ids + MachineGuid + config 供 EvaluateLicense；试用锚点/last_seen
// 持久化。config 以指针持有——指向 Win32App 值成员，move 赋值不改变对象地址，
// 不悬垂（区别于引用持有）。
class LicenseRuntime {
public:
    LicenseRuntime(AppConfig* config, VoiceStickCoordinator* coordinator);

    // 当前授权状态（kTrial/kActive/kExpired）。EvaluateLicense 需要 trial_anchor：
    // 无锚点时返回满试用期（30 天），由 EnsureTrialAnchor 负责立即落锚。
    LicenseStatus CurrentStatus() const;
    // local_asr.enabled 且无试用锚点时写锚点并保存。
    void EnsureTrialAnchor();
    // now > last_seen 时推进并保存（防时钟回拨基准）；保存失败仅记日志。
    void AdvanceLastSeen();
    // 本地引擎放行：状态为 kTrial/kActive。
    bool LocalAsrAllowed() const;
    // 记录当前状态一行（启动/状态变更时调用；不在按键路径上，避免日志噪音）。
    void LogStatus(const char* reason) const;

private:
    std::vector<std::string> NormalizedDeviceIds() const;
    void SaveConfigQuietly(const char* what);

    AppConfig* config_;
    VoiceStickCoordinator* coordinator_;
};

}  // namespace voicestick
