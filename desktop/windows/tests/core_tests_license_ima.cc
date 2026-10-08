// N8 cut14: license/ima/asr-refiner/caps batch extracted from core_tests.cc
// (40 tests; count converged). Pre-moved to test_support.h: ImaGoldenVector+
// ImaEncodeForTest, misnamed helper TestSha256Hex, plus all two-direction
// convergent helpers (ascending order). Orphan registration fixed first.
#include "test_support.h"

void TestLicenseVerifySerial() {
    using namespace voicestick;
    const std::vector<std::string> devices = {"AB12", "00FF"};
    const std::string guid = "{11111111-2222-3333-4444-555555555555}";
    // 向量 1：年费码，绑定 AB12，到期 2027-01-01（未过期，相对固定 now=2026-09-13）
    auto r = VerifyLicenseSerial(kTestSerial1, devices, guid, DateToDays(2026, 9, 13));
    assert(r.ok && r.edition == LicenseEdition::kAnnual);
    assert(r.expiry_days == 365);  // 2027-01-01 距 2026-01-01
    // 绑定机器不匹配 → kWrongBinding
    r = VerifyLicenseSerial(kTestSerial1, devices, "{99999999-8888-7777-6666-555555555555}");
    assert(!r.ok && r.reason == LicenseError::kWrongBinding);
    // 设备不在场列表 → kWrongBinding
    r = VerifyLicenseSerial(kTestSerial1, {"EEEE"}, guid);
    assert(!r.ok && r.reason == LicenseError::kWrongBinding);
    // 向量 2：买断码（edition 2, expiry 0xFFFF）
    r = VerifyLicenseSerial(kTestSerial2, {"CD34"}, "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}");
    assert(r.ok && r.edition == LicenseEdition::kPerpetual && r.perpetual);
    // 篡改一个字符 → kBadSignature
    std::string tampered = kTestSerial1; tampered[10] = tampered[10] == 'A' ? 'B' : 'A';
    r = VerifyLicenseSerial(tampered, devices, guid);
    assert(!r.ok && r.reason == LicenseError::kBadSignature);
    // 截断/垃圾 → kBadFormat
    r = VerifyLicenseSerial("HELLO", devices, guid);
    assert(!r.ok && r.reason == LicenseError::kBadFormat);
    // 向量 3 到期 2026-12-31，未过期边界（now=2026-12-30）仍可用
    r = VerifyLicenseSerial(kTestSerial3, devices, "no-braces-guid", DateToDays(2026, 12, 30));
    assert(r.ok && r.expiry_days == 364);
    // 过期：用 now=2027-01-02 判定
    r = VerifyLicenseSerial(kTestSerial3, devices, "no-braces-guid", DateToDays(2027, 1, 2));
    assert(!r.ok && r.reason == LicenseError::kExpired);
}

void TestC8LogUrlPromptHygiene() {
    namespace fs = std::filesystem;
    std::error_code ec;

    // ① 日志轮转：超限改名 <path>.old（只留一代），未超限不动，文件缺失静默。
    const auto dir = fs::temp_directory_path() / "voicestick_c8_log";
    fs::create_directories(dir, ec);
    const auto log_path = dir / "app.log";
    {
        std::ofstream out(log_path, std::ios::binary | std::ios::trunc);
        out << std::string(2048, 'x');
    }
    RotateLogIfTooLarge(log_path, 1024);  // 2048 > 1024 → 轮转
    assert(!fs::exists(log_path, ec));
    const auto old_path = dir / "app.log.old";
    assert(fs::exists(old_path, ec));
    assert(fs::file_size(old_path, ec) == 2048);
    RotateLogIfTooLarge(log_path, 1024);  // 文件已不在 → 静默返回
    assert(fs::exists(old_path, ec));
    {
        std::ofstream out(log_path, std::ios::binary | std::ios::trunc);
        out << "tiny";
    }
    RotateLogIfTooLarge(log_path, 1024);  // 4 < 1024 → 不轮转
    assert(fs::exists(log_path, ec));
    assert(fs::file_size(log_path, ec) == 4);
    fs::remove_all(dir, ec);

    // ② URL 脱敏：query（腾讯签名串所在）与 fragment 必须去掉，日志只留 scheme/host/path。
    assert(AsrClientTencent::UrlWithoutQuery(
               "wss://asr.cloud.tencent.com/asr/v2/1?authorization=SECRET&x=1") ==
           "wss://asr.cloud.tencent.com/asr/v2/1");
    assert(AsrClientTencent::UrlWithoutQuery("wss://h/p#frag") == "wss://h/p");
    assert(AsrClientTencent::UrlWithoutQuery("wss://h/p") == "wss://h/p");
    assert(AsrClientTencent::UrlWithoutQuery("").empty());

    // ③ 精修 prompt 封顶 4096 字节，且截断落在 UTF-8 字符边界。
    const auto capped = LLMRefinementClient::BuildRefinePrompt(std::string(6000, 'a'), {});
    assert(capped.size() == 4096);
    assert(capped == std::string(4096, 'a'));
    std::string cjk;  // 3000 × "中"(3B) = 9000B；4096 % 3 == 1 → 直接截必切半码点
    for (int i = 0; i < 3000; ++i) cjk += "\xE4\xB8\xAD";
    const auto capped_cjk = LLMRefinementClient::BuildRefinePrompt(cjk, {});
    assert(capped_cjk.size() <= 4096);
    assert(capped_cjk.size() % 3 == 0);  // 无半截码点
    // 短 override 不受影响
    assert(LLMRefinementClient::BuildRefinePrompt("my custom prompt", {}) == "my custom prompt");
}

void TestSecureCloudUrlPolicy() {
    // C7：云/ASR 链路 TLS-only——ws:// 与 http:// 明文一律拒绝（api_key 经此传输，
    // 明文等于把凭据放到网络上）；wss:// 映射 https://、https:// 原样。
    assert(SecureHttpUrlFromWebSocketUrl("wss://api.xiaozhi.me/voicestick/asr/") ==
           "https://api.xiaozhi.me/voicestick/asr/");
    assert(SecureHttpUrlFromWebSocketUrl("https://example.com/x") == "https://example.com/x");
    assert(!SecureHttpUrlFromWebSocketUrl("ws://api.xiaozhi.me/x"));
    assert(!SecureHttpUrlFromWebSocketUrl("http://example.com/x"));
    assert(!SecureHttpUrlFromWebSocketUrl("ftp://example.com"));
    assert(!SecureHttpUrlFromWebSocketUrl(""));
    assert(!SecureHttpUrlFromWebSocketUrl("api.xiaozhi.me"));  // 无 scheme
    assert(!SecureHttpUrlFromWebSocketUrl("wss:/broken"));     // scheme 残缺

    // 响应结果链接由 ShellExecute 打开，只放行 https://（防明文与 file:// 等被篡改执行）。
    assert(IsHttpsUrl("https://example.com/pay"));
    assert(!IsHttpsUrl("http://example.com/pay"));
    assert(!IsHttpsUrl("file:///C:/Windows/System32/cmd.exe"));
    assert(!IsHttpsUrl("javascript:alert(1)"));
    assert(!IsHttpsUrl(""));
}

void TestLicenseBindingDevicesUnion() {
    using namespace voicestick;
    // C2：绑定候选 = 已连接 ∪ 已配对，归一化（去 VS-/RC- 前缀 + 大写 hex）、去重，
    // 连接侧在前保持既有顺序语义；非法 id 跳过。
    auto devices = LicenseBindingDevices({"VS-00ff", "vs-1234"}, {"00FF", "AB12", "zz"});
    assert(devices.size() == 3);
    assert(devices[0] == "00FF");
    assert(devices[1] == "1234");
    assert(devices[2] == "AB12");
    assert(LicenseBindingDevices({}, {"", "VS-", "hello"}).empty());

    // 语义级：设备关机（已连接为空）但已配对 → 串码仍验签通过。
    // 原实现只取「当前已连接」，此处会 kWrongBinding 而跌回试用、本地麦被闸。
    const std::string guid = "{11111111-2222-3333-4444-555555555555}";
    auto offline = LicenseBindingDevices({}, {"AB12", "00FF"});
    auto r = VerifyLicenseSerial(kTestSerial1, offline, guid, DateToDays(2026, 9, 13));
    assert(r.ok && r.edition == LicenseEdition::kAnnual);

    // 反例：既未连接也未配对 → 仍判绑定不符（配对才放宽，不是无条件放行）。
    auto r2 = VerifyLicenseSerial(kTestSerial1, LicenseBindingDevices({}, {}), guid,
                                  DateToDays(2026, 9, 13));
    assert(!r2.ok && r2.reason == LicenseError::kWrongBinding);
}

void TestLicenseDateToDaysPreEpoch() {
    using namespace voicestick;
    // C3 回归：纪元（2026-01-01）前的日期必须钳到 0，不得 uint32 下溢。
    // 下溢时 now 约为 42.9 亿 → ① 有效年卡被 now >= expiry 误判过期；
    // ② last_seen > now 恒假 → 时钟回拨检测完全失效。
    assert(DateToDays(2026, 1, 1) == 0);
    assert(DateToDays(2026, 1, 2) == 1);
    assert(DateToDays(2027, 1, 1) == 365);
    // 纪元前：RTC 失电复位到 1970、2024/2025 手动回调
    assert(DateToDays(1970, 1, 1) == 0);
    assert(DateToDays(2024, 6, 15) == 0);
    assert(DateToDays(2025, 12, 31) == 0);
    // 正常年份的「今天」不可能是下溢大数（40000 天 ≈ 2135 年）
    assert(DaysSinceEpochTodayUtc() < 40000);

    // 语义级：时钟早于纪元时——年费码仍有效（不被判过期）。
    const std::vector<std::string> devices = {"AB12", "00FF"};
    const std::string guid = "{11111111-2222-3333-4444-555555555555}";
    const auto pre_epoch = DateToDays(1970, 1, 1);
    assert(pre_epoch == 0);
    auto r = VerifyLicenseSerial(kTestSerial1, devices, guid, pre_epoch);
    assert(r.ok && r.edition == LicenseEdition::kAnnual);

    // 语义级：回拨必须被识别（下溢时恒为 false）。
    LicenseConfig cfg;
    cfg.trial_anchor_days = DateToDays(2026, 9, 1);
    cfg.last_seen_days = DateToDays(2026, 9, 13);  // 纪元后的正常日期
    auto s = EvaluateLicense(cfg, devices, guid, pre_epoch);
    assert(s.clock_rollback);
}

void TestLicenseStatus() {
    using namespace voicestick;
    LicenseConfig cfg;  // 默认空
    const std::vector<std::string> devices = {"AB12"};
    const std::string guid = "{11111111-2222-3333-4444-555555555555}";
    const auto now = DateToDays(2026, 9, 13);
    // 无串码无锚点：调用方负责先写锚点；此处直接给锚点
    cfg.trial_anchor_days = DateToDays(2026, 9, 1);
    auto s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kTrial && s.days_remaining == 18);  // 9-1 + 30d
    // 锚点 40 天前 → 过期
    cfg.trial_anchor_days = DateToDays(2026, 8, 4);
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kExpired);
    // 有效串码 → Active
    cfg.serial = kTestSerial1;
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kActive);
    // 时钟回拨：last_seen 在未来 3 天 → grace 封顶（剩余按 last_seen 冻结为 15 天，
    // 宽限自真实 now 起 7-3=4 天，取 min）
    cfg.trial_anchor_days = DateToDays(2026, 9, 1);
    cfg.serial.clear();
    cfg.last_seen_days = now + 3;
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kTrial && s.clock_rollback && s.days_remaining == 4);
    // 回拨超过宽限（last_seen 未来 30 天）
    cfg.last_seen_days = now + 30;
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kExpired);
    // 无锚点 → 满试用期，标记由调用方写锚点
    cfg = LicenseConfig{};
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kTrial && s.days_remaining == kLicenseTrialDays);
    // 过期串码（reason=kExpired）→ kExpired（serial3 绑定 00FF + no-braces-guid）
    cfg.serial = kTestSerial3;
    s = EvaluateLicense(cfg, {"00FF"}, "no-braces-guid", DateToDays(2027, 1, 2));
    assert(s.state == LicenseState::kExpired);
    // 格式错误/绑定不匹配的串码 → 落入试用（视为无串码）
    cfg.serial = "HELLO";
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kTrial && s.days_remaining == kLicenseTrialDays);
}

void TestLicenseConfigRoundTrip() {
    assert(AppConfig::Defaults().license.serial.empty());
    assert(!AppConfig::Defaults().license.trial_anchor_days.has_value());
    assert(!AppConfig::Defaults().license.last_seen_days.has_value());

    auto temp = std::filesystem::temp_directory_path() / "voicestick_license_test.toml";
    std::filesystem::remove(temp);

    // 缺省两态：只有 serial，锚点/last_seen 缺省 → 缺省不落盘，往返相等。
    AppConfig config = AppConfig::Defaults();
    config.license.serial = kTestSerial1;
    config.Save(temp);
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.license == config.license);
    assert(loaded.license.serial == kTestSerial1);
    assert(!loaded.license.trial_anchor_days.has_value());
    assert(!loaded.license.last_seen_days.has_value());

    // 锚点/last_seen 有值 → 序列化并往返。
    config.license.trial_anchor_days = 243;  // 2026-09-01
    config.license.last_seen_days = 258;     // 2026-09-16
    config.Save(temp);
    loaded = AppConfig::Load(temp);
    assert(loaded.license == config.license);
    assert(loaded.license.trial_anchor_days == 243);
    assert(loaded.license.last_seen_days == 258);

    std::filesystem::remove(temp);
}

void TestSavePairedDeviceInfoPreservesDiskLicense() {
    auto temp = std::filesystem::temp_directory_path() / "voicestick_paired_save_license.toml";
    std::filesystem::remove(temp);

    // 配对设备落盘（模拟首配）。
    AppConfig config = AppConfig::Defaults();
    PairedDeviceEntry entry;
    entry.device_id = "AB12";
    entry.bluetooth_address = 0x1234;
    config.paired_devices.push_back(entry);
    config.Save(temp);

    // 协调器持有的是「锚点写入前」的过期快照（smoke 真机事故复现）。
    AppConfig stale = AppConfig::Load(temp);

    // LicenseRuntime 随后把试用锚点写进磁盘（经另一个对象 + 全量 Save）。
    AppConfig runtime_view = AppConfig::Load(temp);
    runtime_view.license.trial_anchor_days = 243;
    runtime_view.license.last_seen_days = 258;
    runtime_view.Save(temp);

    // 过期快照走设备信息保存路径：必须先重取磁盘 license 再落盘。
    stale.ReloadLicenseFromDisk(temp);
    stale.SavePairedDeviceInfo(temp, "AB12", "xiaomi_remote_2_pro", "1.0");
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.license.trial_anchor_days == 243);
    assert(loaded.license.last_seen_days == 258);
    // 设备信息本身仍落盘。
    assert(loaded.paired_devices.size() == 1);
    assert(loaded.paired_devices[0].hardware == "xiaomi_remote_2_pro");

    // 磁盘文件不存在时不崩溃、不改内存值。
    AppConfig fresh = AppConfig::Defaults();
    fresh.license.serial = "KEEP";
    fresh.ReloadLicenseFromDisk(temp.parent_path() / "definitely_missing.toml");
    assert(fresh.license.serial == "KEEP");

    std::filesystem::remove(temp);
}

void TestVolcengineTableIdConfigRoundTrip() {
    assert(AppConfig::Defaults().volcengine_boosting_table_id.empty());
    assert(AppConfig::Defaults().volcengine_correct_table_id.empty());

    auto temp = std::filesystem::temp_directory_path() / "voicestick_volc_table_id_test.toml";
    std::filesystem::remove(temp);

    AppConfig config = AppConfig::Defaults();
    config.volcengine_boosting_table_id = "  boost-123  ";
    config.volcengine_correct_table_id = "correct-456";
    config.Save(temp);

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.volcengine_boosting_table_id == "boost-123");
    assert(loaded.volcengine_correct_table_id == "correct-456");

    std::filesystem::remove(temp);
}


void TestAppConfig() {
    AppConfig cloud = AppConfig::Defaults();
    assert(cloud.asr_provider == AsrProvider::kVolcengine);
    cloud.asr_provider = AsrProvider::kVoiceStickCloud;
    cloud.voicestick_cloud_url = "";
    assert(cloud.ActiveWebsocketUrl() == "wss://api.xiaozhi.me/voicestick/asr/");

    cloud.voicestick_cloud_url = "  wss://example.test/asr?token=1  ";
    assert(cloud.ActiveWebsocketUrl() == "wss://example.test/asr?token=1");

    AppConfig volcengine = AppConfig::Defaults();
    volcengine.asr_provider = AsrProvider::kVolcengine;
    assert(volcengine.ActiveWebsocketUrl().starts_with("wss://openspeech.bytedance.com/"));

    PairedDeviceEntry entry;
    entry.device_id = "5A74";
    entry.bluetooth_address = 0x70041DDA5A76;
    entry.address_kind = BluetoothAddressKind::kPublic;
    entry.name = "VS-5A74";
    entry.hardware = "stick_s3";
    entry.firmware_version = "0.1.2";
    AppConfig cache = AppConfig::Defaults();
    cache.paired_devices.push_back(entry);
    cache.paired_device_ids.push_back(entry.device_id);
    assert(cache.paired_devices.front().hardware == "stick_s3");
    assert(cache.paired_devices.front().firmware_version == "0.1.2");
    assert(OverlayThemeColorFromName("auto") == OverlayThemeColor::kAuto);
    assert(OverlayThemeColorFromName("black") == OverlayThemeColor::kBlack);
    assert(OverlayThemeColorFromName("pink") == OverlayThemeColor::kPink);
    assert(OverlayThemeColorName(OverlayThemeColor::kAuto) == "auto");
    assert(OverlayThemeColorName(OverlayThemeColor::kGreen) == "green");
    assert(OverlayThemeColorName(OverlayThemeColor::kBlack) == "black");
    assert(OverlayThemeColorDisplayName(OverlayThemeColor::kAuto) == "Auto");
    assert(OverlayThemeColorDisplayName(OverlayThemeColor::kYellow) == "Yellow");
    assert(OverlayThemeColorDisplayName(OverlayThemeColor::kBlack) == "Black");
    assert(DefaultOverlayThemeColor() == OverlayThemeColor::kAuto);
    assert(VoiceStickCoordinator::ThemeColorForConfig(AppConfig::Defaults(), "5A74") == OverlayThemeColor::kAuto);
    assert(OverlayThemeSizeFromName("medium") == OverlayThemeSize::kMedium);
    assert(OverlayThemeSizeFromName("small") == OverlayThemeSize::kSmall);
    assert(OverlayThemeSizeFromName("big") == OverlayThemeSize::kBig);
    assert(OverlayThemeSizeName(OverlayThemeSize::kMedium) == "medium");
    assert(OverlayThemeSizeDisplayName(OverlayThemeSize::kSmall) == "Small");
    assert(OverlayPositionFromName("bottom_center") == OverlayPosition::kBottomCenter);
    assert(OverlayPositionFromName("middle_bottom") == OverlayPosition::kBottomCenter);
    assert(OverlayPositionFromName("top_right") == OverlayPosition::kTopRight);
    assert(OverlayPositionName(OverlayPosition::kBottomLeft) == "bottom_left");
    assert(OverlayPositionName(OverlayPosition::kBottomCenter) == "bottom_center");
    assert(OverlayPositionDisplayName(OverlayPosition::kCenter) == "Center");
    assert(OverlayPositionDisplayName(OverlayPosition::kBottomCenter) == "Bottom Center");
    assert(DefaultOverlayPosition() == OverlayPosition::kBottomCenter);
    cache.default_output_profile.target = OutputTarget::kSubtitle;
    cache.default_output_profile.transform = TextTransform::kOriginal;
    cache.device_output_profiles["5A74"] = OutputProfile{
        OutputTarget::kSubtitle,
        TextTransform::kTranslate,
        "zh-Hans",
    };
    auto profile = cache.OutputProfileForDevice(std::optional<std::string>("5A74"));
    assert(profile.target == OutputTarget::kSubtitle);
    assert(profile.transform == TextTransform::kTranslate);
    assert(profile.translation_target == "zh-Hans");
    assert(OutputTargetName(OutputTarget::kFocusedApp) == "focused_app");
    assert(TextTransformFromName("translate") == TextTransform::kTranslate);
    assert(AppConfig::Defaults().ui_language == UiLanguage::kSystem);
    assert(AppConfig::Defaults().launch_at_login);
    assert(UiLanguageFromName("system") == UiLanguage::kSystem);
    assert(UiLanguageFromName("en") == UiLanguage::kEnglish);
    assert(UiLanguageFromName("zh-Hans") == UiLanguage::kSimplifiedChinese);
    assert(UiLanguageFromName("invalid") == UiLanguage::kSystem);
    assert(UiLanguageName(UiLanguage::kSystem) == "system");
    assert(UiLanguageName(UiLanguage::kEnglish) == "en");
    assert(UiLanguageName(UiLanguage::kSimplifiedChinese) == "zh-Hans");
    assert(UiLanguageFromLocaleName(L"en-US") == UiLanguage::kEnglish);
    assert(UiLanguageFromLocaleName(L"en-GB") == UiLanguage::kEnglish);
    assert(UiLanguageFromLocaleName(L"zh-CN") == UiLanguage::kSimplifiedChinese);
    assert(UiLanguageFromLocaleName(L"zh-Hans") == UiLanguage::kSimplifiedChinese);
    assert(UiLanguageFromLocaleName(L"zh-TW") == UiLanguage::kSimplifiedChinese);
    assert(UiLanguageFromLocaleName(L"ja-JP") == UiLanguage::kEnglish);
    assert(UiLanguageFromLocaleName(L"") == UiLanguage::kEnglish);
    assert(!Tr(StringId::kSettingsTitle, UiLanguage::kEnglish).empty());
    assert(Tr(StringId::kSettingsTitle, UiLanguage::kSimplifiedChinese) == "VoiceStick 设置");
    assert(Tr(StringId::kSettingsLaunchAtLogin, UiLanguage::kEnglish) == "Start VoiceStick when Windows starts");
    assert(Tr(StringId::kMenuLaunchAtLogin, UiLanguage::kSimplifiedChinese) == "开机自启动");
    assert(Tr(StringId::kPairManualIdHint, UiLanguage::kEnglish) == "Can't find it? Enter the 4-digit ID shown on the Stick:");
    assert(Tr(StringId::kPairManualIdHint, UiLanguage::kSimplifiedChinese) == "找不到设备？请输入 Stick 屏幕显示的 4 位 ID：");
    assert(Tr(StringId::kHotkeyCapturePrompt, UiLanguage::kEnglish) == "Press a hotkey combination...");
    // 快捷键录入超时提示（UIPI 前台提权隔离引导，两个捕获对话框共用）。
    assert(Tr(StringId::kHotkeyCaptureTimeoutTitle, UiLanguage::kSimplifiedChinese) == "快捷键录入");
    assert(!Tr(StringId::kHotkeyCaptureTimeoutBody, UiLanguage::kEnglish).empty());
    assert(Tr(StringId::kHotkeyCaptureTimeoutBody, UiLanguage::kSimplifiedChinese)
               .find("管理员") != std::string::npos);
    // 按键映射录入超时提示（keymap 专用：UIPI 隔离 + RC003 返回键死键双因覆盖，
    // 引导手动输入——死键系统零事件，按键录入永远无响应，见 Plan/xiaomi-keymap-hotkey-capture-issue.md §0.1）。
    assert(Tr(StringId::kXiaomiKeymapCaptureHintTitle, UiLanguage::kSimplifiedChinese) == "按键录入");
    assert(Tr(StringId::kXiaomiKeymapCaptureHintBody, UiLanguage::kSimplifiedChinese)
               .find("RC003") != std::string::npos);
    assert(Tr(StringId::kXiaomiKeymapCaptureHintBody, UiLanguage::kSimplifiedChinese)
               .find("手动输入") != std::string::npos);
    assert(Tr(StringId::kXiaomiKeymapCaptureHintBody, UiLanguage::kEnglish)
               .find("RC003") != std::string::npos);
    assert(Tr(StringId::kFirmwareUpdateFinalizing, UiLanguage::kSimplifiedChinese) == "正在完成固件更新...");
    assert(BatteryStatusText(83, false, false, UiLanguage::kEnglish) == "83%");
    assert(BatteryStatusText(83, true, false, UiLanguage::kEnglish) == "83%, charging");
    assert(BatteryStatusText(83, false, true, UiLanguage::kSimplifiedChinese) == "83%，外接电源");
    assert(DeviceTitleWithBattery(L"VS-5A74", 83, true, false, UiLanguage::kSimplifiedChinese) == L"VS-5A74 (83%，充电中)");
    assert(ImuWakeSensitivityFromName("low") == ImuWakeSensitivity::kLow);
    assert(ImuWakeSensitivityFromName("medium") == ImuWakeSensitivity::kMedium);
    assert(ImuWakeSensitivityFromName("high") == ImuWakeSensitivity::kHigh);
    assert(ImuWakeSensitivityFromName("invalid") == ImuWakeSensitivity::kLow);
    assert(ImuWakeSensitivityName(ImuWakeSensitivity::kLow) == "low");
    assert(ImuWakeSensitivityName(ImuWakeSensitivity::kMedium) == "medium");
    assert(ImuWakeSensitivityName(ImuWakeSensitivity::kHigh) == "high");
    assert(ImuWakeSensitivityDisplayName(ImuWakeSensitivity::kLow) == "Low");
    assert(ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity::kLow) == 800);
    assert(ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity::kMedium) == 500);
    assert(ImuWakeSensitivityThresholdLsb(ImuWakeSensitivity::kHigh) == 250);
    assert(LocalizationTablesAreComplete());
    const auto hotwords = ParseHotwordList(" 小智,VoiceStick\r\n小智\n豆包 ");
    assert((hotwords == std::vector<std::string>{"小智", "VoiceStick", "豆包"}));
}

void TestLlmRefinePromptAndPayload() {
    // 内置默认精修 prompt 含三类清理要求关键词（中文）。
    const auto prompt = LLMRefinementClient::BuildRefinePrompt("");
    assert(prompt.find("语音停顿") != std::string::npos);
    assert(prompt.find("标点") != std::string::npos);
    assert(prompt.find("填充词") != std::string::npos);

    // 非空 override 去空白后原样返回。
    const auto custom = LLMRefinementClient::BuildRefinePrompt("  my custom prompt  ");
    assert(custom == "my custom prompt");

    // 热词非空时追加热词替换指引（二遍 ASR 不吃 corpus 热词，由 LLM 精修兜底）；
    // 默认 prompt 与用户 override 都追加；空热词不追加。
    const auto refine_with_hotwords =
        LLMRefinementClient::BuildRefinePrompt("", {"AGENTS.md", "CLAUDE.md"});
    assert(refine_with_hotwords.find("热词表") != std::string::npos);
    assert(refine_with_hotwords.find("AGENTS.md") != std::string::npos);
    assert(refine_with_hotwords.find("CLAUDE.md") != std::string::npos);
    assert(refine_with_hotwords.find("语音停顿") != std::string::npos);
    const auto custom_with_hotwords =
        LLMRefinementClient::BuildRefinePrompt("my custom prompt", {"AGENTS.md"});
    assert(custom_with_hotwords.starts_with("my custom prompt"));
    assert(custom_with_hotwords.find("AGENTS.md") != std::string::npos);
    assert(LLMRefinementClient::BuildRefinePrompt("my custom prompt", {}) == "my custom prompt");

    // few-shot 示例与「原文已正确的热词不得改写」约束在热词段中。
    assert(refine_with_hotwords.find("示例") != std::string::npos);
    assert(refine_with_hotwords.find("原样保留") != std::string::npos);

    // 精修守卫：原文已出现的热词在精修结果中必须保留。
    assert(LLMRefinementClient::RefineResultKeepsHotwords(
        "编辑 AGENTS.md 文件", "编辑 AGENTS.md 这个文件。", {"AGENTS.md"}));
    assert(!LLMRefinementClient::RefineResultKeepsHotwords(
        "编辑 AGENTS.md 文件", "编辑 CLDE.md 这个文件。", {"AGENTS.md"}));
    // 原文中没有的热词不受约束（精修纠错场景）；空热词表恒真。
    assert(LLMRefinementClient::RefineResultKeepsHotwords(
        "编辑 agentsdmd 文件", "编辑 AGENTS.md 文件。", {"AGENTS.md"}));
    assert(LLMRefinementClient::RefineResultKeepsHotwords("a", "b", {}));
    assert(LLMRefinementClient::RefineResultKeepsHotwords("a", "b", {""}));

    // 翻译 prompt 已融合精修要求，且保留翻译语义与热词。
    const auto translation_prompt = LLMTranslationClient::SystemPrompt("en", {});
    assert(translation_prompt.find("Translate") != std::string::npos);
    assert(translation_prompt.find("pause spaces") != std::string::npos);
    const auto translation_with_hotwords = LLMTranslationClient::SystemPrompt("zh-Hans", {"小智", "VoiceStick"});
    assert(translation_with_hotwords.find("小智") != std::string::npos);
    assert(translation_with_hotwords.find("VoiceStick") != std::string::npos);

    // 请求体为合法 JSON：temperature:0、system+user 两条消息、model 透传。
    const auto payload = LLMChatClient::BuildChatPayload("gpt-x", "sys-prompt", "hello world");
    assert(payload.find("\"model\":\"gpt-x\"") != std::string::npos);
    assert(payload.find("\"temperature\":0") != std::string::npos);
    assert(payload.find("\"role\":\"system\"") != std::string::npos);
    assert(payload.find("\"role\":\"user\"") != std::string::npos);
    auto* root = cJSON_Parse(payload.c_str());
    assert(root != nullptr);
    auto* model = cJSON_GetObjectItemCaseSensitive(root, "model");
    assert(cJSON_IsString(model) && std::string(model->valuestring) == "gpt-x");
    auto* messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    assert(cJSON_IsArray(messages) && cJSON_GetArraySize(messages) == 2);
    auto* user_msg = cJSON_GetArrayItem(messages, 1);
    auto* user_content = cJSON_GetObjectItemCaseSensitive(user_msg, "content");
    assert(cJSON_IsString(user_content) && std::string(user_content->valuestring) == "hello world");
    cJSON_Delete(root);

    // disable_thinking=true 时注入两种风格的关思考参数，且 payload 仍为合法 JSON。
    const auto no_think_payload =
        LLMChatClient::BuildChatPayload("gpt-x", "sys-prompt", "hello world",
                                         /*stream=*/false, /*disable_thinking=*/true);
    assert(no_think_payload.find("\"enable_thinking\":false") != std::string::npos);
    assert(no_think_payload.find("\"chat_template_kwargs\":{\"enable_thinking\":false}") !=
           std::string::npos);
    auto* no_think_root = cJSON_Parse(no_think_payload.c_str());
    assert(no_think_root != nullptr);
    auto* enable_thinking = cJSON_GetObjectItemCaseSensitive(no_think_root, "enable_thinking");
    assert(cJSON_IsBool(enable_thinking) && !cJSON_IsTrue(enable_thinking));
    cJSON_Delete(no_think_root);

    // 精修默认关闭、prompt 默认空。llm_disable_thinking 默认开启（关闭模型深度思考）。
    assert(AppConfig::Defaults().refine_enabled == false);
    assert(AppConfig::Defaults().refine_prompt.empty());
    assert(AppConfig::Defaults().llm_disable_thinking == true);

    // refine_prompt 多行字符串 TOML 保存/加载往返测试：
    // 验证换行等控制字符被正确转义为 \n 等 TOML 转义序列，确保回读一致。
    {
        auto temp = std::filesystem::temp_directory_path() / "voicestick_refine_prompt_test.toml";
        std::filesystem::remove(temp);

        AppConfig config = AppConfig::Defaults();
        config.refine_enabled = true;
        // 含换行、tab、双引号和反斜杠的自定义 prompt
        config.refine_prompt =
            "你是一个后处理器。\n"
            "规则：\n"
            "\t• 去除\"多余\"空格\n"
            "\t• 修正标点\\格式\n"
            "仅返回文本。";
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.refine_enabled == true);
        assert(loaded.refine_prompt == config.refine_prompt);
        assert(loaded.refine_prompt.find("你是一个后处理器") != std::string::npos);
        assert(loaded.refine_prompt.find("\n\t• ") != std::string::npos);
        assert(loaded.refine_prompt.find("\"多余\"") != std::string::npos);
        assert(loaded.refine_prompt.find("标点\\格式") != std::string::npos);

        std::filesystem::remove(temp);
    }

    // refine_prompt 为空字符串时，保存为 "" 再加载仍为空（不落盘为带转义的多行）。
    {
        auto temp = std::filesystem::temp_directory_path() / "voicestick_refine_prompt_empty_test.toml";
        std::filesystem::remove(temp);

        AppConfig config = AppConfig::Defaults();
        config.refine_prompt.clear();
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.refine_prompt.empty());

        std::filesystem::remove(temp);
    }
}

