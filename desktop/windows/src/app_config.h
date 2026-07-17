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
    // wechat 模式运行期派生：按下即录音跳过 300ms 阈值。不暴露到配置文件/UI，
    // InteractionModeFromName 不解析（保持回退 hold_to_talk），仅 coordinator 在
    // wechat 模式 + hold_to_talk 时构造并下发给固件。
    kHoldToTalkInstant,
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
    kWechatInputMethod,
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

struct WechatInputMethodConfig {
    // 触发第三方输入法语音输入的快捷键字符串（legacy 字段，仅向后兼容加载旧配置；
    // 运行时按触发模式取 hotkey_hold/hotkey_click，不再使用此字段）。
    std::string hotkey = "ctrl+win";
    // 长按式（hold_to_talk）触发热键，例如微信输入法的 "ctrl+win"。
    std::string hotkey_hold = "ctrl+win";
    // 点按式（click_to_talk）触发热键，例如 Typeless 的 "ralt"。
    std::string hotkey_click = "ralt";
    // 第三方输入法专属触发模式：长按式（hold_to_talk）或点按式（click_to_talk）。
    // 与全局 interaction_mode 解耦：wechat 的触发方式只影响 wechat 模式，
    // 不污染 focused_app/字幕的全局 interaction_mode（切输出目标时不再互相影响）。
    // 旧配置迁移：加载时若缺此字段，从顶层 interaction_mode 继承并把顶层重置为 kHoldToTalk。
    InteractionMode trigger_mode = InteractionMode::kHoldToTalk;
    // 虚拟麦克风播放端名称子串，例如 "CABLE Input (VB-Audio Virtual Cable)"。
    std::string virtual_mic_playback_name = "CABLE Input";
    // 虚拟麦克风录音端名称子串，例如 "CABLE Output (VB-Audio Virtual Cable)"。
    // 与播放端是不同设备：播放端是 VoiceStick 写入的 eRender 设备（CABLE Input），
    // 录音端是系统录音设备（CABLE Output），auto_switch 切默认录音设备指向此端。
    std::string virtual_mic_capture_name = "CABLE Output";
    // 是否在录音期间自动将默认录音设备切换为虚拟麦克风。
    bool auto_switch_default_recording_device = false;

    // 按当前交互模式返回应使用的热键。kHoldToTalkInstant 归入长按式。
    std::string ActiveHotkey(InteractionMode mode) const {
        return mode == InteractionMode::kClickToTalk ? hotkey_click : hotkey_hold;
    }

    bool operator==(const WechatInputMethodConfig& other) const = default;
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
    WechatInputMethodConfig wechat_input_method;
    std::map<std::string, OutputProfile> device_output_profiles;
    bool auto_enter = true;
    bool global_hotkey_enabled = true;
    std::string global_hotkey = "Alt+X";
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
    // 默认 100/333/0.25/4.0（P2：low_factor 由 0.15→0.25 补偿固件死区 3.0→1.5dps 下调；
    // 阈值单位为固件缩放角速率 dps×REPORT_GAIN=4，对应物理特征点约 25/83 dps），
    // 详见 air_mouse_kin.h AirMouseCurveParams。曲线为平滑 sigmoid（无折角）。
    // 运行期组装时经 AirMouseCurveClamp 钳位（low<high 不变式、界限 [1,200]/[50,800]/factor[0.05,0.5]/[2,6]），配置文件可存原值。
    double air_mouse_curve_low_thresh = 100.0;
    double air_mouse_curve_high_thresh = 333.0;
    double air_mouse_curve_low_factor = 0.25;
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
    // 首启种子配置：若 target 不存在且 template 存在，则复制 template 到 target。
    // 返回是否执行了复制。target 已存在不覆盖；template 不存在或复制失败均返回 false，不抛异常。
    static bool SeedConfigFromTemplate(const std::filesystem::path& template_path,
                                       const std::filesystem::path& target_path);
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
