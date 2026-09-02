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

// MiniEncoderC 编码器设置。全局默认值存于 AppConfig::default_encoder_settings
//（TOML 顶层 encoder_* 键），[device.<id>.encoder] 表按设备整体覆盖。
struct EncoderSettings {
    // 旋转注入方向键开关（顺时针→Down、逆时针→Up，每格一次）。默认开启。
    bool to_arrow = true;
    // 旋转方向翻转：true 时顺时针→Up、逆时针→Down。默认关闭。
    bool rotation_invert = false;
    // 旋转顺时针/逆时针注入的按键（key_spec 语法，如 "down"/"ctrl+pageup"）。
    std::string rotate_cw_key = "down";
    std::string rotate_ccw_key = "up";
    // 旋转快慢分档阈值（格/秒）：窗口格速 = steps * 100，>= 阈值判快速档。默认 200。
    int rotate_fast_threshold = 200;
    // 快速档顺时针/逆时针注入的按键（key_spec 语法），默认翻页键。
    std::string rotate_cw_fast_key = "pagedown";
    std::string rotate_ccw_fast_key = "pageup";
    // 慢速注入延迟判定窗（ms）：慢速事件先挂起，窗内判快则整段丢弃（快甩加速段），
    // 到期无快速事件才按累计格数补注。0 = 关闭延迟判定（立即注入，旧行为）。默认 80。
    int rotate_decide_window_ms = 80;
    // 录音灯颜色：red/green/blue/yellow/purple/cyan/white/off。下发固件 NVS 持久化。
    std::string led_color = "red";
    // 单击动作："recording"（同主键录音语义）或 "key"（注入 press_key）。
    std::string press_action = "recording";
    // 单击自定义按键（action=key 时生效；空 = 未配置）。
    std::string press_key;
    // 双击动作："key"（注入 double_click_key，默认 enter=现行为）
    // 或 "recording"（双击开始/停止录音，经 remote_button 通道）。
    std::string double_click_action = "key";
    std::string double_click_key = "enter";

    bool operator==(const EncoderSettings& other) const = default;
};

// 小米蓝牙遥控器 2 Pro 设置。全局默认值存于 AppConfig::default_xiaomi_settings
//（标量仅结构默认值；key_map 全局默认在顶层 [xiaomi.keys] 表），
// [device.<id>.xiaomi]（及 .keys 子表）按设备整体覆盖，结构镜像 [device.<id>.encoder]。
struct XiaomiSettings {
    // ADPCM 解码后增益（dB），消费侧 ±24 限幅。默认 12.0。
    double gain_db = 12.0;
    // 语音键双击时序窗（ms）：第一次短击释放后等待第二次按下的最大窗口。默认 350。
    int double_click_ms = 350;
    // 按键映射：button_id（见 xiaomi_buttons.h，mic 除外）→ key_spec 字符串
    //（空串=显式取消该键映射，覆盖全局默认）。空 map = 全部保持系统原生行为。
    std::map<std::string, std::string> key_map;

    bool operator==(const XiaomiSettings& other) const = default;
};

// 设备交互设置（IMU/体感）。全局默认值存于 AppConfig::default_interaction_settings
//（TOML 顶层 imu_wake_sensitivity/tap_to_arrow/tap_sensitivity/air_mouse_sensitivity_x/y 键，
// 向后兼容旧配置），[device.<id>.interaction] 表按设备整体覆盖；消费点统一走
// InteractionSettingsForDevice()。tau/invert_y/曲线等进阶体感参数仍属全局配置。
struct InteractionSettings {
    // IMU 唤醒灵敏度档（低/中/高）：映射固件唤醒阈值，越高越易从深睡被体感唤醒。默认低。
    ImuWakeSensitivity imu_wake_sensitivity = ImuWakeSensitivity::kLow;
    // 敲击手势映射方向键：双击设备外壳注入下方向键，在候选/选项间向下切换。默认关闭。
    bool tap_to_arrow = false;
    // 敲击灵敏度 1~10 档：1=最不灵敏（需大力敲），10=最灵敏（轻触即发）。默认 5。
    int tap_sensitivity = 5;
    // 体感鼠标左右（yaw）灵敏度档 1~10，映射 gain_x=sensitivity_x×48。默认 5。
    int air_mouse_sensitivity_x = 5;
    // 体感鼠标上下（pitch）灵敏度档 1~10，映射 gain_y=sensitivity_y×48。默认 5。
    int air_mouse_sensitivity_y = 5;

    bool operator==(const InteractionSettings& other) const = default;
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