void TestHotwordProcessConfig() {
    // 默认关闭、prompt 默认空（空 = 使用内置默认提示词）。
    assert(AppConfig::Defaults().hotword_process_enabled == false);
    assert(AppConfig::Defaults().hotword_process_prompt.empty());

    // TOML 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_hotword_process_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.hotword_process_enabled = true;
    config.hotword_process_prompt = "自定义提炼提示词\n第二行";
    config.Save(temp);
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.hotword_process_enabled == true);
    assert(loaded.hotword_process_prompt == config.hotword_process_prompt);
    std::filesystem::remove(temp);
}

void TestHotwordExtractorPromptAndParse() {
    // 内置默认提示词含提取语义关键词；覆盖值 Trim 后原样返回。
    const auto prompt = HotwordExtractor::BuildExtractPrompt("");
    assert(prompt.find("热词") != std::string::npos);
    assert(prompt.find("专有名词") != std::string::npos);
    const auto custom = HotwordExtractor::BuildExtractPrompt("  my extract prompt  ");
    assert(custom == "my extract prompt");

    // 解析：换行/逗号切分、Trim、去重（复用 ParseHotwordList 语义）。
    const auto words = HotwordExtractor::ParseExtractResult("小智\nVoiceStick\r\n小智\n豆包,AGI");
    assert((words == std::vector<std::string>{"小智", "VoiceStick", "豆包", "AGI"}));

    // 空输入 / 纯空白 → 空结果。
    assert(HotwordExtractor::ParseExtractResult("").empty());
    assert(HotwordExtractor::ParseExtractResult("  \n \n").empty());

    // 单词超过 64 字符被过滤。
    const std::string long_word(65, 'x');
    assert(HotwordExtractor::ParseExtractResult(long_word).empty());

    // 总量截断到 20 个。
    std::string many;
    for (int i = 0; i < 25; ++i) {
        many += "w" + std::to_string(i);
        many += "\n";
    }
    assert(HotwordExtractor::ParseExtractResult(many).size() == 20);

    // DiffNewHotwords：保序、剔除已存在词、自身去重。
    const auto diff = HotwordExtractor::DiffNewHotwords({"a", "b", "c", "a"}, {"b"});
    assert((diff == std::vector<std::string>{"a", "c"}));
    assert(HotwordExtractor::DiffNewHotwords({"b"}, {"b"}).empty());
}

void TestHotwordCandidateMiner() {
    // 标识符提取：字母开头 + 含大写/数字/._-；普通小写词、版本号、CJK 不算；
    // 尾部标点剥离。
    const auto tokens = ExtractIdentifierTokens(
        "编辑 AGENTS.md 和 hello world 文件，version 2.0 发布了 RamDisk、build_win.bat。");
    assert((tokens == std::vector<std::string>{"AGENTS.md", "RamDisk", "build_win.bat"}));
    assert(ExtractIdentifierTokens("plain english words only").empty());
    assert((ExtractIdentifierTokens("kAutoHideTimerId.") == std::vector<std::string>{"kAutoHideTimerId"}));

    // 挖掘：refined 新增且不在热词表的标识符。
    const auto mined = MineRefinementCandidates(
        "测试 agentsdmd 和 cloud dmd 文件", "测试 AGENTS.md 和 CLAUDE.md 文件。", {});
    assert((mined == std::vector<std::string>{"AGENTS.md", "CLAUDE.md"}));
    assert(MineRefinementCandidates("测试 agentsdmd 文件", "测试 AGENTS.md 文件。",
                                    {"AGENTS.md"}).empty());
    // 原文已有的词不重复挖掘。
    assert(MineRefinementCandidates("测试 RamDisk 文件", "测试 RamDisk 文件。", {}).empty());

    // 计数到阈值才建议；忽略词不建议；已通知词不重复建议。
    HotwordCandidateStore store;
    assert(RecordHotwordCandidates(store, {"AGENTS.md"}).empty());
    assert(RecordHotwordCandidates(store, {"AGENTS.md"}).empty());
    auto suggested = RecordHotwordCandidates(store, {"AGENTS.md", "CLAUDE.md"});
    assert((suggested == std::vector<std::string>{"AGENTS.md"}));
    assert(store.counts["AGENTS.md"] == 3);
    assert(store.counts["CLAUDE.md"] == 1);
    store.notified.insert("AGENTS.md");
    assert(RecordHotwordCandidates(store, {"AGENTS.md"}).empty());
    store.dismissed.insert("CLAUDE.md");
    assert(RecordHotwordCandidates(store, {"CLAUDE.md", "CLAUDE.md"}).empty());

    // 待确认列表：达阈值且未忽略。
    assert((PendingHotwordSuggestions(store) == std::vector<std::string>{"AGENTS.md"}));
    store.dismissed.insert("AGENTS.md");
    assert(PendingHotwordSuggestions(store).empty());

    // JSON 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_hotword_candidates_test.json";
    std::filesystem::remove(temp);
    HotwordCandidateStore round_trip;
    round_trip.counts["AGENTS.md"] = 5;
    round_trip.dismissed.insert("cloud dmd");
    round_trip.notified.insert("VoiceStick");
    SaveHotwordCandidates(temp, round_trip);
    const auto loaded = LoadHotwordCandidates(temp);
    assert(loaded.counts.at("AGENTS.md") == 5);
    assert(loaded.dismissed.contains("cloud dmd"));
    assert(loaded.notified.contains("VoiceStick"));
    std::filesystem::remove(temp);

    // 缺失文件返回空 store。
    assert(LoadHotwordCandidates(temp).counts.empty());
}

void TestHotwordCandidatesSingleWriter() {
    // B13：候选文件唯一写入口——coordinator 后台挖掘与设置页 UI 同写一个文件，原
    // coordinator load-once 缓存整存会用陈旧快照覆盖设置页刚写入的 dismissed，
    // 用户「忽略」的词反复弹回（评审 B13）。
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto dir = fs::temp_directory_path() / "voicestick_b13_candidates";
    fs::create_directories(dir, ec);
    const auto path = dir / "hotword_candidates.json";
    fs::remove(path, ec);

    // 挖掘三轮达阈值（kHotwordCandidateThreshold = 3）→ 第三轮给出建议。
    assert(RecordHotwordCandidatesToDisk(path, {"AlphaCode"}).empty());
    assert(RecordHotwordCandidatesToDisk(path, {"AlphaCode"}).empty());
    const auto suggested = RecordHotwordCandidatesToDisk(path, {"AlphaCode"});
    assert((suggested == std::vector<std::string>{"AlphaCode"}));
    assert((PendingHotwordSuggestions(LoadHotwordCandidates(path)) ==
            std::vector<std::string>{"AlphaCode"}));

    // 用户在设置页「忽略」
    assert(DismissHotwordCandidateOnDisk(path, "AlphaCode"));
    assert(PendingHotwordSuggestions(LoadHotwordCandidates(path)).empty());

    // 关键回归：后续挖掘不得让已忽略的词重新弹回（原 load-once 缓存会覆盖 dismissed）。
    const auto after = RecordHotwordCandidatesToDisk(path, {"AlphaCode", "FreshWord"});
    for (const auto& word : after) assert(word != "AlphaCode");
    assert(LoadHotwordCandidates(path).dismissed.contains("AlphaCode"));

    // 「加入」消费路径：从 counts/notified 移除。
    RecordHotwordCandidatesToDisk(path, {"FreshWord"});
    assert(ConsumeHotwordCandidateOnDisk(path, "FreshWord"));
    assert(!LoadHotwordCandidates(path).counts.contains("FreshWord"));

    // 写入原子性：保存后文件可立即解析（原 trunc 直写会读到半截 JSON）。
    for (int i = 0; i < 3; ++i) RecordHotwordCandidatesToDisk(path, {"AtomicCheck"});
    assert(LoadHotwordCandidates(path).counts.contains("AtomicCheck"));

    // C8b：路径单一出处——由配置路径推导，文件名不再散落各模块字面量
    //（原协调器/设置页4处硬编码，改名会静默脱钩）。
    const auto derived = HotwordCandidatesPath(dir / "config.toml");
    assert(derived.parent_path() == dir);
    assert(derived.filename() == "hotword_candidates.json");

    fs::remove_all(dir, ec);
}

void TestHotwordValidationUnified() {
    // B14：四套口径统一到 ValidateHotword——断言拒绝原因 + 既有两个布尔包装完全一致。
    assert(ValidateHotword("Opus") == HotwordRejectReason::kNone);
    assert(ValidateHotword("覃海洋") == HotwordRejectReason::kNone);
    assert(ValidateHotword("ESP32-S3") == HotwordRejectReason::kNone);
    assert(ValidateHotword("win_sparkle") == HotwordRejectReason::kNone);
    assert(ValidateHotword("") == HotwordRejectReason::kEmpty);
    assert(ValidateHotword("带空格 的词") == HotwordRejectReason::kWhitespace);
    assert(ValidateHotword("hello world") == HotwordRejectReason::kWhitespace);
    // 点号：平台权威**放行**（旗舰热词 CLAUDE.md/AGENTS.md 必须可用，与
    // hotword_select.py 对齐）；腾讯 API 单独收窄拒绝（唯一有意差异，同步处带词记录）。
    assert(ValidateHotword("CLAUDE.md") == HotwordRejectReason::kNone);
    assert(ValidateHotwordForTencent("CLAUDE.md") == HotwordRejectReason::kCharset);
    assert(ValidateHotword("hello.world") == HotwordRejectReason::kNone);
    assert(ValidateHotwordForTencent("hello.world") == HotwordRejectReason::kCharset);
    // 孤立续字节：原①当 seq_len=1 计入 CJK、原④直接放行——现统一拒绝非法 UTF-8。
    assert(ValidateHotword(std::string("a\x80z")) == HotwordRejectReason::kCharset);
    std::string cjk_too_long;  // 11 个真实 CJK 字符（勿用 std::string(n, '热')：多字节
    for (int i = 0; i < 11; ++i) {  // 字符字面量会被截断成单字节，验的就不是长度规则）
        cjk_too_long += "\xE7\x83\xAD";
    }
    assert(ValidateHotword(cjk_too_long) == HotwordRejectReason::kTooLong);
    assert(ValidateHotword(std::string(31, 'a')) == HotwordRejectReason::kTooLong);

    // 统一性对拍：两个既有包装分别等价于其委托口径；且腾讯口径必须是平台权威的
    // **超集**（只多拒绝、绝不少拒绝）——差异仅来自 API 收窄，且同步处已带词记录。
    const std::vector<std::string> corpus = {
        "Opus", "覃海洋", "ESP32-S3", "VB-CABLE", "win_sparkle", "CLAUDE.md",
        "AGENTS.md", "带空格 的词", "", "hello.world", "a-b_c",
        "Node.js", "中文中文中文中文中文中文", "tab\there", "ok_123", "..hidden"};
    for (const auto& word : corpus) {
        assert(IsValidHotword(word) == (ValidateHotword(word) == HotwordRejectReason::kNone));
        assert(TencentAsrVocabClient::IsValidHotwordChars(word) ==
               (ValidateHotwordForTencent(word) == HotwordRejectReason::kNone));
        if (!IsValidHotword(word)) assert(!TencentAsrVocabClient::IsValidHotwordChars(word));
    }
    // 点号词：平台放行、腾讯拒绝（唯一有意差异）。
    assert(IsValidHotword("hello.world"));
    assert(!TencentAsrVocabClient::IsValidHotwordChars("hello.world"));
}

void TestHotwordSelector() {
    const std::int64_t now = 1760000000;  // 固定 now，保证确定性

    // 评分：同 count 下手动词（source=manual）胜过挖掘词。
    HotwordUsage manual{};
    manual.count = 3;
    manual.last_used_ts = now;
    manual.source = "manual";
    HotwordUsage mined{};
    mined.count = 3;
    mined.last_used_ts = now;
    assert(HotwordScore(manual, now) > HotwordScore(mined, now));
    // 高频词（count=30）能胜过低计数手动词——频率优先的设计意图。
    HotwordUsage high{};
    high.count = 30;
    high.last_used_ts = now;
    HotwordUsage low_manual{};
    low_manual.count = 0;
    low_manual.last_used_ts = now;
    low_manual.source = "manual";
    assert(HotwordScore(high, now) > HotwordScore(low_manual, now));
    // 未知时间戳（0）新近度记 0：只剩 count 与 manual 项。
    HotwordUsage unknown_ts{};
    unknown_ts.count = 0;
    unknown_ts.source = "manual";
    assert(HotwordScore(unknown_ts, now) == kHotwordWManual);

    // 合法性过滤：含空白 / 超长词被过滤。
    assert(IsValidHotword("Opus"));
    assert(IsValidHotword("覃海洋"));
    assert(IsValidHotword("ESP32-S3"));
    assert(!IsValidHotword(""));
    assert(!IsValidHotword("带空格 的词"));
    assert(!IsValidHotword(std::string(11, '热')));  // 11 个汉字超限
    assert(!IsValidHotword(std::string(31, 'a')));   // 31 个 ASCII 超限

    // 排序：评分降序；同分按字典序；库外词按 manual 处理。
    HotwordUsageStore store;
    HotwordUsage opus{};
    opus.count = 3;
    opus.last_used_ts = now;
    store["Opus"] = opus;
    HotwordUsage vs{};
    vs.count = 3;
    vs.last_used_ts = now;
    vs.source = "manual";
    store["VoiceStick"] = vs;
    const auto ranked = RankHotwords(store, {"Opus", "VoiceStick", "带空格 的词"}, now);
    assert((ranked == std::vector<std::string>{"VoiceStick", "Opus"}));
    // 库外词（CLAUDE.md）按 manual（score=2.0），高于同 count 的 mined 词。
    HotwordUsage mined3{};
    mined3.count = 3;
    mined3.last_used_ts = now;
    store["BLE"] = mined3;
    const auto ranked2 = RankHotwords(store, {"BLE", "CLAUDE.md"}, now);
    assert((ranked2 == std::vector<std::string>{"CLAUDE.md", "BLE"}));
    // 同分（count/时间戳/source 全同）按字典序稳定。
    HotwordUsageStore equal_store;
    HotwordUsage same{};
    same.count = 1;
    same.last_used_ts = now;
    equal_store["zeta"] = same;
    equal_store["alpha"] = same;
    const auto ranked3 = RankHotwords(equal_store, {"zeta", "alpha"}, now);
    assert((ranked3 == std::vector<std::string>{"alpha", "zeta"}));

    // 预算裁剪：按评分序装入，高分词在前且累计不超预算。
    std::vector<std::string> many;
    HotwordUsageStore big_store;
    for (int i = 0; i < 100; ++i) {
        const std::string word = "术语" + std::to_string(i);  // 每词 2 tokens
        many.push_back(word);
        HotwordUsage u{};
        u.count = 100 - i;  // 编号小的词频更高
        u.last_used_ts = now;
        big_store[word] = u;
    }
    const auto ranked_many = RankHotwords(big_store, many, now);
    const auto fitted = AsrProtocol::FitHotwordsToCorpusBudget(ranked_many);
    int used = 0;
    for (const auto& word : fitted) used += AsrProtocol::EstimateHotwordTokens(word);
    assert(used <= AsrProtocol::kHotwordCorpusTokenBudget);
    assert(fitted.size() < ranked_many.size());
    assert(ranked_many.front() == "术语0");
    assert(fitted.front() == "术语0");

    // prompt 段上限：top-N 且保持评分序。
    const auto prompt_words = TrimHotwordsForPrompt(big_store, many, kHotwordPromptMaxWords, now);
    assert(static_cast<int>(prompt_words.size()) == kHotwordPromptMaxWords);
    assert(prompt_words.front() == "术语0");

    // 使用统计记录：命中热词计数+刷新时间戳；未命中的不产生条目。
    HotwordUsageStore usage_store;
    RecordHotwordUsageInText(usage_store, "编辑 AGENTS.md 和 CLAUDE.md 文件", {"AGENTS.md", "BLE"}, now);
    assert(usage_store.at("AGENTS.md").count == 1);
    assert(usage_store.at("AGENTS.md").last_used_ts == now);
    assert(usage_store.at("AGENTS.md").source == "manual");
    assert(!usage_store.contains("BLE"));
    // 大小写不敏感。
    RecordHotwordUsageInText(usage_store, "编辑 agents.md 文件", {"AGENTS.md"}, now + 10);
    assert(usage_store.at("AGENTS.md").count == 2);
    assert(usage_store.at("AGENTS.md").last_used_ts == now + 10);

    // JSON 往返（与 hotword_select.py load_stats_json 列表形态兼容）。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_hotword_usage_test.json";
    std::filesystem::remove(temp);
    SaveHotwordUsage(temp, usage_store);
    const auto loaded = LoadHotwordUsage(temp);
    assert(loaded.at("AGENTS.md").count == 2);
    assert(loaded.at("AGENTS.md").last_used_ts == now + 10);
    assert(loaded.at("AGENTS.md").source == "manual");
    std::filesystem::remove(temp);
    // 缺失/损坏文件返回空 store。
    assert(LoadHotwordUsage(temp).empty());
    auto bad_temp = std::filesystem::temp_directory_path() / "voicestick_hotword_usage_bad.json";
    std::filesystem::remove(bad_temp);
    {
        std::ofstream f(bad_temp, std::ios::binary);
        f << "{not-json";
    }
    assert(LoadHotwordUsage(bad_temp).empty());
    std::filesystem::remove(bad_temp);
}

void TestHotwordExtractionPromptAndParse() {
    // prompt 附已知热词表（"已知热词表："列表行）；空表时不附加（注意基础规则文本里
    // 含「已知热词表中的条目」字样，断言要匹配带冒号的列表行）。
    const auto prompt = LLMRefinementClient::BuildHotwordExtractionPrompt({"AGENTS.md", "DeepSeek"});
    assert(prompt.find("AGENTS.md") != std::string::npos);
    assert(prompt.find("JSON") != std::string::npos);
    assert(prompt.find("已知热词表：") != std::string::npos);
    assert(LLMRefinementClient::BuildHotwordExtractionPrompt({}).find("已知热词表：") == std::string::npos);

    const std::string source = "我们研究一下 Stack Chain 是怎么使用的，顺便问问 DeepSeek。";
    const std::vector<std::string> hotwords = {"DeepSeek"};

    // 正常 JSON 数组：过滤已在热词表的 DeepSeek，保留 Stack Chain。
    auto words = LLMRefinementClient::ParseHotwordExtractionResponse(
        "[\"Stack Chain\", \"DeepSeek\"]", source, hotwords);
    assert((words == std::vector<std::string>{"Stack Chain"}));

    // 前后裹解释文字也能容错解析。
    words = LLMRefinementClient::ParseHotwordExtractionResponse(
        "候选如下：[\"Stack Chain\"] 以上。", source, hotwords);
    assert((words == std::vector<std::string>{"Stack Chain"}));

    // 无候选 / 垃圾输出 / 非字符串元素 → 空。
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[]", source, hotwords).empty());
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("没有候选", source, hotwords).empty());
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[1, true, null]", source, hotwords).empty());

    // 防臆造：不在原文出现的词被丢弃。
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[\"OpenAI\"]", source, {}).empty());

    // 忽略大小写：原文中的 "Stack Chain" 能匹配候选 "stack chain"；
    // 与热词仅大小写不同也算重复（"deepseek" 命中热词 "DeepSeek"）。
    words = LLMRefinementClient::ParseHotwordExtractionResponse("[\"stack chain\"]", source, hotwords);
    assert((words == std::vector<std::string>{"stack chain"}));
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[\"deepseek\"]", source, hotwords).empty());

    // 过短 / 超长 / 超 3 词被过滤；同词大小写去重只留第一个。
    const std::string words_source = "alpha beta gamma delta";
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[\"x\"]", words_source, {}).empty());
    assert(LLMRefinementClient::ParseHotwordExtractionResponse(
               "[\"alpha beta gamma delta\"]", words_source, {}).empty());
    assert((LLMRefinementClient::ParseHotwordExtractionResponse("[\"alpha beta\"]", words_source, {})
            == std::vector<std::string>{"alpha beta"}));
    const std::string long_source = "abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz";
    assert(LLMRefinementClient::ParseHotwordExtractionResponse(
               "[\"abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz\"]", long_source, {}).empty());
    assert((LLMRefinementClient::ParseHotwordExtractionResponse(
                "[\"Stack Chain\", \"stack chain\"]", source, hotwords)
            == std::vector<std::string>{"Stack Chain"}));

    // 空白容忍：ASR 英文空格形态不稳定（双空格 / 连写），LLM 规范化输出不应被
    // 防臆造误杀（生产环境 candidates=0 的已证实根因之一）。
    const std::string double_space_source = "我刚才讲了 Stack  Chain 这个新词";
    assert((LLMRefinementClient::ParseHotwordExtractionResponse(
                "[\"Stack Chain\"]", double_space_source, hotwords)
            == std::vector<std::string>{"Stack Chain"}));
    const std::string concat_source = "我刚才讲了 StackChain 这个新词";
    assert((LLMRefinementClient::ParseHotwordExtractionResponse(
                "[\"Stack Chain\"]", concat_source, hotwords)
            == std::vector<std::string>{"Stack Chain"}));

    // 统计回填：bracket/json/items/各拒绝原因计数（只计数不记文本）。
    LLMRefinementClient::HotwordExtractionStats stats;
    words = LLMRefinementClient::ParseHotwordExtractionResponse(
        "[\"Stack Chain\", \"DeepSeek\", \"OpenAI\", \"x\"]", source, hotwords, &stats);
    assert((words == std::vector<std::string>{"Stack Chain"}));
    assert(stats.bracket_found && stats.json_ok && stats.items == 4);
    assert(stats.rejected_hotword == 1 && stats.rejected_not_in_text == 1 &&
           stats.rejected_len == 1);
    stats = {};
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("没有候选", source, hotwords, &stats)
               .empty());
    assert(!stats.bracket_found && !stats.json_ok);
    stats = {};
    assert(LLMRefinementClient::ParseHotwordExtractionResponse("[not json]", source, hotwords, &stats)
               .empty());
    assert(stats.bracket_found && !stats.json_ok);

    // hotword_mining_enabled 配置往返，默认关闭。
    assert(!AppConfig::Defaults().hotword_mining_enabled);
    auto temp = std::filesystem::temp_directory_path() / "voicestick_hotword_mining_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.hotword_mining_enabled = true;
    config.Save(temp);
    assert(AppConfig::Load(temp).hotword_mining_enabled);
    std::filesystem::remove(temp);
}

void TestFirmwareManifestParsingAndVersionCompare() {
    const std::string json =
        "{\"hardware\":\"sticks3\",\"version\":\"0.2.3\",\"ota_url\":\"https://example.test/ota.bin\","
        "\"ota_sha256\":\"abc\",\"ota_size\":123,\"merged_url\":\"https://example.test/merged.bin\","
        "\"merged_sha256\":\"def\",\"merged_size\":456}";
    auto manifest = ParseFirmwareManifest(json);
    assert(manifest.has_value());
    assert(manifest->hardware == "sticks3");
    assert(manifest->version == "0.2.3");
    assert(manifest->ota_size == 123);
    assert(FirmwareVersion::IsOlderThan("0.2.2", "0.2.3"));
    assert(FirmwareVersion::IsOlderThan("0.2.3-beta", "0.2.3"));
    assert(!FirmwareVersion::IsOlderThan("0.2.3", "0.2.3-beta"));
    assert(IsFirmwareHardwareCompatible("sticks3", "0.1.2", "stick_s3"));
    assert(IsFirmwareHardwareCompatible("", "0.1.2", "stick_s3"));
    assert(IsFirmwareHardwareCompatible("", "", "stick_s3"));
}

void TestFirmwareVersionC6Robustness() {
    // C6 回归①：预发布后缀按「前缀 + 数字」比较——rc10 必须新于 rc9。
    // 原字典序因 '1' < '9' 会把 rc10 判成比 rc9 旧，导致 rc10 用户收不到 rc11 之前的
    // 正确顺序判断（发布序颠倒）。
    assert(FirmwareVersion::IsOlderThan("1.0.0-rc9", "1.0.0-rc10"));
    assert(!FirmwareVersion::IsOlderThan("1.0.0-rc10", "1.0.0-rc9"));
    assert(!FirmwareVersion::IsOlderThan("1.0.0-rc10", "1.0.0-rc10"));
    assert(FirmwareVersion::IsOlderThan("1.0.0-beta9", "1.0.0-rc1"));  // 前缀序
    assert(FirmwareVersion::IsOlderThan("1.0.0-rc", "1.0.0-rc1"));     // 无数字后缀=0

    // C6 回归②：解析失败不得 fail-open 成「已是最新」——原实现返回 false，
    // 坏 manifest 版本串 / 设备上报异常格式会让升级提示与最低版本门永不触发。
    assert(FirmwareVersion::IsOlderThan("", "2.4.9"));
    assert(FirmwareVersion::IsOlderThan("garbage", "2.4.9"));
    assert(FirmwareVersion::IsOlderThan("2.4.9", ""));
    // 正常路径不受影响
    assert(!FirmwareVersion::IsOlderThan("2.4.9", "2.4.9"));
    assert(FirmwareVersion::IsOlderThan("2.4.8", "2.4.9"));
}

void TestFirmwareManifestMinimumVersion() {
    // Arrange：新版 manifest 下发 min_version
    const std::string json_with_min =
        "{\"hardware\":\"sticks3\",\"version\":\"2.3.7\","
        "\"ota_url\":\"https://example.test/ota.bin\",\"ota_sha256\":\"abc\",\"ota_size\":123,"
        "\"min_version\":\"2.0.0\"}";
    auto manifest = ParseFirmwareManifest(json_with_min);
    assert(manifest.has_value());
    assert(manifest->min_version == "2.0.0");
    assert(EffectiveMinimumFirmwareVersion(*manifest, "0.3.0") == "2.0.0");

    // 旧 Release manifest 无 min_version → 解析为空，回退本地 fallback
    const std::string json_without_min =
        "{\"hardware\":\"sticks3\",\"version\":\"2.3.7\","
        "\"ota_url\":\"https://example.test/ota.bin\",\"ota_sha256\":\"abc\",\"ota_size\":123}";
    auto legacy = ParseFirmwareManifest(json_without_min);
    assert(legacy.has_value());
    assert(legacy->min_version.empty());
    assert(EffectiveMinimumFirmwareVersion(*legacy, "0.3.0") == "0.3.0");

    // 强制升级判定：current < min_version → below minimum
    assert(FirmwareVersion::IsOlderThan("1.9.9", EffectiveMinimumFirmwareVersion(*manifest, "0.3.0")));
    // 可选升级判定：min_version ≤ current < version → 可更新但不强制
    assert(!FirmwareVersion::IsOlderThan("2.0.0", EffectiveMinimumFirmwareVersion(*manifest, "0.3.0")));
    assert(FirmwareVersion::IsOlderThan("2.0.0", manifest->version));

    // 升级紧急度分类：manifest 下发 min_version 时按其判定
    assert(ClassifyFirmwareUpdateUrgency("1.9.9", *manifest, "0.3.0") ==
           FirmwareUpdateUrgency::kRequired);
    assert(ClassifyFirmwareUpdateUrgency("2.0.0", *manifest, "0.3.0") ==
           FirmwareUpdateUrgency::kOptional);
    assert(ClassifyFirmwareUpdateUrgency("2.3.6", *manifest, "0.3.0") ==
           FirmwareUpdateUrgency::kOptional);
    assert(ClassifyFirmwareUpdateUrgency("2.3.7", *manifest, "0.3.0") ==
           FirmwareUpdateUrgency::kUpToDate);
    // manifest 缺 min_version → 回退本地常量判定（0.3.0 以下强制）
    assert(ClassifyFirmwareUpdateUrgency("0.2.9", *legacy, "0.3.0") ==
           FirmwareUpdateUrgency::kRequired);
    assert(ClassifyFirmwareUpdateUrgency("0.3.0", *legacy, "0.3.0") ==
           FirmwareUpdateUrgency::kOptional);
}

void TestFirmwareManifestFallbackUrls() {
    // Arrange：COS 分发面新 schema，主 URL 指国内源，*_fallback 指 GitHub Release
    const std::string json_with_fallback =
        "{\"hardware\":\"sticks3\",\"version\":\"2.3.10\","
        "\"ota_url\":\"https://dl.davenger.cloud/firmware/v2.3.10/ota.bin\","
        "\"ota_url_fallback\":\"https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.10/ota.bin\","
        "\"ota_sha256\":\"abc\",\"ota_size\":123,"
        "\"merged_url\":\"https://dl.davenger.cloud/firmware/v2.3.10/merged.bin\","
        "\"merged_url_fallback\":\"https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.10/merged.bin\","
        "\"merged_sha256\":\"def\",\"merged_size\":456}";
    auto manifest = ParseFirmwareManifest(json_with_fallback);
    assert(manifest.has_value());
    assert(manifest->ota_url_fallback ==
           "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.10/ota.bin");
    assert(manifest->merged_url_fallback ==
           "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.10/merged.bin");

    // OTA 下载序列：主源在前，回退源在后
    auto urls = OtaDownloadUrls(*manifest);
    assert(urls.size() == 2);
    assert(urls[0] == manifest->ota_url);
    assert(urls[1] == manifest->ota_url_fallback);

    // 旧 Release manifest 无 fallback 字段 → 容错为空，下载序列仅主源
    const std::string json_without_fallback =
        "{\"hardware\":\"sticks3\",\"version\":\"2.3.9\","
        "\"ota_url\":\"https://dl.davenger.cloud/firmware/v2.3.9/ota.bin\","
        "\"ota_sha256\":\"abc\",\"ota_size\":123}";
    auto legacy = ParseFirmwareManifest(json_without_fallback);
    assert(legacy.has_value());
    assert(legacy->ota_url_fallback.empty());
    assert(legacy->merged_url_fallback.empty());
    auto legacy_urls = OtaDownloadUrls(*legacy);
    assert(legacy_urls.size() == 1);
    assert(legacy_urls[0] == legacy->ota_url);

    // fallback 字段类型错误（数字而非字符串）→ 按缺失处理
    const std::string json_bad_fallback =
        "{\"hardware\":\"sticks3\",\"version\":\"2.3.10\","
        "\"ota_url\":\"https://dl.davenger.cloud/ota.bin\","
        "\"ota_url_fallback\":42,"
        "\"ota_sha256\":\"abc\",\"ota_size\":123}";
    auto bad = ParseFirmwareManifest(json_bad_fallback);
    assert(bad.has_value());
    assert(bad->ota_url_fallback.empty());

    // 默认客户端源：主源为国内 COS 域名，回退为 GitHub Release
    const auto primary = FirmwareManifestClient::DefaultManifestUrl();
    assert(primary.find("https://dl.davenger.cloud/firmware/latest/manifest.json") == 0);
    const auto fallback = FirmwareManifestClient::FallbackManifestUrl();
    assert(fallback.find("https://github.com/") == 0);
    assert(fallback.find("/releases/latest/download/manifest.json") != std::string::npos);
}

