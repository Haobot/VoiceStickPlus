#include "localization.h"

#include <Windows.h>

#include <array>
#include <string_view>

namespace voicestick {

namespace {

constexpr std::size_t kStringCount = static_cast<std::size_t>(StringId::kNotificationHotkeyConflictBody) + 1;

using StringTable = std::array<std::string_view, kStringCount>;

constexpr std::size_t Index(StringId id) {
    return static_cast<std::size_t>(id);
}

constexpr StringTable EnglishStrings() {
    StringTable table{};
    table[Index(StringId::kOk)] = "OK";
    table[Index(StringId::kCancel)] = "Cancel";
    table[Index(StringId::kSave)] = "Save";
    table[Index(StringId::kClose)] = "Close";
    table[Index(StringId::kSettingsTitle)] = "VoiceStick Settings";
    table[Index(StringId::kSettingsLanguage)] = "Language";
    table[Index(StringId::kSettingsLanguageSystem)] = "Follow System";
    table[Index(StringId::kSettingsLanguageEnglish)] = "English";
    table[Index(StringId::kSettingsLanguageChineseSimplified)] = "Simplified Chinese";
    table[Index(StringId::kSettingsProvider)] = "Provider";
    table[Index(StringId::kSettingsApiKey)] = "API Key";
    table[Index(StringId::kSettingsResourceId)] = "Resource ID";
    table[Index(StringId::kSettingsHotwords)] = "Hotwords";
    table[Index(StringId::kSettingsHotwordsHint)] = "Separate hotwords with commas or new lines.";
    table[Index(StringId::kSettingsLlmBaseUrl)] = "Base URL";
    table[Index(StringId::kSettingsLlmApiKey)] = "API Key";
    table[Index(StringId::kSettingsLlmModel)] = "Model";
    table[Index(StringId::kSettingsPromptTone)] = "Play prompt tone on device";
    table[Index(StringId::kSettingsDebugAudio)] = "Save debug audio files";
    table[Index(StringId::kSettingsDebugDir)] = "Audio Folder";
    table[Index(StringId::kSettingsChooseDir)] = "Choose...";
    table[Index(StringId::kSettingsOpenConfigFolder)] = "Open Config Folder";
    table[Index(StringId::kSettingsApplyTrial)] = "Apply Trial";
    table[Index(StringId::kSettingsApplyingTrial)] = "Applying trial API key...";
    table[Index(StringId::kSettingsTrialApplied)] = "Trial API key applied.";
    table[Index(StringId::kSettingsTrialPageOpened)] = "Opened trial application page.";
    table[Index(StringId::kSettingsTrialFailedTitle)] = "Could Not Apply Trial API Key";
    table[Index(StringId::kSettingsTrialFailedMessage)] = "Please try again later.";
    table[Index(StringId::kSettingsSaved)] = "Settings saved.";
    table[Index(StringId::kSettingsSaveFailed)] = "Could not save settings.";
    table[Index(StringId::kMenuPairDevice)] = "Pair Device...";
    table[Index(StringId::kMenuSettings)] = "Settings...";
    table[Index(StringId::kMenuQuit)] = "Quit";
    table[Index(StringId::kMenuWebsite)] = "Website";
    table[Index(StringId::kMenuCheckAppUpdates)] = "Check for App Updates...";
    table[Index(StringId::kMenuRestoreLastInput)] = "Restore Last Input";
    table[Index(StringId::kMenuInteraction)] = "Interaction";
    table[Index(StringId::kMenuHoldToTalk)] = "Hold to Talk";
    table[Index(StringId::kMenuClickToTalk)] = "Click to Talk";
    table[Index(StringId::kMenuAutoEnter)] = "Press Return After Paste";
    table[Index(StringId::kMenuOutput)] = "Output";
    table[Index(StringId::kMenuOutputFocusedApp)] = "Focused App";
    table[Index(StringId::kMenuOutputSubtitle)] = "Subtitle";
    table[Index(StringId::kMenuTranslation)] = "Translation";
    table[Index(StringId::kMenuOriginal)] = "Original";
    table[Index(StringId::kMenuThemeColor)] = "Theme Color";
    table[Index(StringId::kMenuThemeSize)] = "Theme Size";
    table[Index(StringId::kMenuOverlayPosition)] = "Overlay Position";
    table[Index(StringId::kMenuForgetDevice)] = "Forget Device";
    table[Index(StringId::kMenuUpdateFirmware)] = "Update Firmware...";
    table[Index(StringId::kMenuFirmwareUpToDate)] = "Firmware Up to Date";
    table[Index(StringId::kMenuHotkey)] = "Hotkey";
    table[Index(StringId::kMenuHotkeyEnabled)] = "Enable Hotkey";
    table[Index(StringId::kMenuHotkeyCustom)] = "Customize Hotkey...";
    table[Index(StringId::kMenuHotkeyConflictTitle)] = "Hotkey registration failed";
    table[Index(StringId::kMenuHotkeyConflictBody)] = "The selected hotkey is already in use. Choose another hotkey from the menu.";
    table[Index(StringId::kStatusNoPairedDevices)] = "No paired VoiceStick devices";
    table[Index(StringId::kStatusScanning)] = "Scanning...";
    table[Index(StringId::kStatusConnected)] = "Connected";
    table[Index(StringId::kStatusDisconnected)] = "Disconnected";
    table[Index(StringId::kStatusReady)] = "Ready";
    table[Index(StringId::kPairTitle)] = "Pair VoiceStick";
    table[Index(StringId::kPairScanning)] = "Scanning for VoiceStick devices...";
    table[Index(StringId::kPairNoDevices)] = "No VoiceStick devices found.";
    table[Index(StringId::kPairConnect)] = "Connect";
    table[Index(StringId::kPairConnecting)] = "Connecting...";
    table[Index(StringId::kPairConnected)] = "Connected.";
    table[Index(StringId::kPairFailed)] = "Pairing failed.";
    table[Index(StringId::kOnboardingTitle)] = "Welcome to VoiceStick";
    table[Index(StringId::kOnboardingSubtitle)] = "Pair your device and start voice input from anywhere.";
    table[Index(StringId::kOnboardingGetStarted)] = "Get Started";
    table[Index(StringId::kFirmwareUpdateTitle)] = "Firmware Update";
    table[Index(StringId::kFirmwareUpdateAvailable)] = "A firmware update is available.";
    table[Index(StringId::kFirmwareUpdateRequired)] = "A firmware update is required.";
    table[Index(StringId::kFirmwareUpdateButton)] = "Update";
    table[Index(StringId::kFirmwareUpdateUpToDate)] = "Firmware is up to date.";
    table[Index(StringId::kFirmwareUpdating)] = "Updating firmware...";
    table[Index(StringId::kFirmwareUpdateSuccess)] = "Firmware updated successfully.";
    table[Index(StringId::kFirmwareUpdateFailed)] = "Firmware update failed.";
    table[Index(StringId::kHotkeyTitle)] = "Hotkey Settings";
    table[Index(StringId::kHotkeyEnabled)] = "Enable global hotkey";
    table[Index(StringId::kHotkeyPreset)] = "Preset";
    table[Index(StringId::kHotkeyCustomKey)] = "Custom Key";
    table[Index(StringId::kOverlayListening)] = "Listening...";
    table[Index(StringId::kOverlayThinking)] = "Thinking...";
    table[Index(StringId::kOverlayError)] = "Error";
    table[Index(StringId::kOverlayAsrError)] = "ASR Error";
    table[Index(StringId::kOverlayNoSpeech)] = "No speech detected";
    table[Index(StringId::kNotificationHotkeyConflictTitle)] = "Hotkey registration failed";
    table[Index(StringId::kNotificationHotkeyConflictBody)] = "The selected hotkey is already in use. Choose another hotkey from the menu.";
    return table;
}

constexpr StringTable ChineseStrings() {
    StringTable table{};
    table[Index(StringId::kOk)] = "确定";
    table[Index(StringId::kCancel)] = "取消";
    table[Index(StringId::kSave)] = "保存";
    table[Index(StringId::kClose)] = "关闭";
    table[Index(StringId::kSettingsTitle)] = "VoiceStick 设置";
    table[Index(StringId::kSettingsLanguage)] = "界面语言";
    table[Index(StringId::kSettingsLanguageSystem)] = "跟随系统";
    table[Index(StringId::kSettingsLanguageEnglish)] = "英文";
    table[Index(StringId::kSettingsLanguageChineseSimplified)] = "简体中文";
    table[Index(StringId::kSettingsProvider)] = "服务提供方";
    table[Index(StringId::kSettingsApiKey)] = "API Key";
    table[Index(StringId::kSettingsResourceId)] = "资源 ID";
    table[Index(StringId::kSettingsHotwords)] = "热词";
    table[Index(StringId::kSettingsHotwordsHint)] = "使用逗号或换行分隔热词。";
    table[Index(StringId::kSettingsLlmBaseUrl)] = "Base URL";
    table[Index(StringId::kSettingsLlmApiKey)] = "API Key";
    table[Index(StringId::kSettingsLlmModel)] = "模型";
    table[Index(StringId::kSettingsPromptTone)] = "在设备上播放提示音";
    table[Index(StringId::kSettingsDebugAudio)] = "保存调试音频文件";
    table[Index(StringId::kSettingsDebugDir)] = "音频文件夹";
    table[Index(StringId::kSettingsChooseDir)] = "选择...";
    table[Index(StringId::kSettingsOpenConfigFolder)] = "打开配置文件夹";
    table[Index(StringId::kSettingsApplyTrial)] = "申请试用";
    table[Index(StringId::kSettingsApplyingTrial)] = "正在申请试用 API Key...";
    table[Index(StringId::kSettingsTrialApplied)] = "试用 API Key 已应用。";
    table[Index(StringId::kSettingsTrialPageOpened)] = "已打开试用申请页面。";
    table[Index(StringId::kSettingsTrialFailedTitle)] = "无法申请试用 API Key";
    table[Index(StringId::kSettingsTrialFailedMessage)] = "请稍后重试。";
    table[Index(StringId::kSettingsSaved)] = "设置已保存。";
    table[Index(StringId::kSettingsSaveFailed)] = "无法保存设置。";
    table[Index(StringId::kMenuPairDevice)] = "配对设备...";
    table[Index(StringId::kMenuSettings)] = "设置...";
    table[Index(StringId::kMenuQuit)] = "退出";
    table[Index(StringId::kMenuWebsite)] = "官网";
    table[Index(StringId::kMenuCheckAppUpdates)] = "检查应用更新...";
    table[Index(StringId::kMenuRestoreLastInput)] = "恢复上次输入";
    table[Index(StringId::kMenuInteraction)] = "交互方式";
    table[Index(StringId::kMenuHoldToTalk)] = "按住说话";
    table[Index(StringId::kMenuClickToTalk)] = "点击说话";
    table[Index(StringId::kMenuAutoEnter)] = "粘贴后按回车";
    table[Index(StringId::kMenuOutput)] = "输出";
    table[Index(StringId::kMenuOutputFocusedApp)] = "当前应用";
    table[Index(StringId::kMenuOutputSubtitle)] = "字幕";
    table[Index(StringId::kMenuTranslation)] = "翻译";
    table[Index(StringId::kMenuOriginal)] = "原文";
    table[Index(StringId::kMenuThemeColor)] = "主题颜色";
    table[Index(StringId::kMenuThemeSize)] = "主题大小";
    table[Index(StringId::kMenuOverlayPosition)] = "悬浮窗位置";
    table[Index(StringId::kMenuForgetDevice)] = "忘记设备";
    table[Index(StringId::kMenuUpdateFirmware)] = "更新固件...";
    table[Index(StringId::kMenuFirmwareUpToDate)] = "固件已是最新";
    table[Index(StringId::kMenuHotkey)] = "热键";
    table[Index(StringId::kMenuHotkeyEnabled)] = "启用热键";
    table[Index(StringId::kMenuHotkeyCustom)] = "自定义热键...";
    table[Index(StringId::kMenuHotkeyConflictTitle)] = "热键注册失败";
    table[Index(StringId::kMenuHotkeyConflictBody)] = "所选热键已被其他程序占用，请在菜单中更换其他热键。";
    table[Index(StringId::kStatusNoPairedDevices)] = "没有已配对的 VoiceStick 设备";
    table[Index(StringId::kStatusScanning)] = "正在扫描...";
    table[Index(StringId::kStatusConnected)] = "已连接";
    table[Index(StringId::kStatusDisconnected)] = "未连接";
    table[Index(StringId::kStatusReady)] = "就绪";
    table[Index(StringId::kPairTitle)] = "配对 VoiceStick";
    table[Index(StringId::kPairScanning)] = "正在扫描 VoiceStick 设备...";
    table[Index(StringId::kPairNoDevices)] = "未找到 VoiceStick 设备。";
    table[Index(StringId::kPairConnect)] = "连接";
    table[Index(StringId::kPairConnecting)] = "正在连接...";
    table[Index(StringId::kPairConnected)] = "已连接。";
    table[Index(StringId::kPairFailed)] = "配对失败。";
    table[Index(StringId::kOnboardingTitle)] = "欢迎使用 VoiceStick";
    table[Index(StringId::kOnboardingSubtitle)] = "配对设备后，即可在任意位置使用语音输入。";
    table[Index(StringId::kOnboardingGetStarted)] = "开始使用";
    table[Index(StringId::kFirmwareUpdateTitle)] = "固件更新";
    table[Index(StringId::kFirmwareUpdateAvailable)] = "有可用的固件更新。";
    table[Index(StringId::kFirmwareUpdateRequired)] = "需要更新固件。";
    table[Index(StringId::kFirmwareUpdateButton)] = "更新";
    table[Index(StringId::kFirmwareUpdateUpToDate)] = "固件已是最新。";
    table[Index(StringId::kFirmwareUpdating)] = "正在更新固件...";
    table[Index(StringId::kFirmwareUpdateSuccess)] = "固件更新成功。";
    table[Index(StringId::kFirmwareUpdateFailed)] = "固件更新失败。";
    table[Index(StringId::kHotkeyTitle)] = "热键设置";
    table[Index(StringId::kHotkeyEnabled)] = "启用全局热键";
    table[Index(StringId::kHotkeyPreset)] = "预设";
    table[Index(StringId::kHotkeyCustomKey)] = "自定义按键";
    table[Index(StringId::kOverlayListening)] = "正在聆听...";
    table[Index(StringId::kOverlayThinking)] = "正在思考...";
    table[Index(StringId::kOverlayError)] = "错误";
    table[Index(StringId::kOverlayAsrError)] = "识别错误";
    table[Index(StringId::kOverlayNoSpeech)] = "未检测到语音";
    table[Index(StringId::kNotificationHotkeyConflictTitle)] = "热键注册失败";
    table[Index(StringId::kNotificationHotkeyConflictBody)] = "所选热键已被其他程序占用，请在菜单中更换其他热键。";
    return table;
}

constexpr StringTable kEnglish = EnglishStrings();
constexpr StringTable kChinese = ChineseStrings();

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

} // namespace

std::string Tr(StringId id, UiLanguage language) {
    const auto index = Index(id);
    const auto& table = language == UiLanguage::kSimplifiedChinese ? kChinese : kEnglish;
    if (index < table.size() && !table[index].empty()) {
        return std::string(table[index]);
    }
    if (index < kEnglish.size()) return std::string(kEnglish[index]);
    return {};
}

std::wstring TrW(StringId id, UiLanguage language) {
    return Utf16FromUtf8(Tr(id, language));
}

bool LocalizationTablesAreComplete() {
    for (std::size_t i = 0; i < kStringCount; ++i) {
        if (kEnglish[i].empty() || kChinese[i].empty()) return false;
    }
    return true;
}

} // namespace voicestick
