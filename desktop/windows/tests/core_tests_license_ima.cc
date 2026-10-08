// N8 cut14: license/ima/asr-refiner/caps batch extracted from core_tests.cc
// (38 tests). Pre-moved: ImaGoldenVector + ImaEncodeForTest -> test_support.h;
// orphan registration (def never called) fixed in core main before this cut.
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

// ---- 电池电压监测：power_log 解析与增量累积 ----

std::vector<std::uint8_t> BuildPowerLogEntry(std::uint32_t uptime_s, std::uint16_t vbat_mv,
                                             std::uint8_t mode, std::uint8_t flags,
                                             std::uint32_t reserved = 0) {
    return {
        static_cast<std::uint8_t>(uptime_s & 0xFF),
        static_cast<std::uint8_t>((uptime_s >> 8) & 0xFF),
        static_cast<std::uint8_t>((uptime_s >> 16) & 0xFF),
        static_cast<std::uint8_t>((uptime_s >> 24) & 0xFF),
        static_cast<std::uint8_t>(vbat_mv & 0xFF),
        static_cast<std::uint8_t>((vbat_mv >> 8) & 0xFF),
        mode,
        flags,
        static_cast<std::uint8_t>(reserved & 0xFF),
        static_cast<std::uint8_t>((reserved >> 8) & 0xFF),
        static_cast<std::uint8_t>((reserved >> 16) & 0xFF),
        static_cast<std::uint8_t>((reserved >> 24) & 0xFF),
    };
}

std::string Base64EncodeForTest(const std::vector<std::uint8_t>& data) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (i + 1 < data.size() ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0) |
                                (i + 2 < data.size() ? static_cast<std::uint32_t>(data[i + 2]) : 0);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < data.size() ? kTable[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < data.size() ? kTable[n & 0x3F] : '=');
    }
    return out;
}

std::vector<std::uint8_t> BuildStateJsonFrame(const std::string& json) {
    std::vector<std::uint8_t> frame{0x01, 0x10,
                                    static_cast<std::uint8_t>(json.size() & 0xFF),
                                    static_cast<std::uint8_t>((json.size() >> 8) & 0xFF)};
    frame.insert(frame.end(), json.begin(), json.end());
    return frame;
}


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
}