void TestCoordinatorSyncsImuWakeSensitivityOnConnectionAndConfigUpdate() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.imu_wake_sensitivity = ImuWakeSensitivity::kHigh;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    assert(!ble_ptr->sent_imu_wake_sensitivities.empty());
    assert(ble_ptr->sent_imu_wake_sensitivities.back().threshold_lsb == 250);
    // 设备交互设置按设备单播（IMU 唤醒灵敏度现属于设备级 InteractionSettings）。
    assert(ble_ptr->sent_imu_wake_sensitivities.back().device_id == "5A74");

    AppConfig updated = config;
    updated.default_interaction_settings.imu_wake_sensitivity = ImuWakeSensitivity::kMedium;
    coordinator.UpdateConfig(updated);

    assert(ble_ptr->sent_imu_wake_sensitivities.back().threshold_lsb == 500);
    assert(ble_ptr->sent_imu_wake_sensitivities.back().device_id == "5A74");
}

void TestCoordinatorUpdateFirmwareFromFile() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "vs_local_fw_test.bin";
    const ByteVector data{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }

    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    // 正常路径：读到的字节原样喂底层，device_id 透传。
    bool ok = false;
    coordinator.UpdateFirmwareFromFile(path.string(), "5A74", {},
        [&](bool s, std::string) { ok = s; });
    assert(ok);
    assert(ble_ptr->captured_firmware_image == data);
    assert(ble_ptr->captured_firmware_device_id == "5A74");

    // 不存在文件：completion(false)，不触达底层（captured 维持上次成功值）。
    bool ok2 = true;
    coordinator.UpdateFirmwareFromFile("nonexistent_vs_fw.bin", "5A74", {},
        [&](bool s, std::string) { ok2 = s; });
    assert(!ok2);
    assert(ble_ptr->captured_firmware_device_id == "5A74");

    // 空文件：completion(false)，不触达底层。
    const std::filesystem::path empty_path =
        std::filesystem::temp_directory_path() / "vs_local_fw_empty.bin";
    { std::ofstream f(empty_path, std::ios::binary); }
    bool ok3 = true;
    coordinator.UpdateFirmwareFromFile(empty_path.string(), "5A74", {},
        [&](bool s, std::string) { ok3 = s; });
    assert(!ok3);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(empty_path, ec);
}

// OTA 发送在途窗口选择：v2.3.8 及更早固件的进度回传间隔是 32KB，未收到首条
// 确认前窗口必须能覆盖它，否则 app 灌到 24KB 就停下等确认、设备攒不到 32KB 不
// 回传，双方互等直到 15s stalled（2026-09-26 真机：confirmed 恒为 0，v2.4.0
// bin 无法推给 v2.3.8 设备）。确认开始流动后回到 24KB 常规窗口（8KB 回传间隔
// 的 3 倍余量，2026-09-20 定的流控节拍）。
void TestOtaMaxInFlightBytes() {
    // 未收到任何确认：窗口须大于旧固件 32KB 回传间隔（含一个 chunk 的余量）。
    assert(BleProtocol::OtaMaxInFlightBytes(0) > 32 * 1024);
    // 任意非零确认（哪怕只有 1 字节）都证明回传通道活着：立即收紧常规窗口。
    assert(BleProtocol::OtaMaxInFlightBytes(1) == 24 * 1024);
    assert(BleProtocol::OtaMaxInFlightBytes(8 * 1024) == 24 * 1024);
}

void TestOtaChunkSizeForPdu() {
    // D6：分块 = max_pdu −15（12B 帧头 +3B ATT），**无下限兜底**——原 max(20,…) 在
    // MTU 退化（pdu≤20）时构造出超过可写上限的包，固件必 bad_offset。
    assert(BleProtocol::OtaChunkSizeForPdu(20) == 5);    // MTU23 退化：20-15（原返回 20 ✗）
    assert(BleProtocol::OtaChunkSizeForPdu(15) == 0);    // 恰好放不下 → 报错
    assert(BleProtocol::OtaChunkSizeForPdu(14) == 0);
    assert(BleProtocol::OtaChunkSizeForPdu(16) == 1);
    assert(BleProtocol::OtaChunkSizeForPdu(247) == 232);   // 247-15
    assert(BleProtocol::OtaChunkSizeForPdu(1000) == 244);  // chunk 上限
    // 自检：包长（chunk + 15）永不超过 PDU 预算。
    for (const std::size_t pdu : {16u, 20u, 247u, 512u}) {
        const std::size_t chunk = BleProtocol::OtaChunkSizeForPdu(pdu);
        assert(chunk > 0 && chunk + 15 <= pdu);
    }
}

void TestParseOtaCliArgs() {
    using namespace voicestick;
    // 无 --ota。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe"};
        assert(!ParseOtaCliArgs(1, argv).has_value());
    }
    // --ota 带路径，无 --device。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/fw.bin"};
        auto r = ParseOtaCliArgs(3, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/fw.bin");
        assert(!r->device_id.has_value());
    }
    // --ota + --device。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/fw.bin",
                                 L"--device", L"5A74"};
        auto r = ParseOtaCliArgs(5, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/fw.bin");
        assert(r->device_id.has_value());
        assert(*r->device_id == "5A74");
    }
    // --device 在前，顺序无关。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--device", L"5A74",
                                 L"--ota", L"C:/fw.bin"};
        auto r = ParseOtaCliArgs(5, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/fw.bin");
        assert(*r->device_id == "5A74");
    }
    // --ota 缺路径 -> nullopt。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota"};
        assert(!ParseOtaCliArgs(2, argv).has_value());
    }
    // 中文路径转 UTF-8。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/固件.bin"};
        auto r = ParseOtaCliArgs(3, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/固件.bin");
    }
    // --device 缺值但 --ota 正常 -> 忽略 --device。
    {
        const wchar_t* argv[] = {L"VoiceStick.exe", L"--ota", L"C:/fw.bin", L"--device"};
        auto r = ParseOtaCliArgs(4, argv);
        assert(r.has_value());
        assert(r->file_path == "C:/fw.bin");
        assert(!r->device_id.has_value());
    }
}

void TestCoordinatorSyncsTapSensitivityOnConnectionAndConfigUpdate() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.tap_sensitivity = 7;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    assert(!ble_ptr->sent_tap_sensitivities.empty());
    assert(ble_ptr->sent_tap_sensitivities.back().level == 7);
    // 设备交互设置按设备单播（敲击灵敏度现属于设备级 InteractionSettings）。
    assert(ble_ptr->sent_tap_sensitivities.back().device_id == "5A74");

    AppConfig updated = config;
    updated.default_interaction_settings.tap_sensitivity = 3;
    coordinator.UpdateConfig(updated);

    assert(ble_ptr->sent_tap_sensitivities.back().level == 3);
    assert(ble_ptr->sent_tap_sensitivities.back().device_id == "5A74");
}

void TestBleEncoderPayloads() {
    auto led = BleProtocol::EncoderLedColorPayload("cyan");
    assert(std::string(led.begin(), led.end()) == "{\"event\":\"encoder_led_color\",\"color\":\"cyan\"}");
    auto led_off = BleProtocol::EncoderLedColorPayload("off");
    assert(std::string(led_off.begin(), led_off.end()) == "{\"event\":\"encoder_led_color\",\"color\":\"off\"}");

    auto gate_off = BleProtocol::EncoderRecordingGatePayload(false);
    assert(std::string(gate_off.begin(), gate_off.end()) == "{\"event\":\"encoder_recording_gate\",\"enabled\":false}");
    auto gate_on = BleProtocol::EncoderRecordingGatePayload(true);
    assert(std::string(gate_on.begin(), gate_on.end()) == "{\"event\":\"encoder_recording_gate\",\"enabled\":true}");
}

// 网关按键路由（P1）：连接/配置变化时对 StickS3 设备逐键下发软件路由。
// 无配对 RC 设备时取全局默认 [xiaomi.keys]（网关模式遥控器不直连桌面端的口径）。
void TestCoordinatorPushesGatewayKeymapRoutes() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_xiaomi_settings.key_map["back"] = "ctrl+alt+d";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    // 连接即下发：桌面端可映射 12 键各一条（kXiaomiMappableButtons，无 mic/
    // volume_mute），back 有映射→software，其余→passthrough。
    ble_ptr->connected_device_ids.insert("53A8");
    ble_ptr->on_connection_change(
        {ConnectedDevice{"53A8", "VS-53A8", std::string(kHardwareStickS3)}});
    assert(ble_ptr->sent_gateway_keymap_sets.size() == 12);
    bool back_software = false;
    for (const auto& sent : ble_ptr->sent_gateway_keymap_sets) {
        assert(sent.device_id.has_value());
        assert(*sent.device_id == "53A8");
        if (sent.key == "back") {
            back_software = sent.software;
        } else {
            assert(!sent.software);
        }
    }
    assert(back_software);

    // 配置热更（对话框保存路径）：映射变化逐键重发——back 清除、tv 新增。
    AppConfig updated = AppConfig::Defaults();
    updated.default_xiaomi_settings.key_map["tv"] = "f5";
    coordinator.UpdateConfig(updated);
    assert(ble_ptr->sent_gateway_keymap_sets.size() == 24);
    bool tv_software = false;
    bool back_passthrough = false;
    // 只查第二批（前 12 条属第一批：back 当时确为 software）。
    for (std::size_t k = 12; k < ble_ptr->sent_gateway_keymap_sets.size(); ++k) {
        const auto& sent = ble_ptr->sent_gateway_keymap_sets[k];
        if (sent.key == "tv") {
            tv_software = sent.software;
        } else if (sent.key == "back") {
            back_passthrough = !sent.software;  // 已清除 → 恢复直通
        }
    }
    assert(tv_software);
    assert(back_passthrough);
}

void TestCoordinatorSyncsEncoderSettingsOnConnectionAndConfigUpdate() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.led_color = "purple";
    config.default_encoder_settings.press_action = "key";  // 派生门控关闭
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    // 连接时按设备单播有效配置：无覆盖设备收到全局默认值。
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    assert(ble_ptr->sent_encoder_led_colors.size() == 1);
    assert(ble_ptr->sent_encoder_led_colors.back().first == "purple");
    assert(ble_ptr->sent_encoder_led_colors.back().second.has_value());
    assert(*ble_ptr->sent_encoder_led_colors.back().second == "5A74");
    assert(ble_ptr->sent_encoder_recording_gates.size() == 1);
    assert(ble_ptr->sent_encoder_recording_gates.back().first == false);
    assert(ble_ptr->sent_encoder_recording_gates.back().second.has_value());
    assert(*ble_ptr->sent_encoder_recording_gates.back().second == "5A74");

    // UpdateConfig 对已连接设备逐台单播；press_action=recording 派生门控打开。
    AppConfig updated = AppConfig::Defaults();
    updated.default_encoder_settings.led_color = "green";
    updated.default_encoder_settings.press_action = "recording";
    coordinator.UpdateConfig(updated);
    assert(ble_ptr->sent_encoder_led_colors.size() == 2);
    assert(ble_ptr->sent_encoder_led_colors.back().first == "green");
    assert(ble_ptr->sent_encoder_led_colors.back().second.has_value());
    assert(*ble_ptr->sent_encoder_led_colors.back().second == "5A74");
    assert(ble_ptr->sent_encoder_recording_gates.size() == 2);
    assert(ble_ptr->sent_encoder_recording_gates.back().first == true);
    assert(ble_ptr->sent_encoder_recording_gates.back().second.has_value());
    assert(*ble_ptr->sent_encoder_recording_gates.back().second == "5A74");
}

void TestCoordinatorSyncsEncoderSettingsPerDeviceOverride() {
    // 按设备覆盖：连接后每台设备收到各自的 led_color / recording_gate。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_encoder_settings.led_color = "red";
    config.default_encoder_settings.press_action = "recording";
    config.paired_device_ids = {"5A74", "9BC1"};
    EncoderSettings override_settings;
    override_settings.led_color = "blue";
    override_settings.press_action = "key";
    config.device_encoder_settings["9BC1"] = override_settings;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->connected_device_ids.insert("9BC1");
    ble_ptr->on_connection_change(
        {ConnectedDevice{"5A74", "VS-5A74"}, ConnectedDevice{"9BC1", "VS-9BC1"}});
    assert(ble_ptr->sent_encoder_led_colors.size() == 2);
    assert(ble_ptr->sent_encoder_recording_gates.size() == 2);
    // 5A74 无覆盖 → 全局默认 red + 门控开；9BC1 覆盖 → blue + 门控关。
    for (const auto& [color, target] : ble_ptr->sent_encoder_led_colors) {
        assert(target.has_value());
        if (*target == "5A74") {
            assert(color == "red");
        } else {
            assert(*target == "9BC1");
            assert(color == "blue");
        }
    }
    for (const auto& [gate, target] : ble_ptr->sent_encoder_recording_gates) {
        assert(target.has_value());
        if (*target == "5A74") {
            assert(gate == true);
        } else {
            assert(*target == "9BC1");
            assert(gate == false);
        }
    }
}

void TestCoordinatorSyncsInteractionSettingsPerDeviceOverride() {
    // 设备交互设置按设备覆盖：连接后每台设备收到各自的有效配置（tap_to_arrow / 灵敏度 / IMU 唤醒）。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.default_interaction_settings.tap_to_arrow = true;
    config.default_interaction_settings.tap_sensitivity = 7;
    config.default_interaction_settings.imu_wake_sensitivity = ImuWakeSensitivity::kHigh;
    config.paired_device_ids = {"5A74", "9BC1"};
    InteractionSettings override;
    override.tap_to_arrow = false;
    override.tap_sensitivity = 3;
    override.imu_wake_sensitivity = ImuWakeSensitivity::kLow;
    config.device_interaction_settings["9BC1"] = override;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->connected_device_ids.insert("9BC1");
    ble_ptr->on_connection_change(
        {ConnectedDevice{"5A74", "VS-5A74"}, ConnectedDevice{"9BC1", "VS-9BC1"}});

    // tap_to_arrow：5A74 默认 true，9BC1 覆盖 false，均按设备单播。
    assert(ble_ptr->sent_tap_enabled.size() == 2);
    for (const auto& [enabled, target] : ble_ptr->sent_tap_enabled) {
        assert(target.has_value());
        if (*target == "5A74") {
            assert(enabled == true);
        } else {
            assert(*target == "9BC1");
            assert(enabled == false);
        }
    }

    // tap_sensitivity：5A74=7，9BC1=3。
    assert(ble_ptr->sent_tap_sensitivities.size() == 2);
    for (const auto& sent : ble_ptr->sent_tap_sensitivities) {
        assert(sent.device_id.has_value());
        if (*sent.device_id == "5A74") {
            assert(sent.level == 7);
        } else {
            assert(*sent.device_id == "9BC1");
            assert(sent.level == 3);
        }
    }

    // imu_wake_sensitivity：5A74=kHigh(250)，9BC1 覆盖 kLow(800)。
    assert(ble_ptr->sent_imu_wake_sensitivities.size() == 2);
    for (const auto& sent : ble_ptr->sent_imu_wake_sensitivities) {
        assert(sent.device_id.has_value());
        if (*sent.device_id == "5A74") {
            assert(sent.threshold_lsb == 250);
        } else {
            assert(*sent.device_id == "9BC1");
            assert(sent.threshold_lsb == 800);
        }
    }
}

void TestAppConfigTapSensitivityRoundTrip() {
    // 默认档 5。
    assert(AppConfig::Defaults().default_interaction_settings.tap_sensitivity == 5);

    // 钳位：越界值回退默认档 5，合法值透传。
    assert(TapSensitivityClamp(0) == 5);
    assert(TapSensitivityClamp(11) == 5);
    assert(TapSensitivityClamp(-1) == 5);
    assert(TapSensitivityClamp(1) == 1);
    assert(TapSensitivityClamp(10) == 10);
    assert(TapSensitivityClamp(3) == 3);

    // TOML 保存/加载往返。
    {
        auto temp = std::filesystem::temp_directory_path() / "voicestick_tap_sensitivity_test.toml";
        std::filesystem::remove(temp);

        AppConfig config = AppConfig::Defaults();
        config.default_interaction_settings.tap_sensitivity = 8;
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.default_interaction_settings.tap_sensitivity == 8);

        std::filesystem::remove(temp);
    }

    // 越界值落盘后回读应被钳位到默认档 5。
    {
        auto temp = std::filesystem::temp_directory_path() / "voicestick_tap_sensitivity_clamp_test.toml";
        std::filesystem::remove(temp);

        AppConfig config = AppConfig::Defaults();
        config.default_interaction_settings.tap_sensitivity = 99;
        config.Save(temp);

        AppConfig loaded = AppConfig::Load(temp);
        assert(loaded.default_interaction_settings.tap_sensitivity == 5);

        std::filesystem::remove(temp);
    }
}

void TestCoordinatorHotkeyWithoutConnectionShowsWakeHint() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.debug_audio_cache = true;
    config.paired_device_ids.push_back("5A74");
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    coordinator.HandleGlobalHotkeyPressed();

    assert(ble_ptr->sent_remote_buttons.empty());
    assert(!ui.statuses.empty());
    assert(ui.statuses.back() == "Hotkey: VoiceStick not connected; press the main button to wake it");
    assert(!ui.notifications.empty());
    assert(ui.notifications.back().find("按主键唤醒") != std::string::npos);
}

void TestCoordinatorHotkeyWithConnectionSendsRemoteButton() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.paired_device_ids.push_back("5A74");
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    coordinator.HandleGlobalHotkeyPressed();

    assert(ble_ptr->sent_remote_buttons.size() == 1);
    assert(ble_ptr->sent_remote_buttons.back().action == RemoteButtonAction::kDown);
    assert(ble_ptr->sent_remote_buttons.back().button == "primary");
    assert(ble_ptr->sent_remote_buttons.back().device_id == std::optional<std::string>("5A74"));
}

void TestWechatStartFailureRollsBackDefaultCapture() {
    // B3：auto_switch 已把默认录音设备切到 CABLE 后 renderer.Start 失败必须回滚，
    // 否则默认麦长期停在 CABLE（静音）；且下次会话把 CABLE 当「原设备」存回（毒化）。
    // 注入缝：假 switcher（记录 SetDefaultCapture）+ 假 renderer（Start 恒 false）
    // + 独立 device_switch_state_path（不碰真实 %APPDATA%）。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();

    FakeDefaultAudioDeviceController* switcher = nullptr;
    auto switcher_factory = [&switcher]() -> std::unique_ptr<IDefaultAudioDeviceController> {
        auto p = std::make_unique<FakeDefaultAudioDeviceController>();
        p->default_capture = AudioDeviceInfo{L"real-mic-id", L"Real Microphone"};
        p->capture_devices = {AudioDeviceInfo{L"real-mic-id", L"Real Microphone"},
                              AudioDeviceInfo{L"cable-id", L"CABLE (VB-Audio)"}};
        switcher = p.get();
        return p;
    };
    FakeVirtualMicRenderer* renderer = nullptr;
    auto renderer_factory =
        [&renderer](const IVirtualMicRenderer::Options&) -> std::unique_ptr<IVirtualMicRenderer> {
        auto p = std::make_unique<FakeVirtualMicRenderer>(false);  // Start 恒失败
        renderer = p.get();
        return p;
    };
    auto hotkey_factory = [](const std::string&) -> std::unique_ptr<IWechatInputMethodHotkey> {
        return std::make_unique<FakeWechatInputMethodHotkey>();
    };

    AppConfig config = AppConfig::Defaults();
    config.default_output_profile.target = OutputTarget::kWechatInputMethod;
    config.wechat_input_method.auto_switch_default_recording_device = true;
    config.wechat_input_method.virtual_mic_capture_name = "CABLE";

    const auto state_path =
        std::filesystem::temp_directory_path() / "voicestick_b3_device_switch.json";
    std::error_code ec;
    std::filesystem::remove(state_path, ec);

    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input,
                                      /*asr_factory*/ {}, renderer_factory, hotkey_factory,
                                      switcher_factory, state_path);
    coordinator.Start();

    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 42));

    // 断言 1：确实先切到 CABLE、失败后又切回原设备（两次 SetDefaultCapture）。
    assert(switcher != nullptr);
    assert(switcher->set_calls.size() == 2);
    assert(switcher->set_calls[0].device_id == L"cable-id");
    assert(switcher->set_calls[1].device_id == L"real-mic-id");
    // 断言 2：回滚后默认录音设备回到真实麦克风（不是 CABLE）。
    assert(switcher->default_capture.has_value());
    assert(switcher->default_capture->id == L"real-mic-id");
    // 断言 3：落盘的设备切换状态已清（下次启动不会再把 CABLE 当「原设备」）。
    assert(!std::filesystem::exists(state_path, ec));
    std::filesystem::remove(state_path, ec);
}

void TestXiaomiAtvvCapsParsing() {
    // TX 命令构造。
    assert(XiaomiAtvvProtocol::GetCapsCommand() == (ByteVector{0x0A, 0x01, 0x00, 0x00, 0x03, 0x03}));
    assert(XiaomiAtvvProtocol::MicOpenAckCommand(false) == (ByteVector{0x0C, 0x00}));
    assert(XiaomiAtvvProtocol::MicOpenAckCommand(true) == (ByteVector{0x0C, 0x00, 0x02}));
    assert(XiaomiAtvvProtocol::MicCloseCommand(false, 0x07) == (ByteVector{0x0D, 0x07}));
    assert(XiaomiAtvvProtocol::MicCloseCommand(true, 0x07) == (ByteVector{0x0D}));

    // v1.0 标准布局：16kHz、interaction=3、协商帧长 120。
    auto caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78});
    assert(caps.has_value());
    assert(caps->IsV1OrLater());
    assert(caps->codecs == 0x02 && caps->Supports16kHz());
    assert(caps->interaction == 0x03);
    assert(caps->frame_bytes == 120);

    // 协商帧长 0 → 默认 120；自定义帧长 256；无帧长字段（len 5）→ 默认 120。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00});
    assert(caps && caps->frame_bytes == XiaomiAtvvProtocol::default_frame_bytes);
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03, 0x01, 0x00});
    assert(caps && caps->frame_bytes == 256);
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02, 0x03});
    assert(caps && caps->frame_bytes == 120);

    // 兼容分支：报 v1 但 codecs==0，旧版双字节 codec 布局（[4]=0x02 且 len≥9）；
    // 旧版布局未定义帧长字段，[5:7] 的垃圾值（0x0100=256）不得被采用，保持默认 120。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00});
    assert(caps && caps->codecs == 0x02 && caps->interaction == 0x03 && caps->Supports16kHz());
    assert(caps->frame_bytes == XiaomiAtvvProtocol::default_frame_bytes);

    // 旧版 v0 布局：len≥9，codecs=[4]，interaction=0，帧长默认。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00});
    assert(caps && !caps->IsV1OrLater());
    assert(caps->codecs == 0x02 && caps->interaction == 0x00);
    assert(caps->frame_bytes == XiaomiAtvvProtocol::default_frame_bytes);

    // 8kHz-only：解析成功但不支持 16kHz（由会话层判 Error）。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x01, 0x03, 0x00, 0x78});
    assert(caps && !caps->Supports16kHz());
    // v1 codecs==0 且不满足兼容分支（[4]&0x03==0）→ 无可用 codec。
    caps = XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x00, 0x00, 0x00, 0x78});
    assert(caps && caps->codecs == 0x00 && !caps->Supports16kHz());

    // 拒绝：空包/短包/错 opcode/v1 缺 interaction 字段/旧版短包。
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0C, 0x01, 0x00, 0x02, 0x03}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x01, 0x00, 0x02}).has_value());
    assert(!XiaomiAtvvProtocol::ParseCaps(ByteVector{0x0B, 0x00, 0x01, 0x00, 0x02}).has_value());
}


void TestImaAdpcmDecoderGolden() {
    ImaAdpcmDecoder decoder;
    // 标准向量：reset(0,0) 后 0x11 → [1,2]。
    decoder.Reset(0, 0);
    auto out = decoder.Decode(ByteVector{0x11});
    assert((out == std::vector<std::int16_t>{1, 2}));
    // 预计算 golden（独立 Python 实现对拍，高半字节优先）。
    decoder.Reset(0, 0);
    out = decoder.Decode(ByteVector{0x11, 0x22, 0x77, 0x88, 0xF0, 0x0F, 0x45, 0x54});
    assert((out == std::vector<std::int16_t>{1, 2, 5, 8, 19, 49, 45, 42,
                                             -10, -3, 3, -90, 30, 208, 468, 781}));
    // 状态连续推进：分段解码与一次性解码逐样本相等。
    decoder.Reset(0, 0);
    auto joined = decoder.Decode(ByteVector{0x11, 0x22, 0x77});
    auto rest = decoder.Decode(ByteVector{0x88, 0xF0, 0x0F, 0x45, 0x54});
    joined.insert(joined.end(), rest.begin(), rest.end());
    assert(joined == out);

    // 编码往返：正弦+斜波（480 样本，偶数）经测试编码器后与解码器逐样本对拍。
    std::vector<std::int16_t> pcm(480);
    for (int i = 0; i < 480; ++i) {
        pcm[i] = static_cast<std::int16_t>(std::sin(i * 0.05) * 12000.0 + i * 10);
    }
    const auto golden = ImaEncodeForTest(pcm);
    decoder.Reset(0, 0);
    out = decoder.Decode(golden.encoded);
    assert(out == golden.expected_decoded);

    // Reset 钳位边界。
    decoder.Reset(0, 200);
    assert(decoder.step_index() == 88);
    decoder.Reset(0, -5);
    assert(decoder.step_index() == 0);
    decoder.Reset(-100, 40);
    assert(decoder.predictor() == -100 && decoder.step_index() == 40);
    // 高 step 起步解码正常推进。
    out = decoder.Decode(ByteVector{0x11, 0x11});
    assert(out.size() == 4);
}

void TestFlashToolFlow() {
    const FlashTestPaths paths;

    // 成功：退出码 0 → Finished(success)。
    {
        FakeFlashRunner runner;
        std::vector<FlashEvent> events;
        FlashTool tool(MakeFlashOptions(paths), paths.python, &runner,
                       [&](const FlashEvent& e) { events.push_back(e); });
        assert(tool.Run());
        assert(runner.calls.size() == 1);
        const FlashEvent& last = events.back();
        assert(last.kind == FlashEvent::kFinished && last.success && !last.cancelled);
    }

    // 失败：退出码非 0 → Finished(failure) 且有错误事件。
    {
        FakeFlashRunner runner;
        runner.exit_codes = {1};
        std::vector<FlashEvent> events;
        FlashTool tool(MakeFlashOptions(paths), paths.python, &runner,
                       [&](const FlashEvent& e) { events.push_back(e); });
        assert(!tool.Run());
        const FlashEvent& last = events.back();
        assert(last.kind == FlashEvent::kFinished && !last.success && !last.cancelled);
        bool saw_error = false;
        for (const auto& e : events) if (e.kind == FlashEvent::kError) saw_error = true;
        assert(saw_error);
    }

    // EraseThenFull：先 erase_flash 后 write_flash 的顺序断言；erase 失败不进入写。
    {
        FakeFlashRunner runner;
        FlashOptions options = MakeFlashOptions(paths);
        options.mode = FlashMode::kEraseThenFull;
        FlashTool tool(options, paths.python, &runner,
                       [](const FlashEvent&) {});
        assert(tool.Run());
        assert(runner.calls.size() == 2);
        assert(ArgvContains(runner.calls[0], L"erase_flash"));
        assert(ArgvContains(runner.calls[1], L"write_flash"));
    }
    {
        FakeFlashRunner runner;
        runner.exit_codes = {1, 0};  // erase 失败
        FlashOptions options = MakeFlashOptions(paths);
        options.mode = FlashMode::kEraseThenFull;
        FlashTool tool(options, paths.python, &runner,
                       [](const FlashEvent&) {});
        assert(!tool.Run());
        assert(runner.calls.size() == 1);  // 不进入第二步
    }

    // 取消：runner 返回 kFlashCancelledExitCode → Finished(cancelled)。
    {
        FakeFlashRunner runner;
        runner.exit_codes = {kFlashCancelledExitCode};
        std::vector<FlashEvent> events;
        FlashTool tool(MakeFlashOptions(paths), paths.python, &runner,
                       [&](const FlashEvent& e) { events.push_back(e); });
        assert(!tool.Run());
        const FlashEvent& last = events.back();
        assert(last.kind == FlashEvent::kFinished && !last.success && last.cancelled);
    }

    // 校验失败：未选串口 → 不拉起子进程，直接 Finished(failure)。
    {
        FakeFlashRunner runner;
        FlashOptions options = MakeFlashOptions(paths);
        options.serial_port.clear();
        std::vector<FlashEvent> events;
        FlashTool tool(options, paths.python, &runner,
                       [&](const FlashEvent& e) { events.push_back(e); });
        assert(!tool.Run());
        assert(runner.calls.empty());
        const FlashEvent& last = events.back();
        assert(last.kind == FlashEvent::kFinished && !last.success);
    }

    // 校验失败：非 .bin 后缀。
    {
        FakeFlashRunner runner;
        FlashOptions options = MakeFlashOptions(paths);
        options.firmware_path = paths.python.wstring();  // .exe 后缀
        FlashTool tool(options, paths.python, &runner,
                       [](const FlashEvent&) {});
        assert(!tool.Run());
        assert(runner.calls.empty());
    }
}