    AsrProvider asr_provider = AsrProvider::kVolcengine;
    std::string voicestick_api_key;
    std::string voicestick_cloud_url = "wss://api.xiaozhi.me/voicestick/asr/";
    std::string volcengine_api_key;
    // 火山自学习平台热词表 / 替换词表 ID（控制台创建）。corpus 热词直传只在流式
    // 第一遍生效，二遍（enable_nonstream）最终文本不吃直传；词表走服务端账户绑定，
    // 是官方备用机制。为空不发送。
    std::string volcengine_boosting_table_id;
    std::string volcengine_correct_table_id;
    std::string tencent_secret_id;
    std::string tencent_secret_key;
    std::string tencent_appid;
    std::string tencent_engine_model_type = "16k_zh";
    std::string tencent_hotword_id;
    std::string llm_base_url = "https://api.openai.com/v1";
    std::string llm_api_key;
    std::string llm_model = "gpt-5.5";
    // 关闭推理型 LLM 的深度思考模式：请求体注入 enable_thinking:false 与
    // chat_template_kwargs.enable_thinking:false，加快精修/翻译输出。
    // 默认开启；对接严格校验未知字段的 OpenAI 官方 API 报 400 时可设 false。
    bool llm_disable_thinking = true;
    // ASR 文本精修：用 LLM 对识别文本去停顿空格 / 修标点 / 去口头语。默认关闭，best-effort。
    bool refine_enabled = false;
    // 精修 system prompt 覆盖；为空时使用内置默认 prompt。
    std::string refine_prompt;
    // 热词处理：划词加词时用 LLM 从选中长文中提炼热词，只把提炼结果写入热词表。
    // 复用 llm_base_url/llm_api_key/llm_model。默认关闭。
    bool hotword_process_enabled = false;
    // 热词候选挖掘：每次识别会话完成后异步让 LLM 从最终文本提炼候选热词，
    // 同一词达阈值后经托盘通知+设置-热词区人工确认入表。默认关闭（多一次 LLM 调用）。
    bool hotword_mining_enabled = false;
    // 热词提炼 system prompt 覆盖；为空时使用内置默认 prompt。
    std::string hotword_process_prompt;
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
    // 粘贴文本后是否自动按回车确认。默认关闭（用户手动回车，避免误提交）。
    bool auto_enter = false;
    bool global_hotkey_enabled = true;
    std::string global_hotkey = "Alt+X";
    bool show_imu_debug = false;
    // 设备交互设置：default_interaction_settings 为全局默认（TOML 顶层 imu_wake_sensitivity/
    // tap_to_arrow/tap_sensitivity/air_mouse_sensitivity_x/y 键，向后兼容旧配置），
    // device_interaction_settings 为 [device.<id>.interaction] 按设备覆盖；消费点统一走
    // InteractionSettingsForDevice()。
    InteractionSettings default_interaction_settings;
    std::map<std::string, InteractionSettings> device_interaction_settings;
    // MiniEncoderC 编码器设置：default_encoder_settings 为全局默认（TOML 顶层
    // encoder_* 键，向后兼容旧配置），device_encoder_settings 为 [device.<id>.encoder]
    // 按设备覆盖；消费点统一走 EncoderSettingsForDevice()。
    EncoderSettings default_encoder_settings;
    std::map<std::string, EncoderSettings> device_encoder_settings;
    // 小米遥控器设置：default_xiaomi_settings 为全局默认（标量仅结构默认值，
    // key_map 走顶层 [xiaomi.keys] 表），device_xiaomi_settings 为
    // [device.<id>.xiaomi]（含 .keys 子表）按设备覆盖；消费点统一走
    // XiaomiSettingsForDevice()。xiaomi_suppress_f5 为全局 F5 抑制开关（Windows）。
    XiaomiSettings default_xiaomi_settings;
    std::map<std::string, XiaomiSettings> device_xiaomi_settings;
    bool xiaomi_suppress_f5 = true;
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
    // 开机自启动：默认开启（随系统登录启动，进入托盘）。
    bool launch_at_login = true;
    // 划词添加热词：启用全局鼠标钩子，划选文本后在选区附近弹出"添加到热词"按钮。
    // 默认关闭以避免常驻低级鼠标钩子。
    bool selection_hotword_enabled = false;
    bool debug_audio_cache = false;
    std::filesystem::path debug_audio_directory;
    // 便携模式：当 exe 同级目录存在 config.toml 时自动激活，
    // 所有数据（配置/日志/调试音频）存储在 exe 目录而非 %APPDATA%。
    bool portable_mode = false;
    // 开发者模式：true 时设置对话框放出全部高级功能（API Key/资源 ID/LLM 凭据/
    // 输出目标/系统区/调试开关等）；false（默认，普通模式）只保留必要功能。
    // config.toml 手改依然生效，仅影响设置页可见性。
    bool developer_mode = false;

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
    // 运行时 Save：先重读磁盘最新凭据覆盖到副本再写回，避免内存过期凭据覆盖用户手改的 key。
    // onboarding/设置对话框保存仍用普通 Save()（需写入用户刚输入的 key）。
    void SavePreservingDiskCredentials() const;
    void SavePreservingDiskCredentials(const std::filesystem::path& path) const;
    // 设置/Onboarding 对话框专用 Save：合并磁盘凭据时，优先保留本对象内存中"用户刚输入"
    // 的非空值，磁盘值仅兜底空字段。等价于"普通 Save + 把内存里已过期的空凭据用磁盘值补回"。
    // 修复路径 B：切 provider 时普通 Save() 用内存空/旧凭据覆盖磁盘 key（如腾讯密钥丢失）。
    void SaveSettingsDialog() const;
    void SaveSettingsDialog(const std::filesystem::path& path) const;
    void SavePairedDevice(const PairedDeviceEntry& entry);
    void SavePairedDeviceInfo(const std::string& device_id,
                              const std::string& hardware,
                              const std::string& firmware_version);
    void RemovePairedDevice(const std::string& device_id);
    std::string ActiveApiKey() const;
    // 腾讯云 ASR 凭据：配置空则回退编译期内置值（预配置 MSI 分发用；不落盘）。
    std::string ActiveTencentSecretId() const;
    std::string ActiveTencentSecretKey() const;
    std::string ActiveTencentAppid() const;
    // DeepSeek LLM 凭据：配置空则回退编译期内置值（翻译/精修/热词提炼复用；不落盘）。
    std::string ActiveLlmApiKey() const;
    std::string ActiveLlmBaseUrl() const;
    std::string ActiveLlmModel() const;
    std::string ActiveWebsocketUrl() const;
    // Volcengine ASR resource_id：配置空则回退 SupportedResourceIds().front()
    // （volc.seedasr.sauc.duration）。修复首启 template resource_id="" 致 volcengine ASR
    // 缺 X-Api-Resource-Id 失败、需进设置切换一次供应商才可用的问题。
    std::string ActiveResourceId() const;
    OutputProfile OutputProfileForDevice(const std::optional<std::string>& device_id) const;
    // 返回设备有效编码器设置：有 [device.<id>.encoder] 覆盖时返回覆盖（加载时已用
    // 全局默认填平所有字段），否则返回全局默认。const 引用返回，旋转热路径零拷贝。
    const EncoderSettings& EncoderSettingsForDevice(const std::optional<std::string>& device_id) const;
    // 返回设备有效交互设置：有 [device.<id>.interaction] 覆盖时返回覆盖（加载时已用
    // 全局默认填平所有字段），否则返回全局默认。const 引用返回，体感热路径零拷贝。
    const InteractionSettings& InteractionSettingsForDevice(const std::optional<std::string>& device_id) const;
    // 返回设备有效小米遥控器设置：有 [device.<id>.xiaomi] 覆盖时返回覆盖（加载时已用
    // 全局默认填平所有字段），否则返回全局默认。
    const XiaomiSettings& XiaomiSettingsForDevice(const std::optional<std::string>& device_id) const;
};

