#include "license.h"
#include "license_public_key.h"
#include "serial_base32.h"
#include <Windows.h>
#include <bcrypt.h>
#include <monocypher.h>
#include <algorithm>
#include <ctime>
namespace voicestick {
namespace {
constexpr std::size_t kPayloadLen = 15, kSerialBytes = 79;

// SHA-256（BCrypt 一次性模式，配方同 firmware_manifest.cc 的 Sha256Hex）。
std::string Sha256Raw(const std::string& input) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::uint8_t digest[32] = {};
    const auto status = BCryptHash(algorithm, nullptr, 0,
                                   const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(input.data())),
                                   static_cast<ULONG>(input.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) return {};
    return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}
}  // namespace

std::uint32_t DateToDays(int year, int month, int day) {
    // Howard Hinnant days_from_civil 纯算术（Civil 历，公元后日期）。
    const int y = year - (month <= 2 ? 1 : 0);
    const unsigned era = static_cast<unsigned>(y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - static_cast<int>(era) * 400);       // [0, 399]
    const unsigned mp = static_cast<unsigned>(month + (month > 2 ? -3 : 9));          // [0, 11]
    const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(day) - 1;         // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                       // [0, 146096]
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 +
                              static_cast<std::int64_t>(doe) - 719468;
    return static_cast<std::uint32_t>(days - kLicenseEpochDays);
}

std::uint32_t DaysSinceEpochTodayUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_utc = {};
    gmtime_s(&tm_utc, &now);
    return DateToDays(tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
}

LicenseVerifyResult VerifyLicenseSerial(const std::string& serial,
                                        const std::vector<std::string>& device_ids,
                                        const std::string& machine_guid,
                                        std::uint32_t now_days) {
    LicenseVerifyResult out;
    const auto bytes = SerialBase32Decode(serial);
    if (!bytes || bytes->size() != kSerialBytes) { out.reason = LicenseError::kBadFormat; return out; }
    const auto* payload = bytes->data();
    const auto* sig = bytes->data() + kPayloadLen;
    // Monocypher EdDSA（curve25519 + BLAKE2b），与 scripts/gen_serial.py 逐字节互操作。
    if (crypto_eddsa_check(sig, kEmbeddedPublicKey, payload, kPayloadLen) != 0) {
        out.reason = LicenseError::kBadSignature; return out;
    }
    out.edition = static_cast<LicenseEdition>(payload[0]);
    if (out.edition != LicenseEdition::kAnnual && out.edition != LicenseEdition::kPerpetual) {
        out.reason = LicenseError::kBadFormat; return out;
    }
    const std::string embedded(payload + 1, payload + 9);
    bool bound = false;
    for (const auto& id : device_ids) {
        if (ComputeBindingKey(id, machine_guid) == embedded) { bound = true; break; }
    }
    if (!bound) { out.reason = LicenseError::kWrongBinding; return out; }
    out.expiry_days = static_cast<std::uint32_t>(payload[9]) |
                      (static_cast<std::uint32_t>(payload[10]) << 8);
    out.perpetual = out.expiry_days == kLicensePerpetualMarker;
    if (!out.perpetual && now_days >= out.expiry_days) {
        out.reason = LicenseError::kExpired; return out;
    }
    out.ok = true; out.reason = LicenseError::kNone;
    return out;
}

std::string ComputeBindingKey(const std::string& device_id, const std::string& machine_guid) {
    return Sha256Raw(device_id + "\n" + machine_guid).substr(0, 8);
}

LicenseStatus EvaluateLicense(const LicenseConfig& cfg,
                              const std::vector<std::string>& device_ids,
                              const std::string& machine_guid,
                              std::uint32_t now_days) {
    LicenseStatus status;
    // 1) 有串码：验签。ok→Active；kExpired→Expired；其余（格式/签名/绑定）
    //    视为无串码，落入试用。
    if (!cfg.serial.empty()) {
        const auto r = VerifyLicenseSerial(cfg.serial, device_ids, machine_guid, now_days);
        if (r.ok) {
            status.state = LicenseState::kActive;
            status.perpetual = r.perpetual;
            status.expiry_days = r.expiry_days;
            status.days_remaining = r.perpetual ? 0
                : static_cast<int>(r.expiry_days) - static_cast<int>(now_days);
            return status;
        }
        if (r.reason == LicenseError::kExpired) {
            status.state = LicenseState::kExpired;
            status.expiry_days = r.expiry_days;
            return status;
        }
    }
    // 2) 无锚点：返回 kTrial 且剩余满试用期，调用方（runtime）负责立即写锚点。
    if (!cfg.trial_anchor_days) {
        status.state = LicenseState::kTrial;
        status.days_remaining = kLicenseTrialDays;
        return status;
    }
    // 3) 时钟回拨：effective_now = max(now, last_seen)；rollback = now < last_seen。
    std::int64_t effective_now = now_days;
    if (cfg.last_seen_days && *cfg.last_seen_days > now_days) {
        status.clock_rollback = true;
        effective_now = *cfg.last_seen_days;
    }
    const std::int64_t deadline =
        static_cast<std::int64_t>(*cfg.trial_anchor_days) + kLicenseTrialDays;
    std::int64_t remaining;
    if (status.clock_rollback) {
        // 回拨期间不信任当前时钟：试用期限按 last_seen 冻结判定，
        // 同时宽限封顶 kLicenseRollbackGraceDays 天（自真实 now 起算）。
        const std::int64_t grace_remaining =
            kLicenseRollbackGraceDays - (effective_now - now_days);
        remaining = std::min(deadline - effective_now, grace_remaining);
    } else {
        remaining = deadline - effective_now;
    }
    if (remaining <= 0) {
        status.state = LicenseState::kExpired;
        status.days_remaining = 0;
        return status;
    }
    status.state = LicenseState::kTrial;
    status.days_remaining = static_cast<int>(remaining);
    return status;
}
}  // namespace voicestick