void TestPowerLogMonitor() {
    // 1) 条目解析。
    const auto anchor_entry = BuildPowerLogEntry(1000, 0, 0xFF, kPowerLogFlagTimeAnchor, 1755800000u);
    PowerLogEntryData parsed{};
    assert(ParsePowerLogEntry(anchor_entry.data(), anchor_entry.size(), &parsed));
    assert(parsed.uptime_s == 1000);
    assert(parsed.is_time_anchor);
    assert(parsed.anchor_epoch == 1755800000u);

    // 2) 分片帧解析：ParseStateEvent 对 power_log 帧应返回 nullopt（无 event 字段）。
    std::vector<std::uint8_t> blob;
    const auto periodic1 = BuildPowerLogEntry(1060, 4100, 0, kPowerLogFlagPeriodic);
    blob.insert(blob.end(), periodic1.begin(), periodic1.end());
    const auto json = std::string("{\"power_log\":{\"seq\":0,\"offset\":16,\"total\":40,") +
                      "\"eof\":0,\"data\":\"" + Base64EncodeForTest(blob) + "\"}}";
    const auto frame = BuildStateJsonFrame(json);
    assert(!BleProtocol::ParseStateEvent(frame).has_value());
    const auto fragment = BleProtocol::ParsePowerLogFragment(frame);
    assert(fragment.has_value());
    assert(fragment->seq == 0);
    assert(fragment->offset == 16);
    assert(fragment->total == 40);
    assert(!fragment->eof);
    assert(fragment->data == blob);

    // eof 空数据分片（探测响应）。
    const auto eof_frame = BuildStateJsonFrame(
        "{\"power_log\":{\"seq\":0,\"offset\":1000000000,\"total\":40,\"eof\":1,\"data\":\"\"}}");
    const auto eof_fragment = BleProtocol::ParsePowerLogFragment(eof_frame);
    assert(eof_fragment.has_value());
    assert(eof_fragment->eof);
    assert(eof_fragment->total == 40);
    assert(eof_fragment->data.empty());

    // 3) 命令 payload。
    const auto dump_payload = BleProtocol::PowerLogDumpPayload(1000000000u, 160);
    const std::string dump_json(dump_payload.begin(), dump_payload.end());
    assert(dump_json.find("\"power_log\"") != std::string::npos);
    assert(dump_json.find("\"cmd\":\"dump\"") != std::string::npos);
    assert(dump_json.find("\"offset\":1000000000") != std::string::npos);
    assert(dump_json.find("\"max\":160") != std::string::npos);
    const auto anchor_payload = BleProtocol::PowerLogTimeAnchorPayload(1755800000u);
    const std::string anchor_json(anchor_payload.begin(), anchor_payload.end());
    assert(anchor_json.find("\"cmd\":\"time_anchor\"") != std::string::npos);
    assert(anchor_json.find("\"epoch\":1755800000") != std::string::npos);

    // 3b) 供电态（USB）自动关机：power_mgmt 事件解析 + 命令 payload。
    // ParseStateEvent 对 power_mgmt 帧返回 nullopt（由 ParsePowerMgmtEvent 消费）。
    const auto pm_on_frame = BuildStateJsonFrame(
        "{\"event\":\"power_mgmt\",\"usb_auto_off\":true}");
    assert(!BleProtocol::ParseStateEvent(pm_on_frame).has_value());
    const auto pm_on = BleProtocol::ParsePowerMgmtEvent(pm_on_frame);
    assert(pm_on.has_value() && *pm_on);
    const auto pm_off = BleProtocol::ParsePowerMgmtEvent(BuildStateJsonFrame(
        "{\"event\":\"power_mgmt\",\"usb_auto_off\":false}"));
    assert(pm_off.has_value() && !*pm_off);
    // 缺字段或非 power_mgmt 帧 → nullopt。
    assert(!BleProtocol::ParsePowerMgmtEvent(
        BuildStateJsonFrame("{\"event\":\"power_mgmt\"}")).has_value());
    assert(!BleProtocol::ParsePowerMgmtEvent(frame).has_value());  // power_log 分片帧
    // 命令 payload：set 带布尔 enabled，get 为查询命令。
    const auto set_on_payload = BleProtocol::UsbAutoOffPayload(true);
    const std::string set_on_json(set_on_payload.begin(), set_on_payload.end());
    assert(set_on_json.find("\"event\":\"usb_auto_off\"") != std::string::npos);
    assert(set_on_json.find("\"enabled\":true") != std::string::npos);
    const auto set_off_payload = BleProtocol::UsbAutoOffPayload(false);
    const std::string set_off_json(set_off_payload.begin(), set_off_payload.end());
    assert(set_off_json.find("\"enabled\":false") != std::string::npos);
    const auto get_payload = BleProtocol::UsbAutoOffGetPayload();
    const std::string get_json(get_payload.begin(), get_payload.end());
    assert(get_json.find("\"event\":\"usb_auto_off_get\"") != std::string::npos);

    // 4) 累积器：锚点 + 周期采样 + 非周期事件过滤 + epoch 对齐。
    PowerLogAccumulator accumulator;
    std::vector<std::uint8_t> blob1;
    blob1.insert(blob1.end(), anchor_entry.begin(), anchor_entry.end());
    // 非周期模式切换事件（无 PERIODIC 位）：不应产生采样。
    const auto mode_entry = BuildPowerLogEntry(1010, 0, 1, 0);
    blob1.insert(blob1.end(), mode_entry.begin(), mode_entry.end());
    // 两个周期采样（一个充电、一个放电）。
    const auto sample1 = BuildPowerLogEntry(1060, 4100, 0,
                                            kPowerLogFlagPeriodic | kPowerLogFlagUsbPowered |
                                                kPowerLogFlagCharging);
    blob1.insert(blob1.end(), sample1.begin(), sample1.end());
    const auto sample2 = BuildPowerLogEntry(1120, 4090, 0, kPowerLogFlagPeriodic);
    blob1.insert(blob1.end(), sample2.begin(), sample2.end());

    std::vector<PowerLogSample> new_samples;
    assert(accumulator.ConsumeIncrementalBlob(blob1.data(), blob1.size(), &new_samples));
    assert(new_samples.size() == 2);
    assert(accumulator.samples().size() == 2);
    // epoch 对齐：anchor(epoch=1755800000, uptime=1000) → uptime 1060 → +60s。
    assert(accumulator.samples()[0].epoch_s == 1755800060);
    assert(accumulator.samples()[1].epoch_s == 1755800120);
    assert(accumulator.samples()[0].vbat_mv == 4100);
    assert(accumulator.samples()[0].charging);
    assert(accumulator.samples()[0].usb_powered);
    assert(!accumulator.samples()[1].charging);
    assert(accumulator.last_uptime_s() == 1120);

    // 5) 增量第二段：仅新周期采样。
    std::vector<std::uint8_t> blob2;
    const auto sample3 = BuildPowerLogEntry(1180, 4080, 0, kPowerLogFlagPeriodic);
    blob2.insert(blob2.end(), sample3.begin(), sample3.end());
    new_samples.clear();
    assert(accumulator.ConsumeIncrementalBlob(blob2.data(), blob2.size(), &new_samples));
    assert(new_samples.size() == 1);
    assert(accumulator.samples().size() == 3);
    assert(accumulator.samples()[2].epoch_s == 1755800180);

    // 6) 重启检测：uptime 回退返回 false 且状态不变。
    std::vector<std::uint8_t> blob_restart;
    const auto stale = BuildPowerLogEntry(50, 4000, 0, kPowerLogFlagPeriodic);
    blob_restart.insert(blob_restart.end(), stale.begin(), stale.end());
    new_samples.clear();
    assert(!accumulator.ConsumeIncrementalBlob(blob_restart.data(), blob_restart.size(),
                                               &new_samples));
    assert(new_samples.empty());
    assert(accumulator.samples().size() == 3);

    // 7) 长度非 12 倍数视为流损坏。
    const std::vector<std::uint8_t> bad_blob(13, 0);
    assert(!accumulator.ConsumeIncrementalBlob(bad_blob.data(), bad_blob.size(), nullptr));

    // 8) CSV：表头 + 每采样一行 + 无效读数标记。
    const std::string csv = accumulator.FormatCsv();
    assert(csv.find("seq,timestamp_iso,epoch_s,uptime_s,vbat_mv,vbat_v") == 0);
    assert(csv.find("1755800060") != std::string::npos);
    assert(csv.find("4.100") != std::string::npos);
    assert(csv.find("S0_ACTIVE") != std::string::npos);

    // 9) 无锚点场景：epoch 未对齐（-1）但仍收集采样。
    PowerLogAccumulator bare;
    std::vector<std::uint8_t> blob_no_anchor;
    const auto orphan = BuildPowerLogEntry(60, 4050, 0, kPowerLogFlagPeriodic);
    blob_no_anchor.insert(blob_no_anchor.end(), orphan.begin(), orphan.end());
    assert(bare.ConsumeIncrementalBlob(blob_no_anchor.data(), blob_no_anchor.size(), nullptr));
    assert(bare.samples().size() == 1);
    assert(bare.samples()[0].epoch_s == -1);

    // 10) 锚点之后的更新锚点生效（取 uptime 最大者）。
    PowerLogAccumulator re_anchor;
    std::vector<std::uint8_t> blob_re_anchor;
    blob_re_anchor.insert(blob_re_anchor.end(), anchor_entry.begin(), anchor_entry.end());
    const auto anchor2 = BuildPowerLogEntry(2000, 0, 0xFF, kPowerLogFlagTimeAnchor, 1755810000u);
    blob_re_anchor.insert(blob_re_anchor.end(), anchor2.begin(), anchor2.end());
    const auto sample_after = BuildPowerLogEntry(2060, 4070, 0, kPowerLogFlagPeriodic);
    blob_re_anchor.insert(blob_re_anchor.end(), sample_after.begin(), sample_after.end());
    assert(re_anchor.ConsumeIncrementalBlob(blob_re_anchor.data(), blob_re_anchor.size(), nullptr));
    assert(re_anchor.samples().size() == 1);
    assert(re_anchor.samples()[0].epoch_s == 1755810060);

    printf("TestPowerLogMonitor passed\n");
}



void TestTextRefinerRules() {
    printf(">> TestTextRefinerRules\n"); fflush(stdout);
    struct Case { const char* in; const char* want; const char* note; };
    const Case cases[] = {
        // 句首语气词（嗯/呃 直接删；啊/哦/噢/哎/唉/诶 须后跟标点才删，防误伤实义开头）
        {"嗯，帮我把这个文件重命名一下。", "帮我把这个文件重命名一下。", "句首嗯+标点"},
        {"呃我们试试", "我们试试", "句首呃无标点"},
        {"啊，开会了。", "开会了。", "句首啊+标点"},
        {"哦，对了，会议改到下午三点了。", "对了，会议改到下午三点了。", "句首哦+标点"},
        {"哦对了开会", "哦对了开会", "哦后无标点不动（保守）"},
        {"嗯帮我打开", "帮我打开", "句首嗯无标点"},
        // CJK 叠字：连续同字 >=3 时，笑声字（哈/嘿/呵/嘻）保留 2 个，其余保留 1 个
        {"我我我想去吃火锅。", "我想去吃火锅。", "口吃叠字保留1"},
        {"哈哈哈", "哈哈", "3哈保留2"},
        {"哈哈哈哈", "哈哈", "4哈保留2"},
        {"哈哈", "哈哈", "2次不规整"},
        {"AAAA", "AAAA", "拉丁叠字不规整"},
        {"555", "555", "数字叠字不规整"},
        // 中文（CJK）字符之间的停顿空格清除；拉丁/数字周围空格保留
        {"搜一下 这个 项目", "搜一下这个项目", "CJK间空格清除"},
        {"搜一下 llama.cpp 这个", "搜一下 llama.cpp 这个", "拉丁周围空格保留"},
        // 重复标点规整（省略号 …… 为合法双码点，连续超 2 个收敛为 2 个）
        {"好。。", "好。", "重复句号"},
        {"真的？？？", "真的？", "重复问号"},
        {"嗯……我觉得还行", "我觉得还行", "省略号跟随句首嗯"},
        // 句首孤立标点清理
        {"，我今天想", "我今天想", "句首孤立标点"},
        // 边界
        {"", "", "空文本"},
        {"嗯。", "", "纯口水词句清空"},
        {"帮我在 GitHub 上搜一下 llama.cpp 这个项目。",
         "帮我在 GitHub 上搜一下 llama.cpp 这个项目。", "干净文本不动"},
    };
    for (const auto& c : cases) {
        const std::string got = RuleRefineText(c.in);
        if (got != c.want) {
            printf("   RuleRefineText 失败 [%s]\n     in  =%s\n     got =%s\n     want=%s\n",
                   c.note, c.in, got.c_str(), c.want);
            fflush(stdout);
            assert(false);
        }
    }
    printf("TestTextRefinerRules passed\n");
}

void TestRefineGuardSafety() {
    printf(">> TestRefineGuardSafety\n"); fflush(stdout);
    // 放行：纯口水词删减（m0/refine spike 1.7B 实际输出形态）
    assert(RefineResultSafe("嗯，帮我把这个文件重命名一下。",
                            "帮我把这个文件重命名一下。"));
    assert(RefineResultSafe("啊，那个，你等一下，我马上就来。",
                            "你等一下，我马上就来。"));
    assert(RefineResultSafe("呃 那个 这个项目 嗯 用的是 BLE 连接。",
                            "这个项目用的是 BLE 连接。"));
    assert(RefineResultSafe("I think um we should uh use the model.",
                            "I think we should use the model."));
    assert(RefineResultSafe("然后呢，我们接下来就是要做那个测试了。",
                            "我们接下来就是要做测试了。"));
    // 放行：叠字删减、标点/空白规整、ASCII 大小写纠正（sense voice -> SenseVoice）
    assert(RefineResultSafe("我我我想去吃火锅。", "我想去吃火锅。"));
    assert(RefineResultSafe("我们用的是3.5版本。", "我们用的是 3.5 版本。"));
    assert(RefineResultSafe("我们还是用那个 sense voice 吧。",
                            "我们还是用 SenseVoice 吧。"));
    assert(RefineResultSafe("嗯。", ""));
    // 拦截（spike 真实失误样本回归）：改写、删实词、删实义片段、换字、删专名
    assert(!RefineResultSafe("你吃饭了没有啊？", "你吃饭了吗。"));
    assert(!RefineResultSafe("好的好的，我知道了。", "好的"));
    assert(!RefineResultSafe("这个函数的名字叫 process_data，注意是下划线。",
                             "这个函数的名字叫 process_data。"));
    assert(!RefineResultSafe("就是，我想问一下就是，这个支持 Windows 吗？",
                             "就是，这个支持 Windows 吗？"));
    assert(!RefineResultSafe("帮我把这个文件重命名一下。",
                             "帮我把那个文件重命名一下。"));
    assert(!RefineResultSafe("帮我在 GitHub 上搜一下 llama.cpp 这个项目。",
                             "帮我在 GitHub 上搜一下这个项目。"));
    assert(!RefineResultSafe("嗯，帮我打开浏览器。", ""));
    // 热词守卫联动：原文已正确出现的热词被改丢（含大小写改坏）必须拦截
    assert(!RefineResultSafe("编辑 AGENTS.md 这个文件", "编辑这个文件", {"AGENTS.md"}));
    assert(!RefineResultSafe("编辑 AGENTS.md 这个文件", "编辑 agents.md 这个文件",
                             {"AGENTS.md"}));
    assert(RefineResultSafe("编辑 AGENTS.md 这个文件", "编辑 AGENTS.md 这个文件",
                            {"AGENTS.md"}));
    printf("TestRefineGuardSafety passed\n");
}

void TestLocalRefinementClientOrchestration() {
    printf(">> TestLocalRefinementClientOrchestration\n"); fflush(stdout);
    // 可编程假引擎：注入式驱动编排层（流式/失败/取消/守卫回退）
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;      // 生成的 assistant 文本
        bool fail = false;      // Chat 直接失败
        int chat_calls = 0;
        std::string last_user;
        std::string last_system;
        bool Chat(const std::string& system_prompt, const std::string& user_text,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            ++chat_calls;
            last_system = system_prompt;
            last_user = user_text;
            if (fail) return false;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };

    struct Out {
        bool ok = false;
        std::string text;
        std::vector<std::string> tokens;
        // 引擎观察值：on_complete 时 Chat 已返回，在工作线程内采集
        //（run 返回后 client 连带析构 engine，事后读裸指针是悬垂）。
        int chat_calls = -1;
        std::string last_user;
        std::string last_system;
    };
    // 运行一次精修并同步等待完成（client 栈上持有，析构 join 保证线程收尾）
    auto run = [](std::unique_ptr<FakeEngine> fake, const std::string& text,
                  std::shared_ptr<std::atomic_bool> cancel = nullptr) {
        std::promise<Out> pr;
        auto fut = pr.get_future();
        auto tokens = std::make_shared<std::vector<std::string>>();
        FakeEngine* observer = fake.get();
        LocalRefinementClient client(std::move(fake));
        client.Refine(
            text,
            [tokens](std::string t) { tokens->push_back(std::move(t)); },
            [&pr, tokens, observer](bool ok, std::string s) {
                Out out;
                out.ok = ok;
                out.text = std::move(s);
                out.tokens = *tokens;
                if (observer) {  // 场景 4 传空引擎：无观察值可采
                    out.chat_calls = observer->chat_calls;
                    out.last_user = observer->last_user;
                    out.last_system = observer->last_system;
                }
                pr.set_value(std::move(out));
            },
            std::move(cancel));
        return fut.get();
    };

    {   // 1) 正常精修：L1 规则先行（fake 收到的是规则级文本），守卫放行
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我把这个文件重命名一下。";
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。");
        assert(r.ok);
        assert(r.text == "帮我把这个文件重命名一下。");
        assert(r.chat_calls == 1);
        assert(r.last_user.find("输入：帮我把这个文件重命名一下") !=
               std::string::npos);
        assert(r.last_system.find("输入：") != std::string::npos);  // few-shot
        assert(!r.tokens.empty());
    }
    {   // 2) 模型改坏（加字）被守卫拦截：回退规则级结果
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我我在 GitHub 上搜一下。";
        auto r = run(std::move(fake), "帮我在 GitHub 上搜一下 llama.cpp 这个项目。");
        assert(r.ok);
        assert(r.text == "帮我在 GitHub 上搜一下 llama.cpp 这个项目。");
    }
    {   // 3) 引擎失败：回退规则级结果（含 L1 规整）
        auto fake = std::make_unique<FakeEngine>();
        fake->fail = true;
        auto r = run(std::move(fake), "嗯，帮我打开浏览器。");
        assert(r.ok);
        assert(r.text == "帮我打开浏览器。");
    }
    {   // 4) 空引擎：纯规则层降级
        auto r = run(nullptr, "嗯，好的。");
        assert(r.ok);
        assert(r.text == "好的。");
    }
    {   // 5) 模板残留剥离（输出：前缀）后守卫放行
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "输出：帮我把这个文件重命名一下。";
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。");
        assert(r.ok);
        assert(r.text == "帮我把这个文件重命名一下。");
    }
    {   // 6) 预置取消：不触发引擎，直接回退
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我把这个文件重命名一下。";
        auto cancel = std::make_shared<std::atomic_bool>(true);
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。", cancel);
        assert(!r.ok);
        assert(r.text == "帮我把这个文件重命名一下。");
        assert(r.chat_calls == 0);
    }
    {   // 7) 热词联动：LLM 结果丢热词回退规则级
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "编辑这个文件";   // 丢了 AGENTS.md
        // hotwords 用例：Refine 带 hotwords —— run 不支持，单独构造
        std::promise<std::pair<bool, std::string>> pr;
        auto fut = pr.get_future();
        LocalRefinementClient client(std::move(fake));
        client.Refine("编辑 AGENTS.md 这个文件",
                      [](std::string) {},
                      [&pr](bool ok, std::string s) {
                          pr.set_value({ok, std::move(s)});
                      },
                      nullptr, {"AGENTS.md"});
        auto [ok, text] = fut.get();
        assert(ok);
        assert(text == "编辑 AGENTS.md 这个文件");
    }
    // 8) StripReplyTemplate 纯函数
    assert(LocalRefinementClient::StripReplyTemplate(
               "输入：abc\n输出：\n帮我把文件重命名") == "帮我把文件重命名");
    assert(LocalRefinementClient::StripReplyTemplate(
               "<think>x</think>好的") == "好的");
    assert(LocalRefinementClient::StripReplyTemplate("  干净文本  ") == "干净文本");
    printf("TestLocalRefinementClientOrchestration passed\n");
}

// 自定义精修提示词（设置页可编辑的底层语义）：非空构造注入生效，空串/缺省
// 回退内置 few-shot 默认——空 = 默认，与云端 refine_prompt 语义一致。
void TestLocalRefinementCustomPrompt() {
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string last_system;
        bool Chat(const std::string& system_prompt, const std::string&,
                  const std::function<bool(const std::string&)>&,
                  std::string& completion) override {
            last_system = system_prompt;
            completion = "帮我把这个文件重命名一下。";
            return true;
        }
        bool IsReady() const override { return true; }
    };

    // 观察值在 on_complete 内采集（Chat 已返回、engine 仍存活）
    auto system_of = [](std::unique_ptr<FakeEngine> fake,
                        const std::string& prompt = {}) {
        std::promise<std::string> pr;
        auto fut = pr.get_future();
        FakeEngine* observer = fake.get();
        LocalRefinementClient client(std::move(fake), prompt);
        client.Refine("嗯，帮我把这个文件重命名一下。", nullptr,
                      [&pr, observer](bool, std::string) {
                          pr.set_value(observer->last_system);
                      });
        return fut.get();
    };

    const std::string custom = "自定义提示词：删掉所有口水词。\n输入：嗯 x\n输出：x";
    assert(system_of(std::make_unique<FakeEngine>(), custom) == custom);
    assert(system_of(std::make_unique<FakeEngine>()) ==
           LocalRefinementClient::BuildSystemPrompt());
    assert(system_of(std::make_unique<FakeEngine>(), "") ==
           LocalRefinementClient::BuildSystemPrompt());
    printf("TestLocalRefinementCustomPrompt passed\n");
}

// 诊断日志回调：归因精修结果来自哪一层（llm ok / guard blocked / llm fail /
// llm empty），协调器注入 LogCoordinatorLine 落 VoiceStickApp.log 供实测排查。
void TestLocalRefinementDiagnosticsLogs() {
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;
        bool fail = false;
        bool Chat(const std::string&, const std::string&,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            if (fail) return false;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    struct Out {
        std::string text;
        std::vector<std::string> logs;
    };
    // 构造注入 log 采集，跑一次精修同步取回（输入文本进 RunRefine 时已归一
    // 为规则级文本，日志 in= 即 L1 输出）。
    auto run = [](std::unique_ptr<FakeEngine> fake, const std::string& input) {
        std::promise<Out> pr;
        auto fut = pr.get_future();
        auto logs = std::make_shared<std::vector<std::string>>();
        LocalRefinementClient client(
            std::move(fake), {},
            [logs](std::string_view m) { logs->emplace_back(m); });
        client.Refine(input, nullptr,
                      [&pr, logs](bool, std::string s) {
                          Out out;
                          out.text = std::move(s);
                          out.logs = *logs;
                          pr.set_value(std::move(out));
                      });
        return fut.get();
    };
    auto has = [](const std::vector<std::string>& logs, const char* needle) {
        for (const auto& l : logs) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    {   // LLM 成功放行：in= + llm ok 两行
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我把这个文件重命名一下。";
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。");
        assert(r.text == "帮我把这个文件重命名一下。");
        assert(has(r.logs, "in='帮我把这个文件重命名一下。'"));
        assert(has(r.logs, "llm ok: '帮我把这个文件重命名一下。'"));
    }
    {   // 守卫拦截：guard blocked 行 + 回退规则级
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我我在 GitHub 上搜一下。";  // 加字必被守卫拦
        auto r = run(std::move(fake), "帮我在 GitHub 上搜一下 llama.cpp 这个项目。");
        assert(r.text == "帮我在 GitHub 上搜一下 llama.cpp 这个项目。");
        assert(has(r.logs, "guard blocked"));
    }
    {   // 引擎失败：llm fail 行 + 规则级兜底
        auto fake = std::make_unique<FakeEngine>();
        fake->fail = true;
        auto r = run(std::move(fake), "嗯，帮我打开浏览器。");
        assert(r.text == "帮我打开浏览器。");
        assert(has(r.logs, "llm fail -> rule"));
    }
    printf("TestLocalRefinementDiagnosticsLogs passed\n");
}

// 跨轮纠正指令管线（M2a）：context.turns 非空走 BuildCorrectionSystemPrompt +
// 「上文：/输入：/处理：」prompt + ApplyPinyinCorrections 受限执行；为空走
// 现行 few-shot 管线（回归保护）。场景锚定 M0 spike C 组案例。
void TestLocalRefinementCrossTurnOrchestration() {
    printf(">> TestLocalRefinementCrossTurnOrchestration\n"); fflush(stdout);
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;
        bool fail = false;
        int chat_calls = 0;
        std::string last_user;
        std::string last_system;
        bool Chat(const std::string& system_prompt, const std::string& user_text,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            ++chat_calls;
            last_system = system_prompt;
            last_user = user_text;
            if (fail) return false;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    struct Out {
        bool ok = false;
        std::string text;
        int chat_calls = -1;
        std::string last_user;
        std::string last_system;
    };
    using Ctx = LocalRefinementClient::RefineContext;
    auto run = [](std::unique_ptr<FakeEngine> fake, const std::string& text,
                  Ctx context, std::vector<std::string> hotwords = {}) {
        std::promise<Out> pr;
        auto fut = pr.get_future();
        FakeEngine* observer = fake.get();
        LocalRefinementClient client(std::move(fake));
        client.Refine(
            text, [](std::string) {},
            [&pr, observer](bool ok, std::string s) {
                Out out;
                out.ok = ok;
                out.text = std::move(s);
                if (observer) {
                    out.chat_calls = observer->chat_calls;
                    out.last_user = observer->last_user;
                    out.last_system = observer->last_system;
                }
                pr.set_value(std::move(out));
            },
            nullptr, std::move(hotwords), std::move(context));
        return fut.get();
    };

    {   // 1) 纠正指令执行（C01 真机案例）：prompt 形态 + 受限替换放行
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "鱼器渍→语气词";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "我们刚才测了语气词过滤。"});
        auto r = run(std::move(fake), "那些鱼器渍已经被过滤掉了。", std::move(ctx));
        assert(r.ok);
        assert(r.text == "那些语气词已经被过滤掉了。");
        assert(r.chat_calls == 1);
        // system 是纠正指令模式（区别于 few-shot 生成模式）
        assert(r.last_system.find("参考上文") != std::string::npos);
        assert(r.last_system.find("错词→纠正词") != std::string::npos);
        // user 形态：历史续写块（FakeEngine 默认实现拼「输入：…处理：…」，
        // instruction 空轮次归一化为「无」——与真引擎 KV 重放形态自洽）
        // + 当句输入 + 处理锚（spike build_prompt 同款）
        assert(r.last_user.find("输入：\n处理：无\n") != std::string::npos);
        assert(r.last_user.find("输入：那些鱼器渍已经被过滤掉了。\n处理：") !=
               std::string::npos);
        // user 以「处理：」结尾（生成锚，spike 同款）
        assert(r.last_user.size() >= 9 &&
               r.last_user.compare(r.last_user.size() - 9, 9, "处理：") == 0);
    }
    {   // 1b) V2 热词注入：cross 模式热词非空时 user 含「热词：」行（输入
        //     行后、处理锚前），系统提示词教学示例提及热词；空表无热词行。
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "我们测了过滤。"});
        auto r = run(std::move(fake), "这些水池还没删。",
                     std::move(ctx), {"口水词", "语气词"});
        assert(r.ok);
        assert(r.last_user.find("输入：这些水池还没删。\n热词：口水词，语气词\n处理：") !=
               std::string::npos);
        assert(r.last_system.find("热词") != std::string::npos);
        // 空热词表：不残留热词行
        auto fake2 = std::make_unique<FakeEngine>();
        fake2->reply = "无";
        Ctx ctx2;
        ctx2.cross_turn = true;
        ctx2.turns.push_back({"", "我们测了过滤。"});
        auto r2 = run(std::move(fake2), "这些水池还没删。", std::move(ctx2));
        assert(r2.ok);
        assert(r2.last_user.find("热词：") == std::string::npos);
    }
    {   // 2) 模型输出「无」：结果=规则级文本（无提升无伤害）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "帮我把垃圾倒一下。"});
        auto r = run(std::move(fake), "嗯，帮我把垃圾倒一下。", std::move(ctx));
        assert(r.ok);
        assert(r.text == "帮我把垃圾倒一下。");
    }
    {   // 3) 越界指令部分拒绝：合法删除执行，越界替换拒绝（C05 形态）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "嗯，\n那个→明天\n办→半";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "明天下午三点的会议记得提醒我。"});
        auto r = run(std::move(fake), "嗯，那个会议改成三点办了。", std::move(ctx));
        assert(r.ok);
        assert(r.text == "那个会议改成三点办了。");  // 「嗯，」删除生效
    }
    {   // 4) 纠正词不在上文（C04）：指令拒绝，文本回退（规则级）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "口头鱼→口头语";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "口水词和语气词都要删掉。"});
        auto r = run(std::move(fake), "口头鱼也算语气词吗？", std::move(ctx));
        assert(r.ok);
        assert(r.text == "口头鱼也算语气词吗？");
    }
    {   // 5) 引擎失败：跨轮模式同样回退规则级
        auto fake = std::make_unique<FakeEngine>();
        fake->fail = true;
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "上文。"});
        auto r = run(std::move(fake), "嗯，帮我打开浏览器。", std::move(ctx));
        assert(r.ok);
        assert(r.text == "帮我打开浏览器。");
    }
    {   // 6) 热词保护：指令删除热词回退规则级
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "超导";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "上文。"});
        auto r = run(std::move(fake), "超导材料不错。", std::move(ctx),
                     {"超导"});
        assert(r.ok);
        assert(r.text == "超导材料不错。");
    }
    {   // 7) context 为空：走现行 few-shot 管线（system 含生成式教学）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "帮我把这个文件重命名一下。";
        auto r = run(std::move(fake), "嗯，帮我把这个文件重命名一下。", Ctx{});
        assert(r.ok);
        assert(r.text == "帮我把这个文件重命名一下。");
        assert(r.last_system.find("参考上文") == std::string::npos);
        assert(r.last_user.find("输出：") != std::string::npos);
        assert(r.last_user.find("处理：") == std::string::npos);
    }
    {   // 8) 多轮上文：逐轮续写块 + 守卫域含全部轮 refined
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "语音设别→语音识别";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "这个语音识别项目叫 VoiceStick。"});
        ctx.turns.push_back({"", "语音识别的准确率还可以。"});
        auto r = run(std::move(fake), "这个语音设别模型是哪个？", std::move(ctx));
        assert(r.ok);
        assert(r.text == "这个语音识别模型是哪个？");
        assert(r.last_user.find("输入：\n处理：无\n输入：\n处理：无\n") !=
               std::string::npos);
    }
    {   // 9) 3 参完成回调：跨轮成功时第三参=当轮模型指令输出（协调器存
        //     RefineHistory.instruction 的数据源——KV 重放 assistant 侧需
        //     形态自洽）；非跨轮管线恒给空串
        struct Triple {
            bool ok = false;
            std::string text;
            std::string instruction;
        };
        auto run3 = [](std::unique_ptr<FakeEngine> fake, const std::string& text,
                       Ctx context) {
            std::promise<Triple> pr;
            auto fut = pr.get_future();
            LocalRefinementClient client(std::move(fake));
            client.Refine(
                text, [](std::string) {},
                [&pr](bool ok, std::string s, std::string instr) {
                    Triple t;
                    t.ok = ok;
                    t.text = std::move(s);
                    t.instruction = std::move(instr);
                    pr.set_value(std::move(t));
                },
                nullptr, {}, std::move(context));
            return fut.get();
        };
        {
            auto fake = std::make_unique<FakeEngine>();
            fake->reply = "鱼器渍→语气词\n";
            Ctx ctx;
            ctx.cross_turn = true;
            ctx.turns.push_back({"", "我们刚才测了语气词过滤。", "无"});
            const auto r = run3(std::move(fake), "那些鱼器渍已经被过滤掉了。",
                                std::move(ctx));
            assert(r.ok);
            assert(r.text == "那些语气词已经被过滤掉了。");
            assert(r.instruction == "鱼器渍→语气词");  // stripped（尾部空白已剥）
        }
        {
            auto fake = std::make_unique<FakeEngine>();
            fake->reply = "帮我把这个文件重命名一下。";
            const auto r = run3(std::move(fake),
                                "嗯，帮我把这个文件重命名一下。", Ctx{});
            assert(r.ok);
            assert(r.text == "帮我把这个文件重命名一下。");
            assert(r.instruction.empty());  // 非跨轮管线无指令语义
        }
    }
    {   // 10) 引擎历史 assistant 侧 = instruction（形态自洽重放，防模型
        //     漂移为文本输出——重放 refined 实测 3 轮起漂移，smoke 2026-09-10）：
        //     带 instruction 的历史轮拼出「处理：{instruction}」，绝不出现
        //     refined 文本（守卫域只在 client 内部使用）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"那些鱼器渍被过滤了。", "那些语气词被过滤了。",
                             "鱼器渍→语气词"});
        auto r = run(std::move(fake), "帮我把垃圾倒一下。", std::move(ctx));
        assert(r.ok);
        assert(r.last_user.find("输入：那些鱼器渍被过滤了。\n处理：鱼器渍→语气词\n") !=
               std::string::npos);
        assert(r.last_user.find("那些语气词被过滤了。") == std::string::npos);
    }
    {   // 11) 热词锚点域：上文无正确写法（连续误识别）但热词表有 →
        //     client 须把 hotwords 下传 ApplyPinyinCorrections 作守卫锚点，
        //     指令放行（S2，划词纠错建立的词入热词表后即可自愈）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "逾期次→语气词";
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "我们测了逾期次过滤。"});  // 上文同样误识别
        auto r = run(std::move(fake), "这些逾期次还没删干净。", std::move(ctx),
                     {"语气词"});
        assert(r.ok);
        assert(r.text == "这些语气词还没删干净。");
    }
    printf("TestLocalRefinementCrossTurnOrchestration passed\n");
}