// 内置 key（ActiveApiKey 非空）时向导跳过 kAsr 步；公开版无 key 仍需用户填写。
bool NeedsAsrStep(const AppConfig& config);

// 返回编译期内置凭据（预配置 MSI 分发用；公开构建为空）。
// 来源：CMake VOICESTICK_BUILTIN_* cache 变量 -> builtin_secrets.h 编译期常量。
std::string BuiltinApiKey();
std::string BuiltinTencentSecretId();
std::string BuiltinTencentSecretKey();
std::string BuiltinTencentAppid();
std::string BuiltinLlmApiKey();
std::string BuiltinLlmBaseUrl();
std::string BuiltinLlmModel();

// 通用回退纯函数：配置值优先，空则回退内置值。供腾讯云 SecretKey/AppId 与 LLM
// base_url/api_key/model 字段复用（这些字段不参与 ActiveApiKey 的 provider 分发）。
std::string ResolveActiveString(std::string_view config_value, std::string_view builtin_value);

// 解析当前生效的 ASR API Key：配置值优先；volcengine/tencent 模式空则回退内置 key。
// 抽成纯函数便于单元测试（不依赖编译期常量）。
// cloud 模式不回退内置 key（voicestick_api_key 无内置回退）。
std::string ResolveActiveApiKey(AsrProvider provider,
                                std::string_view voicestick_key,
                                std::string_view volcengine_key,
                                std::string_view tencent_id,
                                std::string_view builtin_volcengine_key,
                                std::string_view builtin_tencent_id);

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
