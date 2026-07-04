#pragma once

#include "air_mouse_kin.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicestick {

enum class AsrProvider {
    kVoiceStickCloud,
    kVolcengine,
    kTencent,
};

enum class InteractionMode {
    kHoldToTalk,
    kClickToTalk,
};

enum class ImuWakeSensitivity {
    kLow,
    kMedium,
    kHigh,
};

enum class UiLanguage {
    kSystem,
    kEnglish,
    kSimplifiedChinese,
};

enum class OverlayThemeColor {
    kWhite,
    kAuto,
    kBlack,
    kPink,
    kGreen,
    kYellow,
    kBlue,
    kPurple,
};

enum class OverlayPosition {
    kCenter,
    kBottomCenter,
    kTopLeft,
    kTopRight,
    kBottomLeft,
    kBottomRight,
};

enum class OverlayThemeSize {
    kBig,
    kMedium,
    kSmall,
};

enum class OutputTarget {
    kFocusedApp,
    kSubtitle,
};

enum class TextTransform {
    kOriginal,
    kTranslate,
};

enum class BluetoothAddressKind : std::uint8_t {
    kUnspecified = 0,
    kPublic = 1,
    kRandom = 2,
};

struct PairedDeviceEntry {
    std::string device_id;
    std::uint64_t bluetooth_address = 0;
    BluetoothAddressKind address_kind = BluetoothAddressKind::kUnspecified;
    std::string name;
    std::string hardware;
    std::string firmware_version;
};

struct OutputProfile {
    OutputTarget target = OutputTarget::kFocusedApp;
    TextTransform transform = TextTransform::kOriginal;
    std::string translation_target = "en";

    bool operator==(const OutputProfile& other) const = default;
};

struct AppConfig {
    static constexpr std::string_view minimum_compatible_firmware_version = "0.3.0";

    AsrProvider asr_provider = AsrProvider::kVoiceStickCloud;
    std::string voicestick_api_key;
    std::string voicestick_cloud_url = "wss://api.xiaozhi.me/voicestick/asr/";
    std::string volcengine_api_key;
    std::string tencent_secret_id;
    std::string tencent_secret_key;
    std::string tencent_appid;
    std::string tencent_engine_model_type = "16k_zh_en";
    std::string tencent_hotword_id;
    std::string llm_base_url = "https://api.openai.com/v1";
    std::string llm_api_key;
    std::string llm_model = "gpt-5.5";
    // ASR 文本精修：用 LLM 对识别文本去停顿空格 / 修标点 / 去口头语。默认开启，best-effort。
    bool refine_enabled = true;
    // 精修 system prompt 覆盖；为空时使用内置默认 prompt。
    std::string refine_prompt;
    InteractionMode interaction_mode = InteractionMode::kHoldToTalk;
    UiLanguage ui_language = UiLanguage::kSystem;
    std::string resource_id = "volc.seedasr.sauc.duration";
    std::vector<std::string> asr_hotwords;
    std::vector<std::string> paired_device_ids;
    std::vector<PairedDeviceEntry> paired_devices;
    std::map<std::string, OverlayThemeColor> device_theme_colors;
    std::map<std::string, OverlayThemeSize> device_theme_sizes;
    std::map<std::string, OverlayPosition> device_overlay_positions;
    OutputProfile default_output_profile;
    std::map<std::string, OutputProfile> device_output_profiles;
    bool auto_enter = true;
    bool global_hotkey_enabled = true;
    std::string global_hotkey = "Alt+X";
    bool prompt_tone_enabled = true;
    bool show_imu_debug = false;
    ImuWakeSensitivity imu_wake_sensitivity = ImuWakeSensitivity::kLow;
    // 敲击手势：双击设备外壳时注入下方向键，用于在候选/选项间向下切换。
    bool tap_to_arrow = false;
    // 敲击灵敏度 1~10 档：1=最不灵敏（需大力敲），10=最灵敏（轻触即发），默认 5。
    int tap_sensitivity = 5;
    // 体感鼠标：左右（yaw）灵敏度档位 1~10，映射 gain_x=sensitivity_x×16。默认 5。
    int air_mouse_sensitivity_x = 5;
    // 体感鼠标：上下（pitch）灵敏度档位 1~10，映射 gain_y=sensitivity_y×16。默认 5。
    int air_mouse_sensitivity_y = 5;
    // 体感鼠标：速度环时间常数（秒），手停滑行 ≈ 3×tau，越大缓停越长。默认 0.05。
    double air_mouse_tau = 0.05;
    // 体感鼠标：是否反转 Y 轴（适配用户习惯）。默认不反转。
    bool air_mouse_invert_y = false;
    // 体感鼠标：三段线性增益曲线参数（运行期可变，热调参面板可调）。
    // 默认 15/50/0.15/4.0（2026-07-04 真机标定），详见 air_mouse_kin.h AirMouseCurveParams。
    // 运行期组装时经 AirMouseCurveClamp 钳位（low<high 不变式、界限），配置文件可存原值。
    double air_mouse_curve_low_thresh = 15.0;
    double air_mouse_curve_high_thresh = 50.0;
    double air_mouse_curve_low_factor = 0.15;
    double air_mouse_curve_high_factor = 4.0;
    // 体感鼠标：控制模式。"angle" = 角度控制（theta 直接映射速度，回中即停）；
    // "rate" = 飞行摇杆/变化率控制（theta 映射加速度，回中后速度保持）。默认 "rate"。
    std::string air_mouse_control_mode = "rate";
    // 体感鼠标：方向锁中立区死区（角度），|theta| 小于此值时光标停并释放方向锁。默认 3.0，范围 1.0~10.0。
    double air_mouse_neutral_deadzone = 3.0;
    // 飞行摇杆模式参数：theta → 加速度增益。默认 80.0，范围 10.0~500.0。
    double air_mouse_rate_gain = 80.0;
    // 飞行摇杆模式参数：速度摩擦系数（1/s）。默认 0.05，范围 0.0~0.5。
    double air_mouse_rate_friction = 0.05;
    // 飞行摇杆模式参数：速度上限（像素/秒）。默认 4000.0，范围 500.0~8000.0。
    double air_mouse_rate_max_speed = 4000.0;
    bool launch_at_login = false;
    bool debug_audio_cache = false;
    std::filesystem::path debug_audio_directory;
    // 便携模式：当 exe 同级目录存在 config.toml 时自动激活，
    // 所有数据（配置/日志/调试音频）存储在 exe 目录而非 %APPDATA%。
    bool portable_mode = false;