// 跨轮管线诊断日志归因（协调器实测排查通道）：correct ok / correct none /
// correct partial（含拒绝明细）/ hotword blocked。
void TestLocalRefinementCrossTurnDiagnosticsLogs() {
    printf(">> TestLocalRefinementCrossTurnDiagnosticsLogs\n"); fflush(stdout);
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;
        bool Chat(const std::string&, const std::string&,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    using Ctx = LocalRefinementClient::RefineContext;
    auto run = [](const std::string& reply, const std::string& text, Ctx ctx,
                  std::vector<std::string> hotwords = {}) {
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = reply;
        auto logs = std::make_shared<std::vector<std::string>>();
        LocalRefinementClient client(
            std::move(fake), {},
            [logs](std::string_view line) { logs->emplace_back(line); });
        std::promise<std::string> pr;
        auto fut = pr.get_future();
        client.Refine(text, [](std::string) {},
                      [&pr](bool, std::string s) { pr.set_value(std::move(s)); },
                      nullptr, std::move(hotwords), std::move(ctx));
        fut.get();
        return std::move(*logs);
    };
    auto has = [](const std::vector<std::string>& logs, const std::string& needle) {
        for (const auto& l : logs)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    };
    printf("   logs scenario 1\n"); fflush(stdout);
    {   // 纠正成功归因
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "我们刚才测了语气词过滤。"});
        const auto logs = run("鱼器渍→语气词", "那些鱼器渍已经被过滤掉了。",
                              std::move(ctx));
        assert(has(logs, "in='那些鱼器渍已经被过滤掉了。'"));
        assert(has(logs, "correct ok: '那些语气词已经被过滤掉了。'"));
    }
    printf("   logs scenario 2\n"); fflush(stdout);
    {   // 无指令归因
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "帮我把垃圾倒一下。"});
        const auto logs = run("无", "帮我把垃圾倒一下。", std::move(ctx));
        assert(has(logs, "correct none"));
    }
    printf("   logs scenario 3\n"); fflush(stdout);
    {   // 部分拒绝归因（含拒绝指令明细）
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "明天下午三点的会议记得提醒我。"});
        const auto logs = run("嗯，\n那个→明天\n办→半",
                              "嗯，那个会议改成三点办了。", std::move(ctx));
        assert(has(logs, "correct partial"));
        assert(has(logs, "那个→明天"));
        assert(has(logs, "办→半"));
    }
    printf("   logs scenario 4\n"); fflush(stdout);
    {   // 热词拦截归因
        Ctx ctx;
        ctx.cross_turn = true;
        ctx.turns.push_back({"", "上文。"});
        const auto logs = run("超导", "超导材料不错。", std::move(ctx), {"超导"});
        assert(has(logs, "hotword blocked"));
    }
    printf("   logs scenario 5\n"); fflush(stdout);
    {   // 引擎失败归因（跨轮同现行）
        struct FailEngine : LocalLlmEngine {
            bool Chat(const std::string&, const std::string&,
                      const std::function<bool(const std::string&)>&,
                      std::string&) override { return false; }
            bool IsReady() const override { return true; }
        };
        auto logs = std::make_shared<std::vector<std::string>>();
        LocalRefinementClient client(
            std::make_unique<FailEngine>(), {},
            [logs](std::string_view line) { logs->emplace_back(line); });
        std::promise<std::string> pr;
        auto fut = pr.get_future();
        Ctx ctx_fail;
        ctx_fail.cross_turn = true;
        ctx_fail.turns.push_back({"", "上文。"});
        client.Refine("嗯，帮我打开浏览器。", [](std::string) {},
                      [&pr](bool, std::string s) { pr.set_value(std::move(s)); },
                      nullptr, {}, std::move(ctx_fail));
        printf("   scenario 5: refine dispatched\n"); fflush(stdout);
        const std::string got5 = fut.get();
        printf("   scenario 5: got='%s'\n", got5.c_str()); fflush(stdout);
        assert(got5 == "帮我打开浏览器。");
        assert(has(*logs, "llm fail -> rule"));
        printf("   scenario 5: assertions done\n"); fflush(stdout);
    }
    printf("TestLocalRefinementCrossTurnDiagnosticsLogs passed\n");
}


// ---- 跨轮纠错 M1：拼音守卫 + 历史缓冲（Doc/Plan/local-asr-accuracy-and-cross-turn-refinement.md §3.5.1/2）----

// 判定矩阵锚定 M0 spike 对拍结果（m0/refine/run_cross_turn_spike.py same_or_near）：
// 韵母集合有交集即同音（声母不参与——渍zì/词cí、马mǎ/打dǎ 须放行）；
// 韵母命中模糊对（e/i 卷舌弱化）且声母交集非空 → 近音。
void TestPinyinSameOrNear() {
    printf(">> TestPinyinSameOrNear\n"); fflush(stdout);
    struct Case { std::uint32_t a, b; bool want; const char* note; };
    const Case cases[] = {
        {U'鱼', U'语', true,  "鱼/语 韵母v交集（真机案例）"},
        {U'器', U'气', true,  "器/气 同音qi"},
        {U'渍', U'词', true,  "渍/词 声母z/c不同但韵母i交集（M0 GO口径）"},
        {U'马', U'打', true,  "马/打 韵母a交集（宽松口径锚定）"},
        {U'设', U'识', true,  "设/识 e/i卷舌弱化+sh声母同"},
        {U'半', U'办', true,  "半/办 同音ban"},
        {U'女', U'旅', true,  "女/旅 韵母v交集"},
        {U'鱼', U'鱼', true,  "同字"},
        {U'A',  U'a',  true,  "ASCII忽略大小写"},
        {U'5',  U'5',  true,  "非汉字同字符"},
        {U'那', U'明', false, "那na/明ming 韵母无交集"},
        {U'塘', U'气', false, "塘tang/气qi 无交集"},
        {U'a',  U'鱼', false, "非汉字vs汉字"},
        {U'鱼', U'x',  false, "汉字vs非汉字"},
        {U'鱼', U'b',  false, "汉字vs字母不等"},
        {0x20000, 0x20001, false, "表外扩展B区字查不到按不同音"},
    };
    for (const auto& c : cases) {
        const bool got = PinyinSameOrNear(c.a, c.b);
        if (got != c.want) {
            printf("   PinyinSameOrNear 失败 [%s] U+%04X/U+%04X got=%d want=%d\n",
                   c.note, c.a, c.b, got, c.want);
            fflush(stdout);
            assert(false);
        }
    }
    printf("TestPinyinSameOrNear passed\n");
}

// golden = M0 spike C 组全 8 案例（report_cross_turn_4b.md）：
// 放行 4（C01/C02/C03/C08）、守卫拒绝回退/部分执行 3（C04/C05/C06）、直通 1（C07）。
void TestApplyPinyinCorrections() {
    printf(">> TestApplyPinyinCorrections\n"); fflush(stdout);
    {   // C01 真机案例：替换放行
        const auto r = ApplyPinyinCorrections(
            "那些鱼器渍已经被过滤掉了。", "鱼器渍→语气词",
            "我们刚才测了语气词过滤。");
        assert(r.text == "那些语气词已经被过滤掉了。");
        assert(r.rejected.empty());
    }
    {   // C02：替换放行
        const auto r = ApplyPinyinCorrections(
            "这个蓝崖遥控器的按键手感不错。", "蓝崖→蓝牙",
            "帮我用蓝牙遥控器测试一下。");
        assert(r.text == "这个蓝牙遥控器的按键手感不错。");
        assert(r.rejected.empty());
    }
    {   // C03：人名替换放行
        const auto r = ApplyPinyinCorrections(
            "章维说他会晚点到。", "章维→张伟",
            "张伟下午的会议来不了。");
        assert(r.text == "张伟说他会晚点到。");
        assert(r.rejected.empty());
    }
    {   // C08：e/i 卷舌弱化纠正放行
        const auto r = ApplyPinyinCorrections(
            "这个语音设别模型是哪个？", "语音设别→语音识别",
            "这个语音识别项目叫 VoiceStick。");
        assert(r.text == "这个语音识别模型是哪个？");
        assert(r.rejected.empty());
    }
    {   // C04：纠正词未在上文出现过 → 拒绝、原文直通
        const auto r = ApplyPinyinCorrections(
            "口头鱼也算语气词吗？", "口头鱼→口头语",
            "口水词和语气词都要删掉。");
        assert(r.text == "口头鱼也算语气词吗？");
        assert(r.rejected.size() == 1 && r.rejected[0] == "口头鱼→口头语");
    }
    {   // C05：混合指令——合法删除执行，两条越界替换拒绝（那/明韵母无交集、半不在上文）
        const auto r = ApplyPinyinCorrections(
            "嗯，那个会议改成三点办了。", "嗯，\n那个→明天\n办→半",
            "明天下午三点的会议记得提醒我。");
        assert(r.text == "那个会议改成三点办了。");  // 「嗯，」删除生效
        assert(r.rejected.size() == 2);
        assert(r.rejected[0] == "那个→明天");
        assert(r.rejected[1] == "办→半");
    }
    {   // C06：长度不等（2≠3）→ 拒绝
        const auto r = ApplyPinyinCorrections(
            "我在鱼塘里养了很多鱼。", "鱼塘→语气词",
            "那些语气词都被过滤掉了。");
        assert(r.text == "我在鱼塘里养了很多鱼。");
        assert(r.rejected.size() == 1);
    }
    {   // C07：无指令直通
        const auto r = ApplyPinyinCorrections("帮我把垃圾倒一下。", "无", "上文无关。");
        assert(r.text == "帮我把垃圾倒一下。");
        assert(r.rejected.empty());
    }
    {   // 边界：src 不在原文 → 拒绝
        const auto r = ApplyPinyinCorrections("今天天气不错。", "天气→气候", "气候很好。");
        assert(r.text == "今天天气不错。");
        assert(r.rejected.size() == 1);
    }
    {   // 边界：删除行含字母数字 → 拒绝（防误删内容词）
        const auto r = ApplyPinyinCorrections("嗯，打开 debug 开关。", "嗯，\ndebug",
                                              "无关。");
        assert(r.text == "打开 debug 开关。");
        assert(r.rejected.size() == 1 && r.rejected[0] == "debug");
    }
    {   // 边界：替换字对拼音不符（那/明）→ 拒绝
        const auto r = ApplyPinyinCorrections("我们那个走。", "那个→明个", "明个再说。");
        assert(r.text == "我们那个走。");
        assert(r.rejected.size() == 1);
    }
    {   // 边界：删除幅度 >60% 整体回退（total-ratio）
        const auto r = ApplyPinyinCorrections("嗯，那个，啊，就这样吧。",
                                              "嗯，\n那个，\n啊，\n就这样吧。", "无关。");
        assert(r.text == "嗯，那个，啊，就这样吧。");  // 回退原文
        bool has_ratio = false;
        for (const auto& x : r.rejected) has_ratio |= (x == "total-ratio");
        assert(has_ratio);
    }
    {   // 边界：空指令直通
        const auto r = ApplyPinyinCorrections("原文。", "", "无关。");
        assert(r.text == "原文。" && r.rejected.empty());
    }
    {   // S2 热词锚点域（划词纠错配套，Doc/Plan/selection-hotword-correction-and-asr-hotword-spike.md）：
        //     热词表整词命中可替代「上文出现过」作锚点——ASR 连续误识别
        //     「语气词」时上文永远无正确写法，用户划词确认的正确词入热词表
        //     后即建立锚点，所有近音变体由逐字近音校验自动容忍。
        const auto r = ApplyPinyinCorrections(
            "我们测试了逾期次过滤。", "逾期次→语气词", "",
            {"语气词"});
        assert(r.text == "我们测试了语气词过滤。");
        assert(r.rejected.empty());
    }
    {   // 近音校验不因热词锚点放宽：dst 与 src 非近音 → 仍拒
        const auto r = ApplyPinyinCorrections(
            "我们测试了逾期次过滤。", "逾期次→蓝牙", "",
            {"语气词"});
        assert(r.text == "我们测试了逾期次过滤。");
        assert(r.rejected.size() == 1);
    }
    {   // 热词整词匹配：dst 仅为热词的子串不算锚点（防意外放行窗口）
        const auto r = ApplyPinyinCorrections(
            "我们测试了逾期次过滤。", "逾期次→语气词", "",
            {"语气词表"});
        assert(r.text == "我们测试了逾期次过滤。");
        assert(r.rejected.size() == 1);
    }
    {   // 回归：无热词无上文仍拒（原锚点语义不变）
        const auto r = ApplyPinyinCorrections(
            "我们测试了逾期次过滤。", "逾期次→语气词", "");
        assert(r.text == "我们测试了逾期次过滤。");
        assert(r.rejected.size() == 1);
    }
    {   // V1 触类旁通：dst 比 src 恰好多 1 字且近音子序列对齐 → 放行。
        //     场景：热词「口水词」的少字变体「口水」（ASR 漏识别尾字）。
        const auto r = ApplyPinyinCorrections(
            "这些口水还没删干净。", "口水→口水词", "", {"口水词"});
        assert(r.text == "这些口水词还没删干净。");
        assert(r.rejected.empty());
    }
    {   // V1 变体含近音错字：水池→口水词（删「口」后「水词」vs「水池」，
        //     水=水、池 chí/词 cí 韵母交集同音——与 C01 渍/词同口径）
        const auto r = ApplyPinyinCorrections(
            "这些水池还没删干净。", "水池→口水词", "", {"口水词"});
        assert(r.text == "这些口水词还没删干净。");
        assert(r.rejected.empty());
    }
    {   // V1 +1 字对齐仍受锚点域约束：dst「水池子」可对齐但不在热词/上文 → 拒
        const auto r = ApplyPinyinCorrections(
            "这些水池还没删干净。", "水池→水池子", "", {"口水词"});
        assert(r.text == "这些水池还没删干净。");
        assert(r.rejected.size() == 1);
    }
    {   // V1 语义反转防护：不→很好（差 1 字但无任何近音对齐路径）→ 拒
        const auto r = ApplyPinyinCorrections(
            "这样不行的。", "不→很好", "", {"很好"});
        assert(r.text == "这样不行的。");
        assert(r.rejected.size() == 1);
    }
    {   // V1 长度差 2 仍拒：水→口水词
        const auto r = ApplyPinyinCorrections(
            "这些水还没删干净。", "水→口水词", "", {"口水词"});
        assert(r.text == "这些水还没删干净。");
        assert(r.rejected.size() == 1);
    }
    {   // V1 只放宽「错词漏字」方向：dst 更短（口水词→口水）→ 仍拒
        const auto r = ApplyPinyinCorrections(
            "这些口水词还没删干净。", "口水词→口水", "", {"口水"});
        assert(r.text == "这些口水词还没删干净。");
        assert(r.rejected.size() == 1);
    }
    {   // V4 热词锚定放宽：等长 dst 命中热词时不再要求逐字全近音，至少一个
        //     位置近音/同字即放行。真机案例「电楼板→洞洞板」（电 diǎn/洞 dòng
        //     韵母无交集），靠尾字「板」同字锚定（2026-09-10 用户实测被误杀）。
        const auto r = ApplyPinyinCorrections(
            "买电楼板了吗？", "电楼板→洞洞板", "", {"洞洞板"});
        assert(r.text == "买洞洞板了吗？");
        assert(r.rejected.empty());
    }
    {   // V4 防幻觉闸：dst 命中热词但与 src 无任何位置近音（今天→洞洞板）→ 拒
        const auto r = ApplyPinyinCorrections(
            "今天天气不错。", "今天→洞洞板", "", {"洞洞板"});
        assert(r.text == "今天天气不错。");
        assert(r.rejected.size() == 1);
    }
    {   // V4 长度闸不变：热词路径仍拒长度差 2（水→口水词，防 find 错位拼接）
        const auto r = ApplyPinyinCorrections(
            "我在喝水。", "水→口水词", "", {"口水词"});
        assert(r.text == "我在喝水。");
        assert(r.rejected.size() == 1);
    }
    {   // V4 上文锚定路径不放宽：dst 在上文但非热词、无近音 → 仍拒
        //     （放宽只给用户显式确认的热词，上文出现≠真值）
        const auto r = ApplyPinyinCorrections(
            "电楼板真不错。", "电楼板→洞楼板", "我说的是洞楼板", {});
        assert(r.text == "电楼板真不错。");
        assert(r.rejected.size() == 1);
    }
    {   // V4 畸形混行容错（4B 真机/冒烟实测输出形态）：同一批指令中
        //     「长句前缀→洞洞板」被长度闸拒、「动作板→洞洞板」正确放行，
        //     逐行独立裁决互不影响。
        const auto r = ApplyPinyinCorrections(
            "那我要到宜家里面去买一些动作板来看看这个效果怎么样。",
            "那我要到宜家里面去买一些动作板→洞洞板\n动作板→洞洞板",
            "", {"洞洞板"});
        assert(r.text ==
               "那我要到宜家里面去买一些洞洞板来看看这个效果怎么样。");
        assert(r.rejected.size() == 1);
    }
    printf("TestApplyPinyinCorrections passed\n");
}

// 划词纠错候选链路纯逻辑（S1，Doc/Plan/selection-hotword-correction-and-asr-hotword-spike.md §1.2）：
// LLM 输出解析 → 近音过滤 → 交给对话框展示。
void TestSelectionCorrection() {
    printf(">> TestSelectionCorrection\n"); fflush(stdout);
    {   // SameOrNearText：等长逐字近音（守卫基线口径）
        assert(SameOrNearText("逾期次", "语气词"));
        assert(SameOrNearText("鱼旗子", "逾期次"));
        assert(!SameOrNearText("逾期次", "蓝牙"));      // 码点不等长
        assert(!SameOrNearText("逾期次", "蓝牙耳机"));  // 等长但非近音
        assert(SameOrNearText("", ""));
    }
    {   // NearVariantText（V1 触类旁通口径）：等长近音 ∨ ±1 字近音子序列
        //     对齐（热词少字变体）；差 1 字但无近音对齐路径、差 2 字均拒
        assert(NearVariantText("逾期次", "语气词"));
        assert(NearVariantText("口水", "口水词"));    // 漏尾字变体
        assert(NearVariantText("水池", "口水词"));    // 漏首字+近音错字变体
        assert(!NearVariantText("逾期次", "蓝牙"));   // 差 1 但对不上
        assert(!NearVariantText("水", "口水词"));     // 差 2
        assert(NearVariantText("", ""));
    }
    {   // ParseCandidateLines：剥序号（1. / 1、/ -）、跳空行、跳「无」类
        //     直答、跳含标点行（候选是词不该有标点）、保序
        const auto lines = ParseCandidateLines(
            "1. 语气词\n"
            "2、鱼旗子\n"
            "\n"
            "无\n"
            "这行有逗号，跳过\n"
            "- 预期刺\n"
            "none\n"
            "3. 语气词\n");
        assert(lines.size() == 4);  // 保序不去重（去重在 FilterCandidates）
        assert(lines[0] == "语气词");
        assert(lines[1] == "鱼旗子");
        assert(lines[2] == "预期刺");
        assert(lines[3] == "语气词");
    }
    {   // FilterCandidates：近音过、非近音拒、去重、去与错词相同项；
        //     V1 起容忍 ±1 字近音子序列（「语气词语」是「逾期次」的
        //     +1 字变体，保留展示，用户点选是最终裁决）
        const auto r = FilterCandidates(
            "逾期次", {"语气词", "鱼旗子", "蓝牙", "语气词", "逾期次", "语气词语"}, {});
        assert(r.size() == 3);
        assert(r[0] == "语气词");
        assert(r[1] == "鱼旗子");
        assert(r[2] == "语气词语");
    }
    {   // FilterCandidates（V1 场景）：划「水池」时候选「口水词」保留
        const auto r = FilterCandidates("水池", {"口水词", "水库", "口水词"}, {});
        assert(r.size() == 1);
        assert(r[0] == "口水词");
    }
    {   // FilterCandidates（V4 热词同口径）：候选命中热词免逐字全近音，
        //     与错词至少一位近音/同字即保留（电楼板→洞洞板 靠尾字「板」）
        const auto r = FilterCandidates("电楼板", {"洞洞板", "电木板"}, {"洞洞板"});
        assert(r.size() == 1);
        assert(r[0] == "洞洞板");
    }
    {   // FilterCandidates（V4）：无热词时同候选仍按近音拒（放宽仅限热词）
        const auto r = FilterCandidates("电楼板", {"洞洞板"}, {});
        assert(r.empty());
    }
    {   // BuildCorrectionCandidatesPrompt：含错词与上下文；空上下文不空行残留；
        //     V3 热词注入——非空加「热词：」行引导从热词出候选，空表不残留
        const auto prompt = BuildCorrectionCandidatesPrompt("逾期次", "我们测了逾期次过滤");
        assert(prompt.find("逾期次") != std::string::npos);
        assert(prompt.find("我们测了逾期次过滤") != std::string::npos);
        const auto bare = BuildCorrectionCandidatesPrompt("逾期次", "");
        assert(bare.find("逾期次") != std::string::npos);
        assert(bare.find("上下文") == std::string::npos);
        assert(bare.find("热词：") == std::string::npos);  // 无热词行（约束句仍提热词）
        const auto with_hotwords =
            BuildCorrectionCandidatesPrompt("水池", "", {"口水词", "语气词"});
        assert(with_hotwords.find("热词：口水词，语气词") != std::string::npos);
    }
    printf("TestSelectionCorrection passed\n");
}

// client 候选生成编排（S1）：引擎输出 → 解析+近音过滤回调；失败给 (false,{})。
// 引擎调用经 engine 互斥与精修串行（同一 llama.cpp 实例非线程安全）。
void TestLocalRefinementGenerateCandidates() {
    printf(">> TestLocalRefinementGenerateCandidates\n"); fflush(stdout);
    class FakeEngine : public LocalLlmEngine {
    public:
        std::string reply;
        bool fail = false;
        std::string last_user;
        std::string last_system;
        bool Chat(const std::string& system_prompt, const std::string& user_text,
                  const std::function<bool(const std::string&)>& on_token,
                  std::string& completion) override {
            last_system = system_prompt;
            last_user = user_text;
            if (fail) return false;
            if (on_token && !on_token(reply)) return false;
            completion = reply;
            return true;
        }
        bool IsReady() const override { return true; }
    };
    struct Out {
        bool ok = false;
        std::vector<std::string> candidates;
        std::string last_user;
    };
    auto run = [](std::unique_ptr<FakeEngine> fake, const std::string& wrong,
                  const std::string& context,
                  std::vector<std::string> hotwords) {
        std::promise<Out> pr;
        auto fut = pr.get_future();
        FakeEngine* observer = fake.get();
        LocalRefinementClient client(std::move(fake));
        client.GenerateCandidates(
            wrong, context, std::move(hotwords),
            [&pr, observer](bool ok, std::vector<std::string> candidates) {
                Out out;
                out.ok = ok;
                out.candidates = std::move(candidates);
                if (observer) out.last_user = observer->last_user;
                pr.set_value(std::move(out));
            });
        return fut.get();
    };
    {   // 1) 多行候选：序号剥除 + 近音过滤 + 去重去原词
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "1. 语气词\n2、鱼旗子\n3. 蓝牙\n语气词";
        const auto r = run(std::move(fake), "逾期次", "我们测了逾期次过滤", {});
        assert(r.ok);
        assert(r.candidates.size() == 2);
        assert(r.candidates[0] == "语气词");
        assert(r.candidates[1] == "鱼旗子");
    }
    {   // 2) 模型直答「无」→ (true, {})
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        const auto r = run(std::move(fake), "逾期次", "", {});
        assert(r.ok);
        assert(r.candidates.empty());
    }
    {   // 3) 引擎失败 → (false, {})
        auto fake = std::make_unique<FakeEngine>();
        fake->fail = true;
        const auto r = run(std::move(fake), "逾期次", "", {});
        assert(!r.ok);
        assert(r.candidates.empty());
    }
    {   // 4) user 形态 = BuildCorrectionCandidatesPrompt(wrong, context)，
        //     system = 候选生成专用（区别于精修 few-shot/纠正指令两种）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "无";
        FakeEngine* observer = fake.get();
        std::promise<void> done;
        auto fut = done.get_future();
        LocalRefinementClient client(std::move(fake));
        client.GenerateCandidates(
            "逾期次", "我们测了逾期次过滤", {},
            [&done](bool, std::vector<std::string>) { done.set_value(); });
        fut.get();
        assert(observer->last_user ==
               BuildCorrectionCandidatesPrompt("逾期次", "我们测了逾期次过滤"));
        assert(observer->last_system ==
               BuildCandidatesSystemPrompt());
    }
    {   // 5) V3 热词注入：prompt 含「热词：」行；+1 字候选（口水词）经
        //     NearVariantText 过滤保留（划词「水池」触类旁通到热词）
        auto fake = std::make_unique<FakeEngine>();
        fake->reply = "1. 口水词";
        const auto r = run(std::move(fake), "水池", "", {"口水词"});
        assert(r.ok);
        assert(r.last_user.find("热词：口水词") != std::string::npos);
        assert(r.candidates.size() == 1);
        assert(r.candidates[0] == "口水词");
    }
    printf("TestLocalRefinementGenerateCandidates passed\n");
}

