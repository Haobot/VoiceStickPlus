#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace voicestick {

enum class LicenseEdition : std::uint8_t { kAnnual = 1, kPerpetual = 2 };
enum class LicenseError { kNone, kBadFormat, kBadSignature, kWrongBinding, kExpired };
struct LicenseVerifyResult {
    bool ok = false;
    LicenseError reason = LicenseError::kNone;
    LicenseEdition edition = LicenseEdition::kAnnual;
    std::uint32_t expiry_days = 0;   // 自 2026-01-01；0xFFFF = 买断
    bool perpetual = false;
};
constexpr std::uint32_t kLicenseEpochDays = 0x4FE6;  // days_from_civil(2026,1,1) = 20454
constexpr int kLicenseTrialDays = 30;
constexpr int kLicenseRollbackGraceDays = 7;
constexpr std::uint32_t kLicensePerpetualMarker = 0xFFFF;
// Howard Hinnant days_from_civil 纯算术，返回距 2026-01-01 的天数（gmtime 安全）。
std::uint32_t DateToDays(int year, int month, int day);
// 当前 UTC 日期距 2026-01-01 天数（EvaluateLicense/VerifyLicenseSerial 的缺省 now）。
std::uint32_t DaysSinceEpochTodayUtc();

// device_ids 为归一化（去前缀大写 hex）设备 ID 列表；验签 + 绑定匹配 + 到期判定。
// now_days 缺省 = DaysSinceEpochTodayUtc()（当前 UTC 日期距 2026-01-01 天数）。
LicenseVerifyResult VerifyLicenseSerial(const std::string& serial,
                                        const std::vector<std::string>& device_ids,
                                        const std::string& machine_guid,
                                        std::uint32_t now_days = DaysSinceEpochTodayUtc());
std::string ComputeBindingKey(const std::string& normalized_device_id,
                              const std::string& machine_guid);  // 8 字节原始值返回 string

struct LicenseConfig {
    std::string serial;             // 原文串码（含连字符）
    std::optional<std::uint32_t> trial_anchor_days;  // 试用期起锚（自 2026-01-01）
    std::optional<std::uint32_t> last_seen_days;     // 见过的最大日期，防回拨
};
enum class LicenseState { kTrial, kActive, kExpired };
struct LicenseStatus {
    LicenseState state = LicenseState::kTrial;
    int days_remaining = 0;          // kTrial/kActive 有意义
    bool perpetual = false;
    bool clock_rollback = false;     // 检测到回拨并进入宽限
    std::uint32_t expiry_days = 0;   // Active 的到期日
};
LicenseStatus EvaluateLicense(const LicenseConfig& cfg,
                              const std::vector<std::string>& device_ids,
                              const std::string& machine_guid,
                              std::uint32_t now_days);
}  // namespace voicestick