    // 返回 exe 所在目录，便携模式下所有数据的根目录。
    static std::filesystem::path PortableBaseDirectory();
    // 检测是否为便携模式：exe 同级目录存在 config.toml 文件。
    static bool IsPortableMode();
    static std::filesystem::path ConfigDirectory();
    static std::filesystem::path ConfigPath();
    static std::filesystem::path DefaultDebugAudioDirectory();
    static AppConfig Defaults();
    static AppConfig Load();
    static AppConfig Load(const std::filesystem::path& path);
    static const std::vector<std::string>& SupportedResourceIds();

    void Save() const;
    void Save(const std::filesystem::path& path) const;
    void SavePairedDevice(const PairedDeviceEntry& entry);
    void SavePairedDeviceInfo(const std::string& device_id,
                              const std::string& hardware,
                              const std::string& firmware_version);
    void RemovePairedDevice(const std::string& device_id);
    std::string ActiveApiKey() const;
    std::string ActiveWebsocketUrl() const;
    OutputProfile OutputProfileForDevice(const std::optional<std::string>& device_id) const;
};

std::string AsrProviderName(AsrProvider provider);
AsrProvider AsrProviderFromName(std::string_view name);
std::string InteractionModeName(InteractionMode mode);
InteractionMode InteractionModeFromName(std::string_view name);
std::string UiLanguageName(UiLanguage language);
UiLanguage UiLanguageFromName(std::string_view name);
UiLanguage UiLanguageFromLocaleName(std::wstring_view locale_name);
UiLanguage EffectiveUiLanguage(UiLanguage configured);
OverlayThemeColor DefaultOverlayThemeColor();
OverlayPosition DefaultOverlayPosition();
std::string OverlayThemeColorName(OverlayThemeColor color);
OverlayThemeColor OverlayThemeColorFromName(std::string_view name);
std::string OverlayThemeColorDisplayName(OverlayThemeColor color);
std::string OverlayThemeSizeName(OverlayThemeSize size);
OverlayThemeSize OverlayThemeSizeFromName(std::string_view name);
std::string OverlayThemeSizeDisplayName(OverlayThemeSize size);
std::string OverlayPositionName(OverlayPosition position);
OverlayPosition OverlayPositionFromName(std::string_view name);
std::string OverlayPositionDisplayName(OverlayPosition position);
std::string OutputTargetName(OutputTarget target);
OutputTarget OutputTargetFromName(std::string_view name);
std::string OutputTargetDisplayName(OutputTarget target);
std::string TextTransformName(TextTransform transform);
TextTransform TextTransformFromName(std::string_view name);
std::string TextTransformDisplayName(TextTransform transform);
std::vector<std::string> ParseDeviceIdList(std::string_view text);
std::vector<std::string> ParseHotwordList(std::string_view text);
std::string ImuWakeSensitivityName(ImuWakeSensitivity sensitivity);
ImuWakeSensitivity ImuWakeSensitivityFromName(std::string_view name);
std::string ImuWakeSensitivityDisplayName(ImuWakeSensitivity sensitivity);
int ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity sensitivity);
// 将敲击灵敏度钳位到 1..10，越界或非法值返回默认档 5。
int TapSensitivityClamp(int level);
int AirMouseSensitivityClamp(int level);
double AirMouseTauClamp(double tau);
double AirMouseNeutralDeadzoneClamp(double deadzone);
// 飞行摇杆/变化率控制参数钳位。
double AirMouseRateGainClamp(double gain);
double AirMouseRateFrictionClamp(double friction);
double AirMouseRateMaxSpeedClamp(double max_speed);

} // namespace voicestick