// 历史缓冲：滑窗 5 轮、2 分钟 TTL 惰性过期、ContextText 拼接、Clear。
void TestRefineHistory() {
    printf(">> TestRefineHistory\n"); fflush(stdout);
    std::int64_t fake_now = 1'000;
    auto now = [&fake_now] { return fake_now; };
    RefineHistory h(/*max_turns=*/5, /*ttl_ms=*/120'000, now);
    {   // 滑窗：7 轮只留最近 5 轮
        for (int i = 1; i <= 7; ++i) {
            h.Add("raw" + std::to_string(i), "refined" + std::to_string(i));
        }
        const auto turns = h.Turns();
        assert(turns.size() == 5);
        assert(turns.front().raw_asr == "raw3");
        assert(turns.back().refined == "refined7");
        assert(h.ContextText() == "refined3。refined4。refined5。refined6。refined7");
    }
    {   // TTL 内不过期
        fake_now += 119'999;
        assert(h.Turns().size() == 5);
    }
    {   // 超时整体过期
        fake_now += 2;
        assert(h.Turns().empty());
        assert(h.ContextText().empty());
    }
    {   // 过期后重新累积，从新轮起算
        fake_now += 1'000;
        h.Add("raw_new", "refined_new");
        const auto turns = h.Turns();
        assert(turns.size() == 1 && turns[0].raw_asr == "raw_new");
        assert(h.ContextText() == "refined_new");
    }
    {   // Clear 立即清空
        h.Clear();
        assert(h.Turns().empty() && h.ContextText().empty());
    }
    {   // 默认时钟构造冒烟（真实 steady_clock，不会立即过期）
        RefineHistory real;
        real.Add("原文", "精修");
        assert(real.Turns().size() == 1);
        assert(real.ContextText() == "精修");
    }
    {   // 三参 Add：instruction 透传读回（KV 续写重放 assistant 侧数据源）；
        // 二参 Add 兼容旧调用（instruction 默认空 = 无指令轮次）
        RefineHistory h2(5, 120'000, now);
        h2.Add("raw1", "refined1", "鱼器渍→语气词");
        h2.Add("raw2", "refined2");
        const auto turns = h2.Turns();
        assert(turns.size() == 2);
        assert(turns[0].instruction == "鱼器渍→语气词");
        assert(turns[1].instruction.empty());
        // ContextText 域不含 instruction（守卫查找域只认 refined）
        assert(h2.ContextText() == "refined1。refined2");
    }
    printf("TestRefineHistory passed\n");
}


// 真模型 smoke：LlamaCppEngine 加载真实 Qwen3-1.7B GGUF 并连发两句（第二句
// 验证 KV 前缀复用延迟收敛）。模型解析与生产同口径（ResolveLocalRefineModelPath，
// env VOICESTICK_REFINE_MODEL 注入）；不在位时 SKIP——不 mock 真实链路。
// 仅 Release（NDEBUG）跑：Debug 无优化下 GGML 推理慢 20~40 倍（实测 prefix
// prefill 700ms/token vs Release ~3ms/token），单句 4 分钟起，全量回归不可接受。
void TestLlamaCppEngineRealModelSmoke() {
    printf(">> TestLlamaCppEngineRealModelSmoke\n"); fflush(stdout);
#ifdef VOICESTICK_LOCAL_REFINE_ENABLED
#ifndef NDEBUG
    printf("TestLlamaCppEngineRealModelSmoke SKIP（Debug 构建推理慢 20~40 倍，"
           "Release 专用）\n");
#else
    const std::string models_dir = ResolveLocalMicModelsDir("", ".");
    const std::string model = ResolveLocalRefineModelPath(models_dir, "");
    if (model.empty()) {
        printf("TestLlamaCppEngineRealModelSmoke SKIP（无精修模型；设 "
               "VOICESTICK_REFINE_MODEL 指向 Qwen3-1.7B GGUF 启用）\n");
        return;
    }
    printf("  model: %s\n", model.c_str()); fflush(stdout);
    auto t0 = std::chrono::steady_clock::now();
    auto engine = LlamaCppEngine::Create(model, 6);
    assert(engine != nullptr);
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    const std::string system = LocalRefinementClient::BuildSystemPrompt();
    const auto chat_once = [&](const std::string& text) {
        const auto start = std::chrono::steady_clock::now();
        std::string out;
        std::string partial;
        const bool ok = engine->Chat(
            system, "输入：" + text + "\n输出：",
            [&partial](std::string piece) {
                partial += piece;
                return true;
            },
            out);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        printf("  chat %lldms ok=%d out=%s\n", static_cast<long long>(ms),
               static_cast<int>(ok), out.substr(0, 60).c_str());
        fflush(stdout);
        return std::make_pair(ok, ms);
    };
    const auto [ok1, ms1] = chat_once("嗯，帮我把这个文件重命名一下。");
    const auto [ok2, ms2] = chat_once("呃，我想问一下今天的会议几点开始。");
    printf("  load=%lldms first=%lldms second=%lldms\n",
           static_cast<long long>(load_ms), static_cast<long long>(ms1),
           static_cast<long long>(ms2));
    assert(ok1 && ok2);
    // 预算宽松（构建机负载波动）：单句 ≤15s；KV 前缀复用后第二句不慢于首句。
    assert(ms1 <= 15000);
    assert(ms2 <= 15000);
    assert(ms2 <= ms1 + 500);
    printf("TestLlamaCppEngineRealModelSmoke passed\n");
#endif  // NDEBUG
#else
    printf("TestLlamaCppEngineRealModelSmoke SKIP（VOICESTICK_ENABLE_LOCAL_REFINE=OFF）\n");
#endif
}

void TestLocalAsrClientStartFailsWhenModelMissing() {
    LocalAsrClient client("Z:/voicestick/不存在的模型目录");
    assert(!client.Start());
    assert(!client.LastStartError().empty());
    printf("TestLocalAsrClientStartFailsWhenModelMissing passed\n");
}

// 跨轮纠错模型解析（M3）：cross_turn 且未显式配 refine_model 时优先 4B
//（M0 spike GO 口径），缺失回退 1.7B；显式配置尊重用户；两档全缺返回空
//（外壳退化为纯规则层）。
void TestResolveLocalRefineModelPathCrossTurn() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "vs_refine_path_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const auto m17 = dir / "Qwen3-1.7B-Q4_K_M" / "Qwen3-1.7B-Q4_K_M.gguf";
    const auto m4 = dir / "Qwen3-4B-Q4_K_M" / "Qwen3-4B-Q4_K_M.gguf";
    fs::create_directories(m17.parent_path(), ec);
    std::ofstream(m17, std::ios::binary) << "x";

    // 只有 1.7B：cross_turn 回退 1.7B，单句口径不受影响
    assert(ResolveLocalRefineModelPath(dir.string(), "", true) == m17.string());
    assert(ResolveLocalRefineModelPath(dir.string(), "", false) == m17.string());

    // 4B 在位：cross_turn 优先 4B；单句口径仍 1.7B（纯删除档维持轻量）
    fs::create_directories(m4.parent_path(), ec);
    std::ofstream(m4, std::ios::binary) << "x";
    assert(ResolveLocalRefineModelPath(dir.string(), "", true) == m4.string());
    assert(ResolveLocalRefineModelPath(dir.string(), "", false) == m17.string());

    // 显式配置（含跨轮开）：尊重用户自管，不按档位覆盖（显式值里的「/」
    // 分隔符被原样保留，用文件系统等价比较而非字符串相等）
    std::ofstream(dir / "Qwen3-1.7B-Q4_K_M" / "x.gguf", std::ios::binary) << "x";
    assert(fs::equivalent(
        fs::path(ResolveLocalRefineModelPath(dir.string(),
                                             "Qwen3-1.7B-Q4_K_M/x.gguf", true)),
        dir / "Qwen3-1.7B-Q4_K_M" / "x.gguf"));
    std::filesystem::remove(dir / "Qwen3-1.7B-Q4_K_M" / "x.gguf");

    // 两档全缺：空（外壳按缺模型退化纯规则）
    fs::remove(m4, ec);
    fs::remove(m17, ec);
    assert(ResolveLocalRefineModelPath(dir.string(), "", true).empty());
    fs::remove_all(dir, ec);
    printf("TestResolveLocalRefineModelPathCrossTurn passed\n");
}

// 4B 真模型 KV 续写 smoke（M2b）：验证 LlamaCppEngine::ChatSessionTurn 的
// 续例链形态（历史轮重放为 [输入：raw\n处理：][instruction]——形态自洽，
// assistant 侧=当轮模型真实指令输出）下——M0 spike 验证的是「上文：」行
// 形态，本测试证明 KV 续写形态效果等价：
// ①跨轮纠错指令仍产生且 ApplyPinyinCorrections 执行后命中期望（C 组案例
// 复刻）；②负例不误改；③第 6 轮触发滑窗重建后仍正常；④延迟收敛（观察值
// 打印）。env VOICESTICK_REFINE_MODEL_4B 或默认 m0/ 路径；不在位 SKIP——
// 不 mock 真实链路。Release only（GGML Debug 慢 20~40 倍）。
void TestLlamaCppEngineSessionTurnSmoke() {
    printf(">> TestLlamaCppEngineSessionTurnSmoke\n"); fflush(stdout);
#ifdef VOICESTICK_LOCAL_REFINE_ENABLED
#ifndef NDEBUG
    printf("TestLlamaCppEngineSessionTurnSmoke SKIP（Debug 构建推理慢，"
           "Release 专用）\n");
#else
    namespace fs = std::filesystem;
    const char* env = std::getenv("VOICESTICK_REFINE_MODEL_4B");
    fs::path model = (env && *env) ? fs::path(env)
                                   : fs::path("m0/models/Qwen3-4B-Q4_K_M")
                                         / "Qwen3-4B-Q4_K_M.gguf";
    if (!fs::exists(model)) {
        printf("TestLlamaCppEngineSessionTurnSmoke SKIP（无 4B 模型；设 "
               "VOICESTICK_REFINE_MODEL_4B 指向 Qwen3-4B GGUF 启用）\n");
        return;
    }
    printf("  model: %s\n", model.string().c_str()); fflush(stdout);
    auto engine = LlamaCppEngine::Create(model.string(), 6);
    if (!engine) {
        printf("   FAIL 引擎加载失败\n"); fflush(stdout);
        std::abort();
    }
    const std::string sys = LocalRefinementClient::BuildCorrectionSystemPrompt();

    // 会话轮（生产形态：轮 k 历史含前 k-1 轮，滑窗由调用方维护——手动推演）。
    // 案例复刻 M0 C 组（鱼器渍/蓝崖/设别/负例/章维）。
    struct Turn {
        const char* raw;        // 本轮 ASR 原文（指令执行前）
        const char* refined;    // 本轮期望精修结果（进守卫域）
        const char* note;
    };
    const Turn turns[] = {
        {"那些鱼器渍已经被过滤掉了。", "那些语气词已经被过滤掉了。", "C01 纠错"},
        {"这个蓝崖遥控器的按键手感不错。", "这个蓝牙遥控器的按键手感不错。", "C02 纠错"},
        {"这个语音设别模型是哪个？", "这个语音识别模型是哪个？", "C08 e/i 纠错"},
        {"帮我把垃圾倒一下。", "帮我把垃圾倒一下。", "负例不误改"},
        {"章维说他会晚点到。", "张伟说他会晚点到。", "C03 人名纠错"},
        {"嗯，帮我把这个文件重命名一下。", "帮我把这个文件重命名一下。",
         "第 6 轮：超 5 轮窗口触发滑窗重建"},
    };
    // 引擎历史（重放 assistant=当轮模型真实输出——指令形态自洽，防形态
    // 漂移为文本输出）与守卫域（各轮 refined 拼接）分开维护。
    // 建立轮（模拟此前口述）assistant=「无」（干净句的真实输出形态）。
    std::vector<std::pair<std::string, std::string>> engine_history = {
        {"我们刚才测了语气词过滤。", "无"},
    };
    std::vector<std::string> refined_history = {"我们刚才测了语气词过滤。"};
    const auto add_seed = [&](const char* text) {
        engine_history.emplace_back(text, "无");
        refined_history.emplace_back(text);
    };
    int corrected = 0;
    for (std::size_t k = 0; k < std::size(turns); ++k) {
        const auto& t = turns[k];
        if (k == 1) add_seed("帮我用蓝牙遥控器测试一下。");   // C02 建立蓝牙
        if (k == 2) add_seed("这个语音识别项目叫 VoiceStick。");  // C08 建立识别
        if (k == 4) add_seed("张伟下午的会议来不了。");        // C03 建立张伟
        const auto t0 = std::chrono::steady_clock::now();
        std::string completion;
        const bool ok = engine->ChatSessionTurn(
            sys, engine_history, "输入：" + std::string(t.raw) + "\n处理：",
            nullptr, completion);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        // Release（NDEBUG）下 assert 是 no-op——smoke 断言必须显式检查，
        // 否则假绿（教训同 TestImaAdpcmDecoderGoldenFixtures）。
        if (!ok || ms > 20000) {
            printf("   FAIL 轮%zu ok=%d ms=%lld\n", k + 1, (int)ok,
                   (long long)ms);
            fflush(stdout);
            std::abort();
        }
        // 守卫域=各轮 refined 拼接（client 语义同款）
        std::string context_all;
        for (const auto& r : refined_history) {
            if (!context_all.empty()) context_all += "。";
            context_all += r;
        }
        const auto stripped =
            LocalRefinementClient::StripReplyTemplate(completion);
        const auto outcome =
            ApplyPinyinCorrections(t.raw, stripped, context_all);
        // 轮 6（滑窗重建轮）目的=引擎存活且不误改：删「嗯，」或原样直通
        // 均算存活（删除指令是否产生不在重建路径验证范围）
        const bool hit = outcome.text == t.refined ||
                         (k == 5 && outcome.text == t.raw);
        printf("  轮%zu %s: %lldms completion=%.40s\n  final=%.30s (%s)\n",
               k + 1, t.note, static_cast<long long>(ms),
               completion.c_str(), outcome.text.c_str(), hit ? "HIT" : "MISS");
        fflush(stdout);
        if (hit) ++corrected;
        // 引擎历史推进：assistant=模型真实输出（形态自洽链）
        engine_history.emplace_back(t.raw, stripped);
        refined_history.push_back(outcome.text);
    }
    // 形态等价门槛：6 轮命中 ≥5（四纠错 + 负例 + 重建轮存活），对齐 M0
    // spike 4B「上文行」形态水平；负例（轮 4）不误改由期望文本断言覆盖
    printf("  命中 %d/6（含负例；门槛 5：四纠错 + 负例 + 重建轮存活）\n",
           corrected);
    if (corrected < 5) {
        fflush(stdout);
        std::abort();
    }
    printf("TestLlamaCppEngineSessionTurnSmoke passed\n");
#endif  // NDEBUG
#else
    printf("TestLlamaCppEngineSessionTurnSmoke SKIP（VOICESTICK_ENABLE_LOCAL_REFINE=OFF）\n");
#endif
}


void TestLocalAsrClientSenseVoiceSmoke() {
    const auto model_dir = DetectSenseVoiceDir();
    if (model_dir.empty()) {
        printf("TestLocalAsrClientSenseVoiceSmoke SKIPPED (模型不在位；"
               "设 VOICESTICK_SENSEVOICE_DIR 指向 SenseVoice 目录后重跑)\n");
        return;
    }
    std::vector<std::int16_t> pcm;
    assert(ReadMonoPcm16Wav(model_dir / "test_wavs" / "zh.wav", pcm));
    assert(pcm.size() >= AudioOpusEncoder::kFrameSamples);

    LocalAsrClient client(model_dir.string());
    assert(client.Start());

    std::mutex mutex;
    std::condition_variable done;
    std::vector<std::string> partial_texts;
    std::string final_text, error_text;
    bool finished = false;
    client.on_partial = [&](std::string text) {
        std::lock_guard<std::mutex> lock(mutex);
        partial_texts.push_back(std::move(text));
    };
    client.on_final = [&](std::string text) {
        std::lock_guard<std::mutex> lock(mutex);
        final_text = std::move(text);
        finished = true;
        done.notify_one();
    };
    client.on_error = [&](std::string error) {
        std::lock_guard<std::mutex> lock(mutex);
        error_text = std::move(error);
        finished = true;
        done.notify_one();
    };

    // PCM → Opus packets → Ogg 字节流，按 ~1s 一块流式发送（块间隔超过节流周期，
    // 复现"边说边识别"形态）：真模型应在录音期间就产出非空 partial。
    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    ByteVector block;
    int frames_in_block = 0;
    std::uint8_t packet[512];
    bool last = false;
    for (size_t off = 0; off < pcm.size() && !last;) {
        const size_t take = std::min<size_t>(AudioOpusEncoder::kFrameSamples,
                                             pcm.size() - off);
        std::vector<std::int16_t> frame(pcm.begin() + off, pcm.begin() + off + take);
        if (frame.size() < AudioOpusEncoder::kFrameSamples) {
            frame.resize(AudioOpusEncoder::kFrameSamples, 0);  // 尾帧补零
            last = true;
        }
        const auto result = encoder.Encode(frame.data(), frame.size(),
                                           packet, sizeof(packet));
        assert(result.encoded_bytes > 0);
        auto page = muxer.Append({packet, static_cast<size_t>(result.encoded_bytes)},
                                 false);
        block.insert(block.end(), page.begin(), page.end());
        off += take;
        if (++frames_in_block >= 25) {   // 25 帧 × 40ms = 1s
            client.SendOggOpusChunk(block, false);
            block.clear();
            frames_in_block = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }
    }
    auto tail = muxer.Finish();
    block.insert(block.end(), tail.begin(), tail.end());
    client.SendOggOpusChunk(block, true);

    std::unique_lock<std::mutex> lock(mutex);
    assert(done.wait_for(lock, std::chrono::seconds(60), [&] { return finished; }));
    assert(error_text.empty());
    assert(!final_text.empty());   // 真模型真推理：非空即链路通（不逐字断言）
    assert(!partial_texts.empty());   // 流式链路：录音期间至少一次非空 partial
    printf("TestLocalAsrClientSenseVoiceSmoke passed: partials=%zu final=%s\n",
           partial_texts.size(), final_text.c_str());
}

// ===== 本地 ASR 流式 partial（滚动重解码调度，假引擎驱动）=====

// 流式调度假引擎：Decode 返回 "n=<样本数>"（输入规模可直接断言），计数调用次数。
class FakeSenseVoiceEngine : public SenseVoiceEngine {
 public:
  std::string Decode(std::span<const std::int16_t> samples) override {
    std::lock_guard<std::mutex> lock(mutex);
    ++decode_calls;
    if (return_empty) return "";
    return "n=" + std::to_string(samples.size());
  }
  int DecodeCalls() const {
    std::lock_guard<std::mutex> lock(mutex);
    return decode_calls;
  }

  mutable std::mutex mutex;
  bool return_empty = false;

 private:
  int decode_calls = 0;
};


// 流式回调测试脚手架：partial 快照 + final/error 完成信号。
struct LocalAsrCallbacks {
    std::mutex mutex;
    std::condition_variable signal;
    int partial_calls = 0;
    std::vector<std::string> partial_texts;
    std::string final_text;
    std::string error_text;
    bool finished = false;

    void Install(LocalAsrClient& client) {
        client.on_partial = [this](std::string text) {
            std::lock_guard<std::mutex> lock(mutex);
            ++partial_calls;
            partial_texts.push_back(std::move(text));
            signal.notify_all();
        };
        client.on_final = [this](std::string text) {
            std::lock_guard<std::mutex> lock(mutex);
            final_text = std::move(text);
            finished = true;
            signal.notify_all();
        };
        client.on_error = [this](std::string error) {
            std::lock_guard<std::mutex> lock(mutex);
            error_text = std::move(error);
            finished = true;
            signal.notify_all();
        };
    }

    template <typename Pred>
    bool Wait(Pred pred, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex);
        return signal.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
    }
};


void TestLocalAsrClientEmitsPartialWhileStreaming() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    auto* fake = engine.get();
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    // 三块 1s 静音，块间隔 700ms > 600ms 节流周期：录音期间应持续产出 partial。
    for (int block = 0; block < 3; ++block) {
        auto chunk = EncodeSilenceFrames(encoder, muxer, 25);
        client.SendOggOpusChunk(chunk, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }
    assert(cb.Wait([&] { return cb.partial_calls >= 2; }, 5000));
    size_t prev_samples = 0;
    for (const auto& text : cb.partial_texts) {
        const size_t samples = SamplesFromFakeText(text);
        assert(samples >= prev_samples);   // 输入单调不减（增量解码不丢不重）
        prev_samples = samples;
    }
    // is_last 附带新音频：最终推理覆盖全量 3s（75 帧 × 640 样本）。
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    assert(cb.Wait([&] { return cb.finished; }, 5000));
    assert(cb.error_text.empty());
    assert(cb.final_text == "n=48000");
    assert(fake->DecodeCalls() >= 3);   // 至少：首块 + 两次周期 + final
    printf("TestLocalAsrClientEmitsPartialWhileStreaming passed: partials=%d\n",
           cb.partial_calls);
}

void TestLocalAsrClientPartialThrottled() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    auto* fake = engine.get();
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    // 五块 0.48s 静音在远小于节流周期的窗口内连发：无节流会逐块解码 5 次。
    for (int block = 0; block < 5; ++block) {
        auto chunk = EncodeSilenceFrames(encoder, muxer, 12);
        client.SendOggOpusChunk(chunk, false);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int calls = fake->DecodeCalls();
    assert(calls >= 1);
    assert(calls <= 3);   // 无节流 = 5
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    assert(cb.Wait([&] { return cb.finished; }, 5000));
    assert(cb.error_text.empty());
    printf("TestLocalAsrClientPartialThrottled passed: decode_calls=%d\n", calls);
}

void TestLocalAsrClientFinalReusesDecodeWhenNoNewAudio() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    auto* fake = engine.get();
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    auto chunk = EncodeSilenceFrames(encoder, muxer, 25);
    client.SendOggOpusChunk(chunk, false);
    assert(cb.Wait([&] { return cb.partial_calls >= 1; }, 5000));
    assert(fake->DecodeCalls() == 1);   // 首块立即解码一次
    // 尾页不含新音频 packet：is_last 应复用上次推理结果，不重复解码（松键秒出 final）。
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    assert(cb.Wait([&] { return cb.finished; }, 5000));
    assert(cb.error_text.empty());
    assert(cb.final_text == cb.partial_texts.back());
    assert(fake->DecodeCalls() == 1);
    printf("TestLocalAsrClientFinalReusesDecodeWhenNoNewAudio passed\n");
}

void TestLocalAsrClientSkipsEmptyPartial() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    engine->return_empty = true;
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    for (int block = 0; block < 2; ++block) {
        auto chunk = EncodeSilenceFrames(encoder, muxer, 25);
        client.SendOggOpusChunk(chunk, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    assert(cb.Wait([&] { return cb.finished; }, 5000));
    assert(cb.partial_calls == 0);   // 空文本 partial 不上报（保持 Listening 显示）
    assert(cb.error_text.empty());
    assert(cb.final_text.empty());   // 引擎返回空 → final 照发（沿用现行为）
    printf("TestLocalAsrClientSkipsEmptyPartial passed\n");
}

void TestLocalAsrClientCancelStopsPartial() {
    auto engine = std::make_unique<FakeSenseVoiceEngine>();
    LocalAsrClient client("Z:/voicestick/无模型目录（假引擎不校验）", 2, std::move(engine));
    assert(client.Start());

    LocalAsrCallbacks cb;
    cb.Install(client);

    AudioOpusEncoder encoder;
    OggOpusMuxer muxer(AudioOpusEncoder::kSampleRate, AudioOpusEncoder::kChannels);
    auto chunk = EncodeSilenceFrames(encoder, muxer, 25);
    client.SendOggOpusChunk(chunk, false);
    assert(cb.Wait([&] { return cb.partial_calls >= 1; }, 5000));

    client.Cancel();
    const int partials_before = cb.partial_calls;
    auto more = EncodeSilenceFrames(encoder, muxer, 25);
    client.SendOggOpusChunk(more, false);
    auto tail = muxer.Finish();
    client.SendOggOpusChunk(tail, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    assert(cb.partial_calls == partials_before);   // 取消后不再产出 partial
    assert(!cb.finished);                          // 也不再有 final/error
    printf("TestLocalAsrClientCancelStopsPartial passed\n");
}

// ===== 本机麦克风模式（local-mic，迭代二）=====

void TestPushToTalkKeyParsing() {
    assert(*ParsePushToTalkKey("right ctrl") == VK_RCONTROL);
    assert(*ParsePushToTalkKey("Right Ctrl") == VK_RCONTROL);   // 大小写/空白不敏感
    assert(*ParsePushToTalkKey(" left ctrl ") == VK_LCONTROL);
    assert(*ParsePushToTalkKey("right shift") == VK_RSHIFT);
    assert(*ParsePushToTalkKey("left shift") == VK_LSHIFT);
    assert(*ParsePushToTalkKey("right alt") == VK_RMENU);
    assert(*ParsePushToTalkKey("left alt") == VK_LMENU);
    assert(*ParsePushToTalkKey("f8") == VK_F8);
    assert(*ParsePushToTalkKey("F24") == VK_F24);
    assert(*ParsePushToTalkKey("capslock") == VK_CAPITAL);
    assert(!ParsePushToTalkKey("ctrl").has_value());     // 左右歧义，拒绝
    assert(!ParsePushToTalkKey("ctrl+c").has_value());   // 组合键不支持（按住说话=单键）
    assert(!ParsePushToTalkKey("foo").has_value());
    assert(!ParsePushToTalkKey("").has_value());
}

void TestFormatPushToTalkKey() {
    // 哨兵：stub 阶段（恒 nullopt）在此干净失败，避免下方解引用空 optional 的 UB。
    assert(FormatPushToTalkKey(VK_RCONTROL).has_value());
    // 命名键：主名（同义 escape/return 取首见的 esc/enter）。
    assert(*FormatPushToTalkKey(VK_RCONTROL) == "right ctrl");
    assert(*FormatPushToTalkKey(VK_LCONTROL) == "left ctrl");
    assert(*FormatPushToTalkKey(VK_RSHIFT) == "right shift");
    assert(*FormatPushToTalkKey(VK_LSHIFT) == "left shift");
    assert(*FormatPushToTalkKey(VK_RMENU) == "right alt");
    assert(*FormatPushToTalkKey(VK_LMENU) == "left alt");
    assert(*FormatPushToTalkKey(VK_CAPITAL) == "capslock");
    assert(*FormatPushToTalkKey(VK_SCROLL) == "scrolllock");
    assert(*FormatPushToTalkKey(VK_PAUSE) == "pause");
    assert(*FormatPushToTalkKey(VK_ESCAPE) == "esc");
    assert(*FormatPushToTalkKey(VK_SPACE) == "space");
    assert(*FormatPushToTalkKey(VK_TAB) == "tab");
    assert(*FormatPushToTalkKey(VK_RETURN) == "enter");
    assert(*FormatPushToTalkKey(VK_BACK) == "backspace");
    // 功能键边界与单字符键。
    assert(*FormatPushToTalkKey(VK_F1) == "f1");
    assert(*FormatPushToTalkKey(VK_F9) == "f9");
    assert(*FormatPushToTalkKey(VK_F24) == "f24");
    assert(*FormatPushToTalkKey('A') == "a");
    assert(*FormatPushToTalkKey('Z') == "z");
    assert(*FormatPushToTalkKey('0') == "0");
    assert(*FormatPushToTalkKey('9') == "9");
    // Parse 本就不收的键：无键名。
    assert(!FormatPushToTalkKey(VK_LWIN).has_value());
    assert(!FormatPushToTalkKey(VK_UP).has_value());
    assert(!FormatPushToTalkKey(VK_NUMPAD0).has_value());
    assert(!FormatPushToTalkKey(VK_F24 + 1u).has_value());  // F25 起越界（SDK 无 VK_F25 常量）
    // 往返一致性：Format 输出可被 Parse 还原为同一 VK（全支持域）。
    const UINT round_trip_keys[] = {VK_RCONTROL, VK_LCONTROL, VK_RSHIFT, VK_LSHIFT,
                                    VK_RMENU,    VK_LMENU,    VK_CAPITAL, VK_SCROLL,
                                    VK_PAUSE,    VK_ESCAPE,   VK_SPACE,   VK_TAB,
                                    VK_RETURN,   VK_BACK,     VK_F1,      VK_F13,
                                    VK_F24,      'A',         'M',        'Z',
                                    '0',         '5',         '9'};
    for (UINT vk : round_trip_keys) {
        const auto name = FormatPushToTalkKey(vk);
        assert(name.has_value());
        const auto parsed = ParsePushToTalkKey(*name);
        assert(parsed.has_value());
        assert(*parsed == vk);
    }
}

// ===== 语音识别设置合并（本地识别为服务提供方末位虚拟项）=====

void TestProviderComboMapping() {
    // 无 legacy cloud 项：条目序 [Volcengine=0, Tencent=1, 本地=2]。
    assert(ProviderComboLocalIndex(false) == 2);
    assert(ProviderComboCloudAt(0, false) == AsrProvider::kVolcengine);
    assert(ProviderComboCloudAt(1, false) == AsrProvider::kTencent);
    assert(ProviderComboIsLocal(2, false));
    assert(!ProviderComboIsLocal(0, false));
    assert(!ProviderComboIsLocal(1, false));
    assert(ProviderComboCloudIndexOf(AsrProvider::kVolcengine, false) == 0);
    assert(ProviderComboCloudIndexOf(AsrProvider::kTencent, false) == 1);
    // 带 legacy cloud 项（老配置 asr_provider=voicestick_cloud 时 0 号位临时插入）：
    // 条目序 [Cloud=0, Volcengine=1, Tencent=2, 本地=3]。
    assert(ProviderComboLocalIndex(true) == 3);
    assert(ProviderComboCloudAt(0, true) == AsrProvider::kVoiceStickCloud);
    assert(ProviderComboCloudAt(1, true) == AsrProvider::kVolcengine);
    assert(ProviderComboCloudAt(2, true) == AsrProvider::kTencent);
    assert(ProviderComboIsLocal(3, true));
    assert(!ProviderComboIsLocal(2, true));
    assert(ProviderComboCloudIndexOf(AsrProvider::kVoiceStickCloud, true) == 0);
    assert(ProviderComboCloudIndexOf(AsrProvider::kVolcengine, true) == 1);
    assert(ProviderComboCloudIndexOf(AsrProvider::kTencent, true) == 2);
    // 越界防御：CB_ERR(-1) 或超界索引一律判非本地，不误触发模型目录行显隐。
    assert(!ProviderComboIsLocal(-1, false));
    assert(!ProviderComboIsLocal(-1, true));
    assert(!ProviderComboIsLocal(99, false));
}

void TestResolveAndValidateModelsDir() {
    namespace fs = std::filesystem;
    // 解析口径：空 = exe_dir/models；相对路径锚 exe 目录；绝对路径原样。
    // 经 fs::path 比较（path 拼接用反斜杠分隔符，字符串形态不作断言目标）。
    assert(fs::path(ResolveLocalMicModelsDir("", "C:/app")) == fs::path("C:/app/models"));
    assert(fs::path(ResolveLocalMicModelsDir("models", "C:/app")) ==
           fs::path("C:/app/models"));
    assert(fs::path(ResolveLocalMicModelsDir("rel/models", "C:/app")) ==
           fs::path("C:/app/rel/models"));
    assert(fs::path(ResolveLocalMicModelsDir("D:/mymodels", "C:/app")) ==
           fs::path("D:/mymodels"));
    // 校验口径：目录缺任一模型文件即报错，齐全才通过（与 Start 同判定）。
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "voicestick_models_validate_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // 哨兵：stub（恒通过）在此干净失败。
    assert(ValidateSenseVoiceModelsDir(dir.string()).has_value());
    { std::ofstream out(dir / "model.int8.onnx", std::ios::binary); }
    assert(ValidateSenseVoiceModelsDir(dir.string()).has_value());  // 只有一半
    { std::ofstream out(dir / "tokens.txt", std::ios::binary); }
    assert(!ValidateSenseVoiceModelsDir(dir.string()).has_value());  // 齐全 → 通过
    fs::remove_all(dir);
    // 不存在的目录同样报错。
    assert(ValidateSenseVoiceModelsDir("Z:/definitely/not/here").has_value());
}

void TestShortcutCaptureClassifyKey() {
    // 默认（按键映射场景，require_modifier=false）：修饰键累积、主键直接捕获。
    ShortcutCapture::Options single;
    assert(ShortcutCapture::ClassifyKey(VK_RCONTROL, single, false) ==
           ShortcutCapture::KeyAction::kAccumulateModifier);
    assert(ShortcutCapture::ClassifyKey(VK_LSHIFT, single, true) ==
           ShortcutCapture::KeyAction::kAccumulateModifier);
    assert(ShortcutCapture::ClassifyKey(VK_LWIN, single, false) ==
           ShortcutCapture::KeyAction::kAccumulateModifier);
    assert(ShortcutCapture::ClassifyKey('A', single, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_F9, single, false) ==
           ShortcutCapture::KeyAction::kCapture);

    // 全局热键场景（require_modifier=true）：裸主键拒绝，带修饰键捕获。
    ShortcutCapture::Options combo;
    combo.require_modifier = true;
    assert(ShortcutCapture::ClassifyKey('A', combo, false) ==
           ShortcutCapture::KeyAction::kRejectNoModifier);
    assert(ShortcutCapture::ClassifyKey('A', combo, true) ==
           ShortcutCapture::KeyAction::kCapture);

    // 按住说话场景（allow_modifier_as_key）：修饰键左右变体直接作为主键捕获，
    // 不再累积等待（right ctrl 即功能键本身）。
    ShortcutCapture::Options ptt;
    ptt.allow_modifier_as_key = true;
    assert(ShortcutCapture::ClassifyKey(VK_RCONTROL, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_LCONTROL, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_LSHIFT, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_LWIN, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);
    assert(ShortcutCapture::ClassifyKey(VK_CAPITAL, ptt, false) ==
           ShortcutCapture::KeyAction::kCapture);

    // Esc 恒为取消（任何模式下都不作为主键捕获）。
    assert(ShortcutCapture::ClassifyKey(VK_ESCAPE, single, false) ==
           ShortcutCapture::KeyAction::kCancel);
    assert(ShortcutCapture::ClassifyKey(VK_ESCAPE, combo, true) ==
           ShortcutCapture::KeyAction::kCancel);
    assert(ShortcutCapture::ClassifyKey(VK_ESCAPE, ptt, false) ==
           ShortcutCapture::KeyAction::kCancel);
}

void TestShortcutCapturePollEligibleVk() {
    // 鼠标键与保留区不采纳：避免点击「录入」按钮/切换窗口被误判为按键。
    assert(!ShortcutCapture::IsPollEligibleVk(0x00));
    assert(!ShortcutCapture::IsPollEligibleVk(VK_LBUTTON));
    assert(!ShortcutCapture::IsPollEligibleVk(VK_RBUTTON));
    assert(!ShortcutCapture::IsPollEligibleVk(VK_MBUTTON));
    assert(!ShortcutCapture::IsPollEligibleVk(VK_XBUTTON2));
    assert(!ShortcutCapture::IsPollEligibleVk(0x07));
    assert(!ShortcutCapture::IsPollEligibleVk(0xFF));
    // 键盘区全覆盖：Backspace/字母/方向/功能/媒体键均可经轮询兜底捕获。
    assert(ShortcutCapture::IsPollEligibleVk(VK_BACK));
    assert(ShortcutCapture::IsPollEligibleVk('A'));
    assert(ShortcutCapture::IsPollEligibleVk(VK_UP));
    assert(ShortcutCapture::IsPollEligibleVk(VK_LCONTROL));
    assert(ShortcutCapture::IsPollEligibleVk(VK_F13));
    assert(ShortcutCapture::IsPollEligibleVk(VK_VOLUME_UP));
    assert(ShortcutCapture::IsPollEligibleVk(VK_BROWSER_BACK));
    assert(ShortcutCapture::IsPollEligibleVk(0xFE));
}

void TestAppConfigLocalAsrRoundTrip() {
    assert(!AppConfig::Defaults().local_asr.enabled);
    assert(AppConfig::Defaults().local_asr.models_dir.empty());
    assert(AppConfig::Defaults().local_asr.push_to_talk_key == "right ctrl");
    // 跨轮纠错默认关（M3：改写能力以设置开关观察）
    assert(!AppConfig::Defaults().local_asr.refine_cross_turn);

    auto temp = std::filesystem::temp_directory_path() / "voicestick_local_asr_test.toml";
    std::filesystem::remove(temp);

    AppConfig config = AppConfig::Defaults();
    config.local_asr.enabled = true;
    config.local_asr.models_dir = "C:/models/sensevoice";
    config.local_asr.push_to_talk_key = "f8";
    config.local_asr.refine_prompt = "提示词第一行\n输入：嗯 x\n输出：x";
    config.local_asr.refine_cross_turn = true;
    config.Save(temp);

    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.local_asr.enabled);
    assert(loaded.local_asr.models_dir == "C:/models/sensevoice");
    assert(loaded.local_asr.push_to_talk_key == "f8");
    // 多行提示词（含换行与中文）往返保持原样
    assert(loaded.local_asr.refine_prompt == config.local_asr.refine_prompt);
    assert(loaded.local_asr.refine_cross_turn);

    // 默认关不落盘（Save 降噪路径）：重存关闭态后文件中无该键
    loaded.local_asr.refine_cross_turn = false;
    loaded.Save(temp);
    const std::string saved = [&] {
        std::ifstream in(temp, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();
    assert(saved.find("refine_cross_turn") == std::string::npos);

    std::filesystem::remove(temp);
}

// 按住说话全链路：热键按下建立 local-mic 会话并启动采集；PCM 喂入走 Opus 主会话
// 管线；释放后尾帧+END 收尾，音频路由到本地 ASR（云端客户端零触碰），final 文本注入。
void TestWasapiMicCaptureSmoke() {
    WasapiMicCapture capture;
    std::mutex mutex;
    std::condition_variable got_pcm;
    std::size_t total_samples = 0;
    int callbacks = 0;
    capture.on_pcm = [&](std::span<const std::int16_t> pcm) {
        std::lock_guard<std::mutex> lock(mutex);
        total_samples += pcm.size();
        ++callbacks;
        got_pcm.notify_one();
    };
    printf("  wasapi: Start()...\n"); fflush(stdout);
    if (!capture.Start()) {
        printf("TestWasapiMicCaptureSmoke skipped: %s\n", capture.LastStartError().c_str());
        return;
    }
    printf("  wasapi: started, waiting pcm\n"); fflush(stdout);
    bool received = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        received = got_pcm.wait_for(lock, std::chrono::seconds(8),
                                    [&] { return total_samples >= 16000; });
    }
    printf("  wasapi: wait done received=%d samples=%zu callbacks=%d\n",
           received ? 1 : 0, total_samples, callbacks); fflush(stdout);
    assert(received);
    assert(total_samples >= 16000);
    capture.Stop();
    capture.Stop();  // 幂等
    printf("TestWasapiMicCaptureSmoke passed: %d callbacks, %zu samples\n",
           callbacks, total_samples);
}

// ===== 剪贴板 vault（迭代三：完整格式恢复，移植自 P1 clipboard_vault）=====
// 真 Win32 剪贴板，非 mock：字节级快照/恢复是本组件的全部契约。测试动本机
// 剪贴板，开头快照用户当前内容，结尾尽力还原。

namespace {





} // namespace

void TestClipboardVaultMultiFormatRoundTrip() {
    // 快照/恢复用户当前剪贴板，尽力不破坏现场。
    std::optional<ClipboardSnapshot> user_content;
    try {
        user_content = ClipboardVault().Save();
    } catch (const std::runtime_error&) {
    }

    const UINT custom_fmt = RegisterClipboardFormatW(L"VoiceStickVaultTestFmt");
    assert(custom_fmt != 0);
    const std::vector<BYTE> dib(64, 0xAB);  // 伪 DIB：剪贴板不校验内容
    const std::vector<BYTE> custom{0x00, 0x01, 0xFF, 0x00, 0x7F};
    const auto text_bytes = VaultBytesOf(L"原始内容-restore");
    VaultSetClipboard({{CF_UNICODETEXT, text_bytes}, {CF_DIB, dib}, {custom_fmt, custom}});

    ClipboardVault vault;
    const ClipboardSnapshot snapshot = vault.Save();
    const auto* text_entry = snapshot.Find(CF_UNICODETEXT);
    const auto* dib_entry = snapshot.Find(CF_DIB);
    const auto* custom_entry = snapshot.Find(custom_fmt);
    assert(text_entry && text_entry->data == text_bytes);
    assert(dib_entry && dib_entry->data == dib);
    assert(custom_entry && custom_entry->data == custom);

    // 快照后剪贴板被异物覆盖，恢复必须还原快照字节（注入借道剪贴板的核心场景）。
    VaultSetClipboard({{CF_UNICODETEXT, VaultBytesOf(L"覆盖内容")}});
    assert(vault.Restore(snapshot));
    assert(VaultGetBytes(CF_UNICODETEXT) == text_bytes);
    assert(VaultGetBytes(CF_DIB) == dib);
    assert(VaultGetBytes(custom_fmt) == custom);

    if (user_content) ClipboardVault().Restore(*user_content);
}

void TestClipboardVaultSkipsHandleFormats() {
    std::optional<ClipboardSnapshot> user_content;
    try {
        user_content = ClipboardVault().Save();
    } catch (const std::runtime_error&) {
    }

    // CF_BITMAP 是句柄类格式（非 HGLOBAL，GlobalLock 无意义）：Save 必须跳过；
    // 恢复后位图丢失为已知限制（位图场景应用几乎都同时提供 CF_DIB 内存版）。
    assert(VaultOpenClipboardWithRetry());
    EmptyClipboard();
    {
        const wchar_t* text = L"带位图的文本";
        const SIZE_T bytes = (wcslen(text) + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        void* ptr = GlobalLock(memory);
        assert(ptr != nullptr);
        memcpy(ptr, text, bytes);
        GlobalUnlock(memory);
        assert(SetClipboardData(CF_UNICODETEXT, memory));
    }
    const HBITMAP bitmap = CreateBitmap(1, 1, 1, 1, nullptr);
    assert(bitmap != nullptr);
    assert(SetClipboardData(CF_BITMAP, bitmap));  // 句柄移交剪贴板，不得 DeleteObject
    CloseClipboard();

    ClipboardVault vault;
    const ClipboardSnapshot snapshot = vault.Save();
    assert(snapshot.Find(CF_UNICODETEXT) != nullptr);
    assert(snapshot.Find(CF_BITMAP) == nullptr);  // 句柄格式不进快照
    // 布置 CF_BITMAP 时系统枚举会同时给出可从位图合成的 CF_DIB，快照经 CF_DIB
    // 保住图像字节——恢复后系统可再合成位图，图像内容实际不丢（优于“直接丢弃”）。
    const auto* dib_entry = snapshot.Find(CF_DIB);

    VaultSetClipboard({{CF_UNICODETEXT, VaultBytesOf(L"覆盖")}});
    assert(vault.Restore(snapshot));
    assert(VaultGetBytes(CF_UNICODETEXT) == snapshot.Find(CF_UNICODETEXT)->data);
    if (dib_entry != nullptr) {
        assert(VaultGetBytes(CF_DIB) == dib_entry->data);
        assert(IsClipboardFormatAvailable(CF_BITMAP));  // 从 CF_DIB 可再合成位图
    }

    if (user_content) ClipboardVault().Restore(*user_content);
}

void TestClipboardVaultEmptyClipboardSnapshot() {
    std::optional<ClipboardSnapshot> user_content;
    try {
        user_content = ClipboardVault().Save();
    } catch (const std::runtime_error&) {
    }

    assert(VaultOpenClipboardWithRetry());
    EmptyClipboard();
    CloseClipboard();

    ClipboardVault vault;
    const ClipboardSnapshot snapshot = vault.Save();
    assert(snapshot.entries.empty());  // 空快照只代表真空剪贴板

    // 空快照恢复 = 清空剪贴板（区别于“打不开”：那是 Save 抛错的职责，防误清）。
    VaultSetClipboard({{CF_UNICODETEXT, VaultBytesOf(L"x")}});
    assert(vault.Restore(snapshot));
    assert(!IsClipboardFormatAvailable(CF_UNICODETEXT));

    if (user_content) ClipboardVault().Restore(*user_content);
}

void TestClipboardVaultSaveThrowsWhenBusy() {
    // 另一线程持有剪贴板：Save 必须抛错而非返回空快照——空快照会让 Restore
    // 误清用户剪贴板（P1 验证语义）。本线程重试窗口 2×10ms，持有 80ms 必失败。
    std::atomic<bool> held{false};
    std::thread holder([&held] {
        if (OpenClipboard(nullptr)) {
            held = true;
            Sleep(80);
            CloseClipboard();
        }
    });
    bool opened = false;
    for (int i = 0; i < 500 && !held; ++i) {  // 等持有方拿到锁（最多 500ms）
        Sleep(1);
        opened = opened || held;
    }
    if (!held) {
        holder.join();
        printf("TestClipboardVaultSaveThrowsWhenBusy skipped: holder open failed\n");
        return;
    }
    ClipboardVault vault(/*open_retries=*/2, /*retry_delay_ms=*/10);
    bool threw = false;
    try {
        (void)vault.Save();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    holder.join();
}

// ================== 本地模型分发（Doc/Plan/local-model-distribution.md） ==================

namespace {



// 回环 HTTP 服务（真 Winsock 真 HTTP 报文）：WinHTTP 走真实回环 TCP 连接，
// 不 mock 网络栈（不伪造原则）。单请求/连接，Connection: close。
class LoopbackHttpServer {
 public:
    struct Response {
        int status = 200;
        std::string body;
        bool honor_range = true;   // 支持 Range → 206 + Content-Range
        std::size_t chunk_size = 0;  // 0 = 一次性发送；否则分块
        int chunk_delay_ms = 0;      // 分块间隔（取消测试用）
    };
    // 未注册路径一律 404。range_start 仅当请求带 Range 时有效。
    using Rules = std::map<std::string, Response>;

    explicit LoopbackHttpServer(Rules rules) : rules_(std::move(rules)) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::fprintf(stderr, "LoopbackHttpServer: WSAStartup failed\n");
            std::abort();
        }
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) {
            std::fprintf(stderr, "LoopbackHttpServer: socket failed\n");
            std::abort();
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;  // 随机端口，避免并行会话互踩
        if (bind(listen_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listen_, 8) != 0) {
            std::fprintf(stderr, "LoopbackHttpServer: bind/listen failed\n");
            std::abort();
        }
        sockaddr_in bound{};
        int bound_size = sizeof(bound);
        if (getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
            std::fprintf(stderr, "LoopbackHttpServer: getsockname failed\n");
            std::abort();
        }
        port_ = ntohs(bound.sin_port);
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~LoopbackHttpServer() {
        stopping_.store(true);
        closesocket(listen_);  // 唤醒阻塞中的 accept
        if (accept_thread_.joinable()) accept_thread_.join();
        WSACleanup();
    }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    std::string Url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    // 取走已收到的 Range 头原文（含 "Range:" 前缀），供断言真的发了续传请求。
    std::vector<std::string> TakeRangeHeaders() {
        std::lock_guard<std::mutex> lock(ranges_mutex_);
        return std::exchange(range_headers_, {});
    }

 private:
    void AcceptLoop() {
        while (!stopping_.load()) {
            sockaddr_in peer{};
            int peer_size = sizeof(peer);
            SOCKET client = accept(listen_, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (client == INVALID_SOCKET) break;  // closesocket 触发，正常退出
            std::thread([this, client] { ServeConnection(client); }).detach();
        }
    }

    void ServeConnection(SOCKET client) {
        std::string request;
        char buffer[1024];
        for (;;) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            request.append(buffer, got);
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }
        const auto first_line_end = request.find("\r\n");
        const std::string first_line = request.substr(0, first_line_end);
        const auto path_begin = first_line.find(' ');
        const auto path_end = first_line.rfind(' ');
        if (path_begin == std::string::npos || path_end == std::string::npos ||
            path_end <= path_begin) {
            closesocket(client);
            return;
        }
        const std::string path =
            first_line.substr(path_begin + 1, path_end - path_begin - 1);

        // 找行首 "range:" 头并解析 "bytes=N-"（大小写不敏感）。
        bool has_range = false;
        std::uint64_t range_start = 0;
        std::string lowered;
        lowered.reserve(request.size());
        for (const char ch : request) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        for (std::size_t pos = lowered.find("range:"); pos != std::string::npos;
             pos = lowered.find("range:", pos + 6)) {
            if (pos != 0 && lowered[pos - 1] != '\n') continue;
            std::size_t value_begin = pos + 6;
            while (value_begin < request.size() &&
                   (request[value_begin] == ' ' || request[value_begin] == '\t')) {
                ++value_begin;
            }
            const std::size_t line_end = lowered.find("\r\n", pos);
            const std::string value =
                request.substr(value_begin, line_end == std::string::npos
                                                ? std::string::npos
                                                : line_end - value_begin);
            if (value.rfind("bytes=", 0) == 0) {
                const auto num_end = value.find('-', 6);
                if (num_end != std::string::npos && num_end > 6) {
                    range_start = std::stoull(value.substr(6, num_end - 6));
                    has_range = true;
                    std::lock_guard<std::mutex> lock(ranges_mutex_);
                    range_headers_.push_back(value);
                }
            }
            break;
        }

        Response response;  // 未注册路径 → 404
        if (const auto rule = rules_.find(path); rule != rules_.end()) {
            response = rule->second;
        } else {
            response.status = 404;
        }
        std::string body = response.body;
        int status = response.status;
        if (response.status == 200 && response.honor_range && has_range) {
            status = 206;
            body = range_start >= body.size() ? std::string() : body.substr(range_start);
        }
        std::string headers = "HTTP/1.1 " + std::to_string(status) + "\r\n";
        if (status == 206) {
            headers += "Content-Range: bytes " + std::to_string(range_start) + "-" +
                       std::to_string(response.body.empty() ? 0 : response.body.size() - 1) +
                       "/" + std::to_string(response.body.size()) + "\r\n";
        }
        headers += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        headers += "Connection: close\r\n\r\n";
        send(client, headers.data(), static_cast<int>(headers.size()), 0);
        if (response.chunk_size == 0) {
            send(client, body.data(), static_cast<int>(body.size()), 0);
        } else {
            for (std::size_t offset = 0; offset < body.size() && !stopping_.load();
                 offset += response.chunk_size) {
                const std::size_t n = std::min(response.chunk_size, body.size() - offset);
                send(client, body.data() + offset, static_cast<int>(n), 0);
                Sleep(response.chunk_delay_ms);
            }
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
    }

    Rules rules_;
    SOCKET listen_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
    std::mutex ranges_mutex_;
    std::vector<std::string> range_headers_;
};





}  // namespace

void TestBundledModelManifestWellFormed() {
    int failed = 0;
    const auto& bundled = BundledModelEntries();

    // 内置清单的具体值与 m0/models 权威副本一致（哈希/体积为实测回填）。
    if (bundled.size() != 2) {
        std::printf("FAIL manifest 应有 2 个条目，实际 %zu\n", bundled.size());
        ++failed;
    }
    if (!bundled.empty() && bundled[0].kind != ModelKind::kAsr) {
        std::printf("FAIL 首条目应为 kAsr\n");
        ++failed;
    }
    if (!bundled.empty() && !bundled[0].required) {
        std::printf("FAIL ASR 条目应为必选\n");
        ++failed;
    }
    if (bundled.size() > 0 && bundled[0].files.size() == 2) {
        const auto& onnx = bundled[0].files[0];
        if (onnx.rel_path != "model.int8.onnx" || onnx.bytes != 239233841 ||
            onnx.sha256.rfind("c71f0ce00bec95b07744e116345e33d8", 0) != 0 ||
            onnx.urls.size() != 4) {
            std::printf("FAIL onnx 文件描述与权威副本不符\n");
            ++failed;
        }
        const auto& tokens = bundled[0].files[1];
        if (tokens.rel_path != "tokens.txt" || tokens.bytes != 315894 ||
            tokens.urls.size() != 4) {
            std::printf("FAIL tokens 文件描述与权威副本不符\n");
            ++failed;
        }
    } else if (bundled.size() > 0) {
        std::printf("FAIL ASR 条目应含 2 个文件\n");
        ++failed;
    }
    if (bundled.size() > 1) {
        const auto& refine = bundled[1];
        if (refine.kind != ModelKind::kRefine || refine.required ||
            refine.files.size() != 1 ||
            refine.files[0].rel_path != "Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf" ||
            refine.files[0].bytes != 1107409472 ||
            refine.files[0].sha256.rfind("b139949c5bd74937ad8ed8c8cf3d9ffb", 0) != 0) {
            std::printf("FAIL 精修条目与权威副本不符\n");
            ++failed;
        }
    }
    if (!ModelEntriesWellFormed(bundled)) {
        std::printf("FAIL 内置清单应通过自洽校验\n");
        ++failed;
    }

    // 分发源分布护栏（Doc/Ref/cos-distribution.md）：四源回退——
    // 正式域名直出首位（DNS 未配时快速失败自动回退，无需改清单）、
    // myqcloud 直出、ModelScope 免费分流、GitHub Release 海外回退。
    for (const auto& entry : bundled) {
        for (const auto& file : entry.files) {
            if (file.urls.empty() ||
                file.urls[0].rfind("https://dl.davenger.cloud/models/", 0) != 0) {
                std::printf("FAIL %s 首位源应为正式域名直出\n",
                            file.rel_path.c_str());
                ++failed;
            }
            const auto has_host = [&file](const char* needle) {
                for (const auto& url : file.urls) {
                    if (url.find(needle) != std::string::npos) return true;
                }
                return false;
            };
            if (!has_host(".myqcloud.com/")) {
                std::printf("FAIL %s 应含 myqcloud 直出回退源\n",
                            file.rel_path.c_str());
                ++failed;
            }
            if (!has_host("modelscope.cn/")) {
                std::printf("FAIL %s 应含 ModelScope 分流源\n",
                            file.rel_path.c_str());
                ++failed;
            }
            if (!has_host("github.com/")) {
                std::printf("FAIL %s 应含 GitHub Release 回退源\n",
                            file.rel_path.c_str());
                ++failed;
            }
        }
    }

    // 自洽校验的拒绝分支（构造畸形清单逐字段破坏）。
    const auto valid_entry = [] {
        ModelEntrySpec entry;
        entry.kind = ModelKind::kAsr;
        entry.required = true;
        ModelFileSpec file;
        file.rel_path = "a.onnx";
        file.bytes = 1;
        file.sha256 = std::string(64, 'a');
        file.urls = {"https://host/path"};
        entry.files = {std::move(file)};
        return entry;
    };
    const std::vector<ModelEntrySpec> valid = {valid_entry()};
    if (!ModelEntriesWellFormed(valid)) {
        std::printf("FAIL 合法清单不应被拒\n");
        ++failed;
    }
    if (ModelEntriesWellFormed({})) {
        std::printf("FAIL 空清单应被拒\n");
        ++failed;
    }
    auto reject = [&](const char* why, std::vector<ModelEntrySpec> broken) {
        if (ModelEntriesWellFormed(broken)) {
            std::printf("FAIL %s 应被拒\n", why);
            ++failed;
        }
    };
    auto no_files = valid_entry();
    no_files.files.clear();
    reject("空 files", {no_files});
    auto empty_rel = valid_entry();
    empty_rel.files[0].rel_path.clear();
    reject("空 rel_path", {empty_rel});
    auto backslash_rel = valid_entry();
    backslash_rel.files[0].rel_path = "a\\b.onnx";
    reject("反斜杠 rel_path", {backslash_rel});
    auto zero_bytes = valid_entry();
    zero_bytes.files[0].bytes = 0;
    reject("bytes=0", {zero_bytes});
    auto short_hash = valid_entry();
    short_hash.files[0].sha256 = "abc";
    reject("短 sha256", {short_hash});
    auto upper_hash = valid_entry();
    upper_hash.files[0].sha256 = std::string(64, 'A');
    reject("非小写 sha256", {upper_hash});
    auto no_urls = valid_entry();
    no_urls.files[0].urls.clear();
    reject("空 urls", {no_urls});
    auto ftp_url = valid_entry();
    ftp_url.files[0].urls = {"ftp://host/path"};
    reject("非 http(s) url", {ftp_url});

    AbortIfFailed(failed, "TestBundledModelManifestWellFormed");
}

void TestModelFilePresentAndVerified() {
    namespace fs = std::filesystem;
    // C5：在位判定 = 存在 + 尺寸 + SHA-256 三者齐全；同尺寸损坏/被替换文件必须判不在位。
    const auto dir = fs::temp_directory_path() / "voicestick_c5_present_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto file = dir / "sample.bin";
    const std::string hello_sha =
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";  // "hello"
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "hello";
    }

    ModelFileSpec spec;
    spec.bytes = 5;
    spec.sha256 = hello_sha;
    assert(VerifyFileSha256(file, hello_sha));                // 底层包装直接可用
    assert(ModelFilePresentAndVerified(file, spec));           // 三者齐全 → 在位

    spec.sha256 = "2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824";
    assert(ModelFilePresentAndVerified(file, spec));           // 大小写不敏感

    spec.sha256 = std::string(64, '0');                        // 同尺寸但内容不同
    assert(!ModelFilePresentAndVerified(file, spec));          // 核心回归：不得静默接受

    spec.sha256 = hello_sha;
    spec.bytes = 6;                                            // 尺寸不符
    assert(!ModelFilePresentAndVerified(file, spec));

    spec.bytes = 5;
    assert(!ModelFilePresentAndVerified(dir / "missing.bin", spec));  // 不存在
    spec.sha256 = "abc";                                             // 非法哈希长度
    assert(!ModelFilePresentAndVerified(file, spec));

    fs::remove_all(dir, ec);
}

void TestModelDownloaderPureFunctions() {
    int failed = 0;

    // ParseModelUrl。
    ParsedModelUrl parsed;
    if (!ParseModelUrl("https://modelscope.cn/models/x/resolve/master/a.gguf", parsed) ||
        parsed.host != "modelscope.cn" || !parsed.secure || parsed.port != 443 ||
        parsed.path != "/models/x/resolve/master/a.gguf") {
        std::printf("FAIL https URL 解析不符\n");
        ++failed;
    }
    if (!ParseModelUrl("http://127.0.0.1:8080/p?x=1", parsed) ||
        parsed.host != "127.0.0.1" || parsed.secure || parsed.port != 8080 ||
        parsed.path != "/p?x=1") {
        std::printf("FAIL 带端口 http URL 解析不符\n");
        ++failed;
    }
    if (ParseModelUrl("ftp://host/path", parsed) || ParseModelUrl("", parsed) ||
        ParseModelUrl("http://", parsed) || ParseModelUrl("not-a-url", parsed)) {
        std::printf("FAIL 非法 URL 应被拒\n");
        ++failed;
    }

    // PlanResume。
    if (PlanResume(0, 100) != ResumePlan::kFreshStart ||
        PlanResume(1, 100) != ResumePlan::kResume ||
        PlanResume(99, 100) != ResumePlan::kResume ||
        PlanResume(100, 100) != ResumePlan::kCorruptRestart ||
        PlanResume(101, 100) != ResumePlan::kCorruptRestart) {
        std::printf("FAIL PlanResume 边界不符\n");
        ++failed;
    }

    // InterpretRangeResponse。
    const auto verdict = [](RangeResponseInfo info) {
        return InterpretRangeResponse(info);
    };
    if (verdict({.status = 200, .has_content_length = true, .content_length = 100,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kOverwritePart) {
        std::printf("FAIL 200 全量应 OverwritePart\n");
        ++failed;
    }
    if (verdict({.status = 200, .has_content_length = true, .content_length = 99,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kInvalid) {
        std::printf("FAIL 200 且 CL 不符应 Invalid\n");
        ++failed;
    }
    if (verdict({.status = 200, .has_content_length = false,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kOverwritePart) {
        std::printf("FAIL 200 无 CL 应 OverwritePart（哈希兜底）\n");
        ++failed;
    }
    if (verdict({.status = 200, .has_content_length = false,
                 .range_base = 50, .expected_bytes = 100}) != RangeVerdict::kOverwritePart) {
        std::printf("FAIL 服务器无视 Range 应回 200 重下\n");
        ++failed;
    }
    if (verdict({.status = 206, .has_content_length = true, .content_length = 50,
                 .range_base = 50, .expected_bytes = 100}) != RangeVerdict::kAppendToPart) {
        std::printf("FAIL 206 自洽应 AppendToPart\n");
        ++failed;
    }
    if (verdict({.status = 206, .has_content_length = true, .content_length = 51,
                 .range_base = 50, .expected_bytes = 100}) != RangeVerdict::kInvalid) {
        std::printf("FAIL 206 CL 与基数不符应 Invalid\n");
        ++failed;
    }
    if (verdict({.status = 206, .has_content_length = true, .content_length = 100,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kInvalid) {
        std::printf("FAIL 未带 Range 却回 206 应 Invalid\n");
        ++failed;
    }
    if (verdict({.status = 206, .has_content_length = false,
                 .range_base = 50, .expected_bytes = 100}) != RangeVerdict::kAppendToPart) {
        std::printf("FAIL 206 无 CL 应 AppendToPart（信任连接边界）\n");
        ++failed;
    }
    if (verdict({.status = 404, .has_content_length = false,
                 .range_base = 0, .expected_bytes = 100}) != RangeVerdict::kInvalid) {
        std::printf("FAIL 非 200/206 应 Invalid（防御）\n");
        ++failed;
    }

    // RequiredDiskBytes。
    if (RequiredDiskBytes({}) != 0) {
        std::printf("FAIL 空清单磁盘需求应为 0\n");
        ++failed;
    }
    {
        std::vector<ModelFileSpec> files;
        ModelFileSpec a;
        a.bytes = 100;
        ModelFileSpec b;
        b.bytes = 23;
        files = {a, b};
        if (RequiredDiskBytes(files) != 123) {
            std::printf("FAIL 磁盘需求应求和\n");
            ++failed;
        }
    }

    AbortIfFailed(failed, "TestModelDownloaderPureFunctions");
}

void TestFinalizePartFile() {
    int failed = 0;
    const auto dir = MakeTempDir("finalize");
    const auto dest = dir / "model.bin";
    const auto part = dir / "model.bin.part";

    // 正常收尾：哈希匹配 → 原子改名。
    WriteFileBytes(part, "hello world");
    if (FinalizePartFile(dest, TestSha256Hex("hello world")) != DownloadResult::kOk ||
        ReadFileBytes(dest) != "hello world" || std::filesystem::exists(part)) {
        std::printf("FAIL 哈希匹配应改名成功且 .part 消失\n");
        ++failed;
    }

    // 哈希不匹配：删除 .part，dest 保持原样。
    WriteFileBytes(part, "tampered");
    if (FinalizePartFile(dest, TestSha256Hex("hello world")) != DownloadResult::kHashMismatch ||
        std::filesystem::exists(part) || ReadFileBytes(dest) != "hello world") {
        std::printf("FAIL 哈希不匹配应删 .part 且不动 dest\n");
        ++failed;
    }

    // .part 不存在（调用方违约）：按不匹配处理。
    if (FinalizePartFile(dest, TestSha256Hex("hello world")) != DownloadResult::kHashMismatch) {
        std::printf("FAIL 缺 .part 应按不匹配处理\n");
        ++failed;
    }

    // 大写期望哈希：实现侧归一化小写后比较。
    WriteFileBytes(part, "hello world");
    std::string upper = TestSha256Hex("hello world");
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const auto second = dir / "second.bin";
    const auto second_part = dir / "second.bin.part";
    WriteFileBytes(second_part, "hello world");
    if (FinalizePartFile(second, upper) != DownloadResult::kOk ||
        ReadFileBytes(second) != "hello world") {
        std::printf("FAIL 大写期望哈希应归一化比较\n");
        ++failed;
    }

    AbortIfFailed(failed, "TestFinalizePartFile");
}

void TestModelDownloaderLoopback() {
    int failed = 0;
    const std::string kBody = "hello world";

    LoopbackHttpServer::Rules rules;
    rules["/ok"] = {.status = 200, .body = kBody};
    rules["/bad"] = {.status = 200, .body = "hello worlx"};  // 同长度坏内容
    rules["/norange"] = {.status = 200, .body = kBody, .honor_range = false};
    rules["/slow"] = {.status = 200, .body = std::string(8192, 'x'),
                      .chunk_size = 256, .chunk_delay_ms = 50};
    LoopbackHttpServer server(std::move(rules));
    ModelDownloader downloader;

    // 用例 1：全量下载成功，progress 收尾必达 total。
    {
        const auto dir = MakeTempDir("full");
        const auto dest = dir / "file.bin";
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/ok")});
        DownloadProgress last;
        int calls = 0;
        auto outcome = downloader.DownloadFile(
            spec, dest, [&](const DownloadProgress& p) { last = p; ++calls; });
        if (outcome.result != DownloadResult::kOk || !outcome.error.empty() ||
            ReadFileBytes(dest) != kBody ||
            last.downloaded != kBody.size() || last.total != kBody.size() || calls < 1) {
            std::printf("FAIL 全量下载: result=%d url_used=%s\n",
                        static_cast<int>(outcome.result), outcome.url_used.c_str());
            ++failed;
        }
    }

    // 用例 2：断点续传——预置半截 .part，服务器须收到 "bytes=5-"。
    {
        const auto dir = MakeTempDir("resume");
        const auto dest = dir / "file.bin";
        const auto part = dir / "file.bin.part";
        WriteFileBytes(part, kBody.substr(0, 5));
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/ok")});
        auto outcome = downloader.DownloadFile(spec, dest);
        const auto ranges = server.TakeRangeHeaders();
        bool saw_range = false;
        for (const auto& range : ranges) {
            if (range.find("bytes=5-") != std::string::npos) saw_range = true;
        }
        if (outcome.result != DownloadResult::kOk || ReadFileBytes(dest) != kBody ||
            !saw_range) {
            std::printf("FAIL 续传: result=%d range_seen=%d\n",
                        static_cast<int>(outcome.result), saw_range ? 1 : 0);
            ++failed;
        }
    }

    // 用例 3：服务器无视 Range 回 200 全量 → 重下成功（OverwritePart 路径）。
    {
        const auto dir = MakeTempDir("norange");
        const auto dest = dir / "file.bin";
        const auto part = dir / "file.bin.part";
        WriteFileBytes(part, kBody.substr(0, 5));
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/norange")});
        auto outcome = downloader.DownloadFile(spec, dest);
        if (outcome.result != DownloadResult::kOk || ReadFileBytes(dest) != kBody) {
            std::printf("FAIL 服务器无视 Range: result=%d\n",
                        static_cast<int>(outcome.result));
            ++failed;
        }
        server.TakeRangeHeaders();  // 清空本用例记录
    }

    // 用例 4：首源哈希不匹配 → 回退次源成功。
    {
        const auto dir = MakeTempDir("fallback");
        const auto dest = dir / "file.bin";
        const auto spec =
            MakeSpecFromBody(kBody, {server.Url("/bad"), server.Url("/ok")});
        auto outcome = downloader.DownloadFile(spec, dest);
        if (outcome.result != DownloadResult::kOk ||
            outcome.url_used != server.Url("/ok") || ReadFileBytes(dest) != kBody) {
            std::printf("FAIL 哈希不匹配换源: result=%d url=%s\n",
                        static_cast<int>(outcome.result), outcome.url_used.c_str());
            ++failed;
        }
    }

    // 用例 5：单源坏内容 → kHashMismatch，.part 已删、dest 不存在。
    {
        const auto dir = MakeTempDir("mismatch");
        const auto dest = dir / "file.bin";
        const auto part = dir / "file.bin.part";
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/bad")});
        auto outcome = downloader.DownloadFile(spec, dest);
        if (outcome.result != DownloadResult::kHashMismatch ||
            std::filesystem::exists(part) || std::filesystem::exists(dest)) {
            std::printf("FAIL 单源哈希不匹配: result=%d\n",
                        static_cast<int>(outcome.result));
            ++failed;
        }
    }

    // 用例 6：404 → 换源成功；全 404 → kNetworkError 且 error 汇总含状态码。
    {
        const auto dir = MakeTempDir("http404");
        const auto dest = dir / "file.bin";
        const auto spec =
            MakeSpecFromBody(kBody, {server.Url("/missing"), server.Url("/ok")});
        auto outcome = downloader.DownloadFile(spec, dest);
        if (outcome.result != DownloadResult::kOk) {
            std::printf("FAIL 404 换源: result=%d\n", static_cast<int>(outcome.result));
            ++failed;
        }
        const auto spec_all_missing = MakeSpecFromBody(kBody, {server.Url("/missing")});
        auto outcome2 = downloader.DownloadFile(spec_all_missing, dest);
        if (outcome2.result != DownloadResult::kNetworkError ||
            outcome2.error.find("404") == std::string::npos) {
            std::printf("FAIL 全 404: result=%d error=%s\n",
                        static_cast<int>(outcome2.result), outcome2.error.c_str());
            ++failed;
        }
    }

    // 用例 7：取消——慢速分块下载，收到首批进度后置位，.part 保留。
    {
        const auto dir = MakeTempDir("cancel");
        const auto dest = dir / "file.bin";
        const auto part = dir / "file.bin.part";
        const auto spec = MakeSpecFromBody(std::string(8192, 'x'), {server.Url("/slow")});
        std::atomic<bool> cancel_flag{false};
        auto outcome = downloader.DownloadFile(
            spec, dest,
            [&](const DownloadProgress& p) {
                if (p.downloaded > 0) cancel_flag.store(true);
            },
            [&] { return cancel_flag.load(); });
        const bool part_kept = std::filesystem::exists(part) &&
                               std::filesystem::file_size(part) < 8192;
        if (outcome.result != DownloadResult::kCancelled || !part_kept ||
            std::filesystem::exists(dest)) {
            std::printf("FAIL 取消: result=%d part_kept=%d\n",
                        static_cast<int>(outcome.result), part_kept ? 1 : 0);
            ++failed;
        }
    }

    // 用例 8：无效清单（urls 空）→ kInvalidSpec。
    {
        const auto dir = MakeTempDir("invalid");
        auto outcome = downloader.DownloadFile(MakeSpecFromBody(kBody, {}),
                                               dir / "file.bin");
        if (outcome.result != DownloadResult::kInvalidSpec) {
            std::printf("FAIL 空 urls 应 kInvalidSpec: result=%d\n",
                        static_cast<int>(outcome.result));
            ++failed;
        }
    }

    // 用例 9：dest 父目录不存在 → kLocalIoError（本地 I/O，与网络无关）。
    {
        const auto dir = MakeTempDir("baddir");
        const auto spec = MakeSpecFromBody(kBody, {server.Url("/ok")});
        auto outcome = downloader.DownloadFile(spec, dir / "no-such-dir" / "file.bin");
        if (outcome.result != DownloadResult::kLocalIoError) {
            std::printf("FAIL 父目录缺失应 kLocalIoError: result=%d\n",
                        static_cast<int>(outcome.result));
            ++failed;
        }
    }

    AbortIfFailed(failed, "TestModelDownloaderLoopback");
}

void TestModelDownloadSession() {
    int failed = 0;
    const std::string kAsr1(4096, 'a');
    const std::string kAsr2(2048, 'b');
    const std::string kRefine(8192, 'c');

    LoopbackHttpServer::Rules rules;
    rules["/asr1"] = {.status = 200, .body = kAsr1};
    rules["/asr2"] = {.status = 200, .body = kAsr2};
    rules["/refine"] = {.status = 200, .body = kRefine};
    rules["/slow"] = {.status = 200, .body = std::string(8192, 's'),
                      .chunk_size = 256, .chunk_delay_ms = 50};
    LoopbackHttpServer server(std::move(rules));
    ModelDownloader downloader;

    const auto make_item = [&](ModelKind kind, const std::string& url,
                               const std::string& body,
                               const std::filesystem::path& dest) {
        ModelDownloadItem item;
        item.kind = kind;
        item.selected = true;
        item.spec = MakeSpecFromBody(body, {url});
        item.dest = dest;
        return item;
    };

    // 用例 1：全成功——进度聚合终值、三个文件落位、summary 全绿。
    {
        const auto dir = MakeTempDir("session_ok");
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/asr1"), kAsr1, dir / "model.int8.onnx"),
            make_item(ModelKind::kAsr, server.Url("/asr2"), kAsr2, dir / "tokens.txt"),
            make_item(ModelKind::kRefine, server.Url("/refine"), kRefine,
                      dir / "Qwen3-1.7B-Q4_K_M" / "Qwen3-1.7B-Q4_K_M.gguf"),
        };
        ModelSessionProgress last;
        ModelDownloadSession session(std::move(items), &downloader,
                                     [&](const ModelSessionProgress& p) { last = p; });
        const auto summary = session.Run();
        const std::uint64_t total = kAsr1.size() + kAsr2.size() + kRefine.size();
        if (!summary.asr_ok || !summary.refine_ok || summary.refine_skipped ||
            summary.cancelled || !summary.errors.empty()) {
            std::printf("FAIL 全成功 summary: asr=%d refine=%d skip=%d cancel=%d errs=%zu\n",
                        summary.asr_ok ? 1 : 0, summary.refine_ok ? 1 : 0,
                        summary.refine_skipped ? 1 : 0, summary.cancelled ? 1 : 0,
                        summary.errors.size());
            ++failed;
        }
        if (last.total != total || last.downloaded != total) {
            std::printf("FAIL 进度聚合终值: %llu/%llu（期望 %llu）\n",
                        static_cast<unsigned long long>(last.downloaded),
                        static_cast<unsigned long long>(last.total),
                        static_cast<unsigned long long>(total));
            ++failed;
        }
        if (ReadFileBytes(dir / "model.int8.onnx") != kAsr1 ||
            ReadFileBytes(dir / "tokens.txt") != kAsr2 ||
            ReadFileBytes(dir / "Qwen3-1.7B-Q4_K_M" / "Qwen3-1.7B-Q4_K_M.gguf") != kRefine) {
            std::printf("FAIL 三个目标文件内容不符\n");
            ++failed;
        }
    }

    // 用例 2：精修源全 404——ASR 仍成功，精修失败记录一条，不阻塞收尾。
    {
        const auto dir = MakeTempDir("session_refine_fail");
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/asr1"), kAsr1, dir / "model.int8.onnx"),
            make_item(ModelKind::kAsr, server.Url("/asr2"), kAsr2, dir / "tokens.txt"),
            make_item(ModelKind::kRefine, server.Url("/missing"), kRefine, dir / "refine.gguf"),
        };
        ModelDownloadSession session(std::move(items), &downloader);
        const auto summary = session.Run();
        if (!summary.asr_ok || summary.refine_ok || summary.refine_skipped ||
            summary.cancelled || summary.errors.size() != 1) {
            std::printf("FAIL 精修失败语义: asr=%d refine=%d errs=%zu\n",
                        summary.asr_ok ? 1 : 0, summary.refine_ok ? 1 : 0,
                        summary.errors.size());
            ++failed;
        }
    }

    // 用例 3：ASR 首文件 404——整体失败，后续条目（含精修）不再发起。
    {
        const auto dir = MakeTempDir("session_asr_fail");
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/missing"), kAsr1, dir / "model.int8.onnx"),
            make_item(ModelKind::kAsr, server.Url("/asr2"), kAsr2, dir / "tokens.txt"),
            make_item(ModelKind::kRefine, server.Url("/refine"), kRefine, dir / "refine.gguf"),
        };
        ModelDownloadSession session(std::move(items), &downloader);
        const auto summary = session.Run();
        if (summary.asr_ok || summary.refine_ok || summary.cancelled ||
            std::filesystem::exists(dir / "tokens.txt") ||
            std::filesystem::exists(dir / "refine.gguf")) {
            std::printf("FAIL ASR 失败应中断: asr=%d 后续文件不应存在\n",
                        summary.asr_ok ? 1 : 0);
            ++failed;
        }
    }

    // 用例 4：取消——慢速源中途置位，cancelled 且 .part 保留。
    {
        const auto dir = MakeTempDir("session_cancel");
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/slow"), std::string(8192, 's'),
                      dir / "model.int8.onnx"),
        };
        auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
        ModelDownloadSession session(std::move(items), &downloader,
                                     [&](const ModelSessionProgress& p) {
                                         if (p.downloaded > 0) cancel_flag->store(true);
                                     },
                                     cancel_flag);
        const auto summary = session.Run();
        const bool part_kept = std::filesystem::exists(dir / "model.int8.onnx.part") &&
                               std::filesystem::file_size(dir / "model.int8.onnx.part") < 8192;
        if (!summary.cancelled || summary.asr_ok || !part_kept) {
            std::printf("FAIL 取消语义: cancel=%d asr=%d part_kept=%d\n",
                        summary.cancelled ? 1 : 0, summary.asr_ok ? 1 : 0, part_kept ? 1 : 0);
            ++failed;
        }
    }

    // 用例 5：精修未勾选——不发起该条目，refine_skipped 且无错误。
    {
        const auto dir = MakeTempDir("session_skip_refine");
        auto refine_item = make_item(ModelKind::kRefine, server.Url("/refine"), kRefine,
                                     dir / "refine.gguf");
        refine_item.selected = false;
        std::vector<ModelDownloadItem> items = {
            make_item(ModelKind::kAsr, server.Url("/asr1"), kAsr1, dir / "model.int8.onnx"),
            make_item(ModelKind::kAsr, server.Url("/asr2"), kAsr2, dir / "tokens.txt"),
            std::move(refine_item),
        };
        ModelDownloadSession session(std::move(items), &downloader);
        const auto summary = session.Run();
        if (!summary.asr_ok || summary.refine_ok || !summary.refine_skipped ||
            !summary.errors.empty() || std::filesystem::exists(dir / "refine.gguf")) {
            std::printf("FAIL 跳过精修语义: asr=%d skip=%d errs=%zu\n",
                        summary.asr_ok ? 1 : 0, summary.refine_skipped ? 1 : 0,
                        summary.errors.size());
            ++failed;
        }
    }

    // 用例 6：BuildModelDownloadItems——条目数与目标路径（含 GGUF 子目录）。
    {
        const auto models_dir = std::filesystem::path("C:/cache/models");
        const auto with_refine = BuildModelDownloadItems(models_dir, true);
        if (with_refine.size() != 3 ||
            with_refine[0].dest != models_dir / "model.int8.onnx" ||
            with_refine[1].dest != models_dir / "tokens.txt" ||
            with_refine[2].dest != models_dir / "Qwen3-1.7B-Q4_K_M" / "Qwen3-1.7B-Q4_K_M.gguf" ||
            with_refine[0].kind != ModelKind::kAsr ||
            with_refine[2].kind != ModelKind::kRefine || !with_refine[2].selected) {
            std::printf("FAIL BuildModelDownloadItems(含精修) 条目不符：%zu 条\n",
                        with_refine.size());
            ++failed;
        }
        const auto without_refine = BuildModelDownloadItems(models_dir, false);
        if (without_refine.size() != 2 ||
            without_refine[0].spec.rel_path != "model.int8.onnx" ||
            without_refine[1].spec.bytes != 315894) {
            std::printf("FAIL BuildModelDownloadItems(不含精修) 条目不符：%zu 条\n",
                        without_refine.size());
            ++failed;
        }
        // spec 与内置清单同源（哈希/URL 不漂移）。
        const auto& bundled = BundledModelEntries();
        if (with_refine.size() == 3 &&
            (with_refine[0].spec.sha256 != bundled[0].files[0].sha256 ||
             with_refine[0].spec.urls != bundled[0].files[0].urls)) {
            std::printf("FAIL 条目 spec 应与内置清单一致\n");
            ++failed;
        }
    }

    // 用例 7：LocalModelCacheModelsDir——非空且尾部三段目录名固定。
    {
        const auto dir = LocalModelCacheModelsDir();
        if (dir.empty() || dir.filename().wstring() != L"sense-voice-int8-2024-07-17" ||
            dir.parent_path().filename().wstring() != L"models" ||
            dir.parent_path().parent_path().filename().wstring() != L"VoiceStick") {
            std::printf("FAIL LocalModelCacheModelsDir 尾部结构不符\n");
            ++failed;
        }
    }

    AbortIfFailed(failed, "TestModelDownloadSession");
}

// B8：UpdateConfig 原子换入与后台读取并发（快照语义压力冒烟）——4 读线程持续走
// 纯配置读路径（WechatSessionUsesDefaultMicDirectly：快照 + firmware_mutex_），
// 主线程 300 次整份换入；无撕裂读、无死锁、读线程全程存活。
void TestCoordinatorConcurrentUpdateConfigStress() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto cloud_asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble),
                                      std::move(cloud_asr), &ui, &input);
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                (void)coordinator.WechatSessionUsesDefaultMicDirectly("RC-0001");
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int i = 0; i < 300; ++i) {
        AppConfig next = AppConfig::Defaults();
        next.ui_language = (i % 2 == 0) ? UiLanguage::kSimplifiedChinese
                                       : UiLanguage::kEnglish;
        coordinator.UpdateConfig(std::move(next));
    }
    stop.store(true);
    for (auto& thread : readers) {
        thread.join();
    }
    int failed = reads.load() > 0 ? 0 : 1;
    if (failed != 0) {
        std::printf("FAIL contract-ish: 读线程零读取（未真正压到快照路径）\n");
        fflush(stdout);
    }
    AbortIfFailed(failed, "TestCoordinatorConcurrentUpdateConfigStress");
}

// B9：任何保存路径都不抹磁盘 [license]（陈旧/空内存副本），且原子写成功不留 .tmp 残迹。
void TestSaveStaleCopyKeepsLicense() {
    namespace fs = std::filesystem;
    int failed = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            ++failed;
            std::printf("FAIL contract-ish: %s\n", what);
            fflush(stdout);
        }
    };
    const auto dir = fs::temp_directory_path() / "vs_config_b9";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const auto path = dir / "config.toml";

    // 1) 建立带 license 的磁盘配置。
    AppConfig on_disk = AppConfig::Defaults();
    on_disk.license.serial = "VS-B9-SERIAL";
    on_disk.license.trial_anchor_days = 100;
    on_disk.license.last_seen_days = 105;
    on_disk.volcengine_api_key = "disk-key";
    on_disk.Save(path);

    // 2) 陈旧副本（license 为空 = 运行期快照）走两条合并保存路径，[license] 必须幸存。
    // plain Save 写**本对象** license（LicenseRuntime 激活/锚点落盘依赖该语义，见
    // TestLicenseConfigRoundTrip 往返），陈旧持有者约定不走 plain Save——B9 定案：
    // 谁的副本谁重取，合并路径自带磁盘 license 保留。
    AppConfig stale = AppConfig::Defaults();
    stale.SavePreservingDiskCredentials(path);
    AppConfig loaded = AppConfig::Load(path);
    expect(loaded.license.serial == "VS-B9-SERIAL",
           "SavePreservingDiskCredentials 后 license.serial 被抹");
    expect(loaded.license.trial_anchor_days.has_value() &&
               *loaded.license.trial_anchor_days == 100,
           "SavePreservingDiskCredentials 后 trial_anchor_days 被抹");
    expect(loaded.license.last_seen_days.has_value() &&
               *loaded.license.last_seen_days == 105,
           "SavePreservingDiskCredentials 后 last_seen_days 被抹");
    expect(loaded.volcengine_api_key == "disk-key",
           "SavePreservingDiskCredentials 凭据保留语义回归");

    stale.SaveSettingsDialog(path);
    loaded = AppConfig::Load(path);
    expect(loaded.license.serial == "VS-B9-SERIAL",
           "SaveSettingsDialog 后 license.serial 被抹");

    // 3) 原子写：成功路径不留 .tmp 残迹，文件完整可回读。
    expect(!fs::exists(fs::path(path.string() + ".tmp")), "成功保存后残留 .tmp");
    expect(loaded.asr_provider == AppConfig::Defaults().asr_provider, "回读内容完整");

    fs::remove_all(dir, ec);
    AbortIfFailed(failed, "TestSaveStaleCopyKeepsLicense");
}







void TestContractFixtures() {
    int failed = 0;
    auto fail = [&](const std::string& msg) {
        ++failed;
        std::printf("FAIL contract: %s\n", msg.c_str());
        fflush(stdout);
    };
#ifdef VOICESTICK_REPO_ROOT
    const std::string path = (std::filesystem::path(VOICESTICK_REPO_ROOT) /
                              "tests/contract/fixtures/manifest.json").string();
#else
    const std::string path = "tests/contract/fixtures/manifest.json";
#endif
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail("manifest 打不开: " + path);
        AbortIfFailed(failed, "TestContractFixtures");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    cJSON* manifest = cJSON_Parse(text.c_str());
    if (manifest == nullptr) {
        fail("manifest JSON 解析失败: " + path);
        AbortIfFailed(failed, "TestContractFixtures");
        return;
    }
    auto section = [&](const char* key) -> cJSON* {
        return cJSON_GetObjectItemCaseSensitive(manifest, key);
    };

    // 1) state 事件：黄金字节 → ParseStateEvent → 公共字段期望（线上字段名）。
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, section("state_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const auto bytes = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const auto event = BleProtocol::ParseStateEvent(bytes);
        if (!event.has_value()) {
            fail(name + ": ParseStateEvent 返回空");
            continue;
        }
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        const cJSON* field = nullptr;
        cJSON_ArrayForEach(field, expect) {
            const std::string key = field->string ? field->string : "";
            bool known = true;
            bool ok = false;
            if (key == "event") {
                ok = event->event == ContractJsonStr(field);
            } else if (key == "button") {
                ok = event->button == ContractJsonStr(field);
            } else if (key == "hardware") {
                ok = event->hardware == ContractJsonStr(field);
            } else if (key == "firmware_version") {
                ok = event->firmware_version == ContractJsonStr(field);
            } else if (key == "direction") {
                ok = event->direction == ContractJsonStr(field);
            } else if (key == "source") {
                ok = event->source == ContractJsonStr(field);
            } else if (key == "key") {
                ok = event->gateway_key == ContractJsonStr(field);
            } else if (key == "session_id") {
                ok = event->session_id.has_value() &&
                     *event->session_id == ContractJsonU32(field);
            } else if (key == "duration_ms") {
                ok = event->duration_ms.has_value() &&
                     *event->duration_ms == ContractJsonU32(field);
            } else if (key == "steps") {
                ok = event->steps.has_value() && *event->steps == ContractJsonU32(field);
            } else if (key == "level") {
                ok = event->battery_level.has_value() &&
                     *event->battery_level == field->valueint;
            } else if (key == "present") {
                ok = event->encoder_present.has_value() &&
                     *event->encoder_present == cJSON_IsTrue(field);
            } else if (key == "charging") {
                ok = event->battery_charging.has_value() &&
                     *event->battery_charging == cJSON_IsTrue(field);
            } else if (key == "usb_powered") {
                ok = event->battery_usb_powered.has_value() &&
                     *event->battery_usb_powered == cJSON_IsTrue(field);
            } else if (key == "pressed") {
                ok = event->gateway_pressed.has_value() &&
                     *event->gateway_pressed == cJSON_IsTrue(field);
            } else if (key == "mode") {
                // gateway_status.mode 线上值 "gateway"/"normal" → 本端 bool。
                ok = event->gateway_mode.has_value() &&
                     *event->gateway_mode == (ContractJsonStr(field) == "gateway");
            } else {
                known = false;
            }
            if (!known) {
                fail(name + ": 未映射的 expect 键 " + key);
            } else if (!ok) {
                fail(name + ": 字段 " + key + " 不符");
            }
        }
    }

    // 2) power_mgmt：独立解析器（ParseStateEvent 对其返回空）。
    cJSON_ArrayForEach(item, section("power_mgmt_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const auto bytes = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const auto pm = BleProtocol::ParsePowerMgmtEvent(bytes);
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        const cJSON* want = cJSON_GetObjectItemCaseSensitive(expect, "usb_auto_off");
        if (!pm.has_value() || want == nullptr) {
            fail(name + ": ParsePowerMgmtEvent/expect 失败");
            continue;
        }
        if (*pm != cJSON_IsTrue(want)) {
            fail(name + ": usb_auto_off 不符");
        }
        // ParseStateEvent 必须跳过 power_mgmt（分发链契约）。
        if (BleProtocol::ParseStateEvent(bytes).has_value()) {
            fail(name + ": ParseStateEvent 应跳过 power_mgmt");
        }
    }

    // 3) OTA state 五态。
    cJSON_ArrayForEach(item, section("ota_state_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const auto bytes = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const auto ota = BleProtocol::ParseFirmwareOtaStateEvent(bytes);
        if (!ota.has_value()) {
            fail(name + ": ParseFirmwareOtaStateEvent 返回空");
            continue;
        }
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        const cJSON* field = nullptr;
        cJSON_ArrayForEach(field, expect) {
            const std::string key = field->string ? field->string : "";
            bool ok = false;
            if (key == "event") {
                ok = ota->event == ContractJsonStr(field);
            } else if (key == "code") {
                ok = ota->code == ContractJsonStr(field);
            } else if (key == "transfer_id") {
                ok = ota->transfer_id.has_value() &&
                     *ota->transfer_id == ContractJsonU32(field);
            } else if (key == "written") {
                ok = ota->written.has_value() && *ota->written == ContractJsonU32(field);
            } else if (key == "size") {
                ok = ota->size.has_value() && *ota->size == ContractJsonU32(field);
            } else if (key == "esp_err") {
                ok = ota->esp_err.has_value() && *ota->esp_err == ContractJsonU32(field);
            } else if (key == "reboot_ms") {
                ok = ota->reboot_ms.has_value() && *ota->reboot_ms == ContractJsonU32(field);
            } else {
                fail(name + ": 未映射的 expect 键 " + key);
                continue;
            }
            if (!ok) fail(name + ": 字段 " + key + " 不符");
        }
    }

    // 4) 二进制 audio / motion。
    cJSON_ArrayForEach(item, section("binary_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const std::string kind = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "kind"));
        const auto bytes = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        if (kind == "audio") {
            const auto audio = BleProtocol::ParseAudioFrame(bytes);
            if (!audio.has_value()) {
                fail(name + ": ParseAudioFrame 返回空");
                continue;
            }
            const cJSON* want_session =
                cJSON_GetObjectItemCaseSensitive(expect, "session_id");
            const cJSON* want_seq = cJSON_GetObjectItemCaseSensitive(expect, "seq");
            const cJSON* want_flags = cJSON_GetObjectItemCaseSensitive(expect, "flags");
            const cJSON* want_payload =
                cJSON_GetObjectItemCaseSensitive(expect, "payload_hex");
            if (want_session && audio->session_id != ContractJsonU32(want_session)) {
                fail(name + ": session_id 不符");
            }
            if (want_seq && audio->seq != ContractJsonU32(want_seq)) {
                fail(name + ": seq 不符");
            }
            if (want_flags &&
                audio->flags != static_cast<std::uint8_t>(want_flags->valueint)) {
                fail(name + ": flags 不符");
            }
            if (want_payload &&
                ContractHexStr(audio->payload) != ContractJsonStr(want_payload)) {
                fail(name + ": payload 不符 got=" + ContractHexStr(audio->payload));
            }
            // 帧头 flags 语义位（spec：bit0=start bit1=end）。
            const cJSON* want_u32 = want_flags;
            if (want_u32 && (ContractJsonU32(want_u32) & 0x01) && !audio->IsStart()) {
                fail(name + ": IsStart() 应为真");
            }
            if (want_u32 && (ContractJsonU32(want_u32) & 0x02) && !audio->IsEnd()) {
                fail(name + ": IsEnd() 应为真");
            }
        } else if (kind == "motion") {
            const auto motion = BleProtocol::ParseMotionFrame(bytes);
            if (!motion.has_value()) {
                fail(name + ": ParseMotionFrame 返回空");
                continue;
            }
            const cJSON* want_dx = cJSON_GetObjectItemCaseSensitive(expect, "dx");
            const cJSON* want_dy = cJSON_GetObjectItemCaseSensitive(expect, "dy");
            if (want_dx &&
                motion->dx != static_cast<std::int16_t>(want_dx->valueint)) {
                fail(name + ": dx 不符");
            }
            if (want_dy &&
                motion->dy != static_cast<std::int16_t>(want_dy->valueint)) {
                fail(name + ": dy 不符");
            }
        } else {
            fail(name + ": 未知 binary kind " + kind);
        }
    }

    // 5) control 构建 → 对象语义比对（键序无关，cJSON_Compare 大小写敏感）。
    cJSON_ArrayForEach(item, section("control_payloads")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const std::string kind = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "kind"));
        const cJSON* args = cJSON_GetObjectItemCaseSensitive(item, "args");
        const cJSON* expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        const auto built = ContractBuildControl(kind, args);
        if (!built.has_value()) {
            fail(name + ": 构建器缺失/构建失败 (kind=" + kind + ")");
            continue;
        }
        const std::string built_text(built->begin(), built->end());
        cJSON* actual = cJSON_Parse(built_text.c_str());
        if (actual == nullptr) {
            fail(name + ": 构建输出不是合法 JSON: " + built_text);
            continue;
        }
        if (!cJSON_Compare(actual, expect, /*case_sensitive=*/TRUE)) {
            fail(name + ": 构建输出与期望不符 got=" + built_text);
        }
        cJSON_Delete(actual);
    }

    // 6) OTA 控制二进制帧：整帧字节相等。
    cJSON_ArrayForEach(item, section("ota_control_frames")) {
        const std::string name = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "name"));
        const std::string kind = ContractJsonStr(
            cJSON_GetObjectItemCaseSensitive(item, "kind"));
        const cJSON* args = cJSON_GetObjectItemCaseSensitive(item, "args");
        const auto want = ContractUnhex(
            ContractJsonStr(cJSON_GetObjectItemCaseSensitive(item, "hex")));
        const auto built = ContractBuildOtaControl(kind, args);
        if (!built.has_value()) {
            fail(name + ": 构建器缺失/构建失败 (kind=" + kind + ")");
            continue;
        }
        if (*built != want) {
            fail(name + ": 字节不符 got=" + ContractHexStr(*built) +
                 " want=" + ContractHexStr(want));
        }
    }

    cJSON_Delete(manifest);
    AbortIfFailed(failed, "TestContractFixtures");
}

// CI 诊断（2026-10-07）：进程级未处理异常探针——任何线程的硬异常（AV/栈溢出/
// fail-fast）在默认终止前把错误码与地址打进日志，用于 ctest SEGFAULT 定位。
//（曾试 AddVectoredExceptionHandlerFirst：SDK 头未声明、且 kernel32 导入库无此符号，
// 改用声明与导出俱在的 SetUnhandledExceptionFilter。）

// Suite entry: core_tests.cc main() calls this once.
void RunLicenseImaAtvvBatchTests() {
    TestLicenseVerifySerial();
    TestC8LogUrlPromptHygiene();
    TestSecureCloudUrlPolicy();
    TestLicenseBindingDevicesUnion();
    TestLicenseDateToDaysPreEpoch();
    TestLicenseStatus();
    TestLicenseConfigRoundTrip();
    TestSavePairedDeviceInfoPreservesDiskLicense();
    TestVolcengineTableIdConfigRoundTrip();
    TestAppConfig();
    TestLlmRefinePromptAndPayload();
    TestHotwordProcessConfig();
    TestHotwordExtractorPromptAndParse();
    TestHotwordCandidateMiner();
    TestHotwordCandidatesSingleWriter();
    TestHotwordValidationUnified();
    TestHotwordSelector();
    TestHotwordExtractionPromptAndParse();
    TestFirmwareManifestParsingAndVersionCompare();
    TestFirmwareVersionC6Robustness();
    TestFirmwareManifestMinimumVersion();
    TestFirmwareManifestFallbackUrls();
    TestCoordinatorSyncsImuWakeSensitivityOnConnectionAndConfigUpdate();
    TestCoordinatorUpdateFirmwareFromFile();
    TestOtaMaxInFlightBytes();
    TestOtaChunkSizeForPdu();
    TestParseOtaCliArgs();
    TestCoordinatorSyncsTapSensitivityOnConnectionAndConfigUpdate();
    TestBleEncoderPayloads();
    TestCoordinatorPushesGatewayKeymapRoutes();
    TestCoordinatorSyncsEncoderSettingsOnConnectionAndConfigUpdate();
    TestCoordinatorSyncsEncoderSettingsPerDeviceOverride();
    TestCoordinatorSyncsInteractionSettingsPerDeviceOverride();
    TestAppConfigTapSensitivityRoundTrip();
    TestCoordinatorHotkeyWithoutConnectionShowsWakeHint();
    TestCoordinatorHotkeyWithConnectionSendsRemoteButton();
    TestWechatStartFailureRollsBackDefaultCapture();
    TestXiaomiAtvvCapsParsing();
    TestImaAdpcmDecoderGolden();
    TestFlashToolFlow();
    TestPowerLogMonitor();
    TestTextRefinerRules();
    TestRefineGuardSafety();
    TestLocalRefinementClientOrchestration();
    TestLocalRefinementCustomPrompt();
    TestLocalRefinementDiagnosticsLogs();
    TestLocalRefinementCrossTurnOrchestration();
    TestLocalRefinementCrossTurnDiagnosticsLogs();
    TestPinyinSameOrNear();
    TestApplyPinyinCorrections();
    TestSelectionCorrection();
    TestLocalRefinementGenerateCandidates();
    TestRefineHistory();
    TestLlamaCppEngineRealModelSmoke();
    TestLocalAsrClientStartFailsWhenModelMissing();
    TestResolveLocalRefineModelPathCrossTurn();
    TestLlamaCppEngineSessionTurnSmoke();
    TestLocalAsrClientSenseVoiceSmoke();
    TestLocalAsrClientEmitsPartialWhileStreaming();
    TestLocalAsrClientPartialThrottled();
    TestLocalAsrClientFinalReusesDecodeWhenNoNewAudio();
    TestLocalAsrClientSkipsEmptyPartial();
    TestLocalAsrClientCancelStopsPartial();
    TestPushToTalkKeyParsing();
    TestFormatPushToTalkKey();
    TestProviderComboMapping();
    TestResolveAndValidateModelsDir();
    TestShortcutCaptureClassifyKey();
    TestShortcutCapturePollEligibleVk();
    TestAppConfigLocalAsrRoundTrip();
    TestWasapiMicCaptureSmoke();
    TestClipboardVaultMultiFormatRoundTrip();
    TestClipboardVaultSkipsHandleFormats();
    TestClipboardVaultEmptyClipboardSnapshot();
    TestClipboardVaultSaveThrowsWhenBusy();
    TestBundledModelManifestWellFormed();
    TestModelFilePresentAndVerified();
    TestModelDownloaderPureFunctions();
    TestFinalizePartFile();
    TestModelDownloaderLoopback();
    TestModelDownloadSession();
    TestCoordinatorConcurrentUpdateConfigStress();
    TestSaveStaleCopyKeepsLicense();
    TestContractFixtures();
}

