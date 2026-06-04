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
    table[Index(StringId::kSettingsLaunchAtLogin)] = "Start VoiceStick when Windows starts";
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
    table[Index(StringId::kMenuLaunchAtLogin)] = "Start at Login";
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
    table[Index(StringId::kPairSignal)] = "Signal";
    table[Index(StringId::kPairBluetoothAddress)] = "Bluetooth Address";
    table[Index(StringId::kPairManualIdHint)] = "Can't find it? Enter the 4-digit ID shown on the Stick:";
    table[Index(StringId::kPairButton)] = "Pair";
    table[Index(StringId::kPairRetry)] = "Retry";
    table[Index(StringId::kPairStillScanning)] = "Still scanning... retrying Bluetooth scan";
    table[Index(StringId::kPairScanFailed)] = "Bluetooth scan failed.";
    table[Index(StringId::kPairAlreadyPaired)] = "This device is already paired. Forget it first or wait for reconnect.";
    table[Index(StringId::kPairWaitingForName)] = "Waiting for device name. Move the Stick closer or enter its ID below.";
    table[Index(StringId::kPairSelectDeviceWithId)] = "Select a VoiceStick with a device ID";
    table[Index(StringId::kPairSelectDevice)] = "Select a device";
    table[Index(StringId::kPairEnterManualId)] = "Enter the 4-digit ID shown on the Stick screen";
    table[Index(StringId::kPairSavedManualWaiting)] = "Saved VS-%s; waiting for advertisement...";
    table[Index(StringId::kPairPairingDevice)] = "Pairing VS-%s...";
    table[Index(StringId::kPairConnectedFinishing)] = "Connected to VS-%s. Finishing up...";
    table[Index(StringId::kPairPairedDevice)] = "Paired VS-%s";
    table[Index(StringId::kPairPairedDeviceFirmware)] = "Paired VS-%s firmware %s";
    table[Index(StringId::kPairTimedOut)] = "Pairing timed out";
    table[Index(StringId::kPairFoundCount)] = "%s found";
    table[Index(StringId::kPairScanningCount)] = "Scanning (%s advertisements)";
    table[Index(StringId::kOnboardingTitle)] = "Welcome to VoiceStick";
    table[Index(StringId::kOnboardingSubtitle)] = "Pair your device and start voice input from anywhere.";
    table[Index(StringId::kOnboardingGetStarted)] = "Get Started";
    table[Index(StringId::kOnboardingSetupTitle)] = "Set up VoiceStick";
    table[Index(StringId::kOnboardingDeviceStep)] = "Device";
    table[Index(StringId::kOnboardingAsrStep)] = "Voice Recognition";
    table[Index(StringId::kOnboardingReadyStep)] = "Ready";
    table[Index(StringId::kOnboardingBack)] = "Back";
    table[Index(StringId::kOnboardingNext)] = "Next";
    table[Index(StringId::kOnboardingFinish)] = "Finish";
    table[Index(StringId::kOnboardingDevicePairedContinue)] = "Device paired. Continue to voice recognition.";
    table[Index(StringId::kOnboardingPairDeviceTitle)] = "Pair your VoiceStick device.";
    table[Index(StringId::kOnboardingPairDeviceDescription)] = "Turn on your StickS3, then use the pairing window to select it.";
    table[Index(StringId::kOnboardingPairDeviceInstruction)] = "Press the front button on your device to dictate into the focused app.";
    table[Index(StringId::kOnboardingPairAnotherDevice)] = "Pair Another Device...";
    table[Index(StringId::kOnboardingPairDeviceButton)] = "Pair Device...";
    table[Index(StringId::kOnboardingChooseAsr)] = "Choose your speech recognition service.";
    table[Index(StringId::kOnboardingProvider)] = "Provider:";
    table[Index(StringId::kOnboardingApiKey)] = "API Key:";
    table[Index(StringId::kOnboardingResource)] = "Resource:";
    table[Index(StringId::kOnboardingReadyTitle)] = "VoiceStick is ready.";
    table[Index(StringId::kOnboardingReadyInstruction)] = "Press the front button on your device to dictate into the focused app.";
    table[Index(StringId::kOnboardingApplyingTrial)] = "Applying trial API key...";
    table[Index(StringId::kOnboardingTrialApplied)] = "Trial API key applied.";
    table[Index(StringId::kOnboardingTrialOpenFailed)] = "Could not open the trial application page.";
    table[Index(StringId::kOnboardingTrialPageOpened)] = "Opened trial application page.";
    table[Index(StringId::kOnboardingTrialApplyFailed)] = "Could not apply a trial API key.";
    table[Index(StringId::kOnboardingPairDeviceFirst)] = "Pair a VoiceStick device first.";
    table[Index(StringId::kOnboardingEnterApiKey)] = "Enter an API key or apply a trial key.";
    table[Index(StringId::kOnboardingDeviceNotPaired)] = "Device: Not paired";
    table[Index(StringId::kOnboardingDeviceSummary)] = "Device: VS-%s";
    table[Index(StringId::kOnboardingAsrSummary)] = "ASR: %s";
    table[Index(StringId::kFirmwareUpdateTitle)] = "Firmware Update";
    table[Index(StringId::kFirmwareUpdateAvailable)] = "A firmware update is available.";
    table[Index(StringId::kFirmwareUpdateRequired)] = "A firmware update is required.";
    table[Index(StringId::kFirmwareUpdateButton)] = "Update";
    table[Index(StringId::kFirmwareUpdateUpToDate)] = "Firmware is up to date.";
    table[Index(StringId::kFirmwareUpdating)] = "Updating firmware...";
    table[Index(StringId::kFirmwareUpdateSuccess)] = "Firmware updated successfully.";
    table[Index(StringId::kFirmwareUpdateFailed)] = "Firmware update failed.";
    table[Index(StringId::kFirmwareUpdateFinalizing)] = "Finalizing firmware update...";
    table[Index(StringId::kFirmwareUpdateTransferring)] = "Transferring firmware over BLE...";
    table[Index(StringId::kFirmwareUpdatedTitle)] = "Firmware Updated";
    table[Index(StringId::kFirmwareUpdatedDetail)] = "The device is rebooting into the new firmware.";
    table[Index(StringId::kFirmwareCancellingTitle)] = "Cancelling Firmware Update";
    table[Index(StringId::kFirmwareCancellingDetail)] = "Stopping transfer and asking the device to abort.";
    table[Index(StringId::kFirmwareDownloading)] = "Downloading OTA firmware %s...";
    table[Index(StringId::kFirmwareCheckFailed)] = "Firmware Check Failed";
    table[Index(StringId::kFirmwareChecking)] = "Checking for firmware updates...";
    table[Index(StringId::kFirmwareUpdateAvailableMenu)] = "Update available: %s";
    table[Index(StringId::kFirmwareLatestMenu)] = "Latest firmware %s";
    table[Index(StringId::kFirmwareUpdateTo)] = "Update to %s...";
    table[Index(StringId::kFirmwareUpdatePromptTitleRequired)] = "Firmware update recommended";
    table[Index(StringId::kFirmwareUpdatePromptTitleAvailable)] = "Firmware update available";
    table[Index(StringId::kFirmwareUpdatePromptBody)] = "VS-%s is running firmware %s.\n\nThe latest firmware is %s.";
    table[Index(StringId::kHotkeyTitle)] = "Hotkey Settings";
    table[Index(StringId::kHotkeyEnabled)] = "Enable global hotkey";
    table[Index(StringId::kHotkeyPreset)] = "Preset";
    table[Index(StringId::kHotkeyCustomKey)] = "Custom Key";
    table[Index(StringId::kHotkeyCurrent)] = "Current hotkey:";
    table[Index(StringId::kHotkeyCaptureButton)] = "Click to record hotkey";
    table[Index(StringId::kHotkeyCapturePrompt)] = "Press a hotkey combination...";
    table[Index(StringId::kHotkeyHint)] = "Hint: at least 1 modifier (Ctrl/Alt/Shift/Win) + 1 main key";
    table[Index(StringId::kHotkeyMissingModifier)] = "Error: at least 1 modifier is required (Ctrl/Alt/Shift/Win)";
    table[Index(StringId::kHotkeyConflictTitle)] = "Hotkey conflict";
    table[Index(StringId::kHotkeyConflictMessage)] = "This hotkey is already used by another app. Choose another combination.";
    table[Index(StringId::kCloudNeedsAttentionTitle)] = "VoiceStick Cloud needs attention";
    table[Index(StringId::kCloudOpenPageQuestion)] = "Open the VoiceStick Cloud page?";
    table[Index(StringId::kNotificationPairedTitle)] = "VoiceStick paired";
    table[Index(StringId::kNotificationManualPairSavedTitle)] = "VoiceStick pairing saved";
    table[Index(StringId::kNotificationManualPairSavedBody)] = "Waiting for VS-%s to advertise.";
    table[Index(StringId::kNotificationFirmwareUpdatedTitle)] = "VoiceStick firmware updated";
    table[Index(StringId::kNotificationFirmwareUpdatedBody)] = "The device is rebooting into the new firmware.";
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
    table[Index(StringId::kSettingsLaunchAtLogin)] = "Windows 启动时自动运行 VoiceStick";
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
    table[Index(StringId::kMenuLaunchAtLogin)] = "开机自启动";
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
    table[Index(StringId::kPairSignal)] = "信号";
    table[Index(StringId::kPairBluetoothAddress)] = "蓝牙地址";
    table[Index(StringId::kPairManualIdHint)] = "找不到设备？请输入 Stick 屏幕显示的 4 位 ID：";
    table[Index(StringId::kPairButton)] = "配对";
    table[Index(StringId::kPairRetry)] = "重试";
    table[Index(StringId::kPairStillScanning)] = "仍在扫描...正在重试蓝牙扫描";
    table[Index(StringId::kPairScanFailed)] = "蓝牙扫描失败。";
    table[Index(StringId::kPairAlreadyPaired)] = "该设备已配对，请先忘记设备或等待重新连接。";
    table[Index(StringId::kPairWaitingForName)] = "正在等待设备名。请将 Stick 靠近电脑，或在下方输入 ID。";
    table[Index(StringId::kPairSelectDeviceWithId)] = "请选择带设备 ID 的 VoiceStick";
    table[Index(StringId::kPairSelectDevice)] = "请选择设备";
    table[Index(StringId::kPairEnterManualId)] = "请输入 Stick 屏幕显示的 4 位 ID";
    table[Index(StringId::kPairSavedManualWaiting)] = "已保存 VS-%s，正在等待设备广播...";
    table[Index(StringId::kPairPairingDevice)] = "正在配对 VS-%s...";
    table[Index(StringId::kPairConnectedFinishing)] = "已连接 VS-%s，正在完成配对...";
    table[Index(StringId::kPairPairedDevice)] = "已配对 VS-%s";
    table[Index(StringId::kPairPairedDeviceFirmware)] = "已配对 VS-%s，固件版本 %s";
    table[Index(StringId::kPairTimedOut)] = "配对超时";
    table[Index(StringId::kPairFoundCount)] = "找到 %s 个设备";
    table[Index(StringId::kPairScanningCount)] = "正在扫描（已收到 %s 条广播）";
    table[Index(StringId::kOnboardingTitle)] = "欢迎使用 VoiceStick";
    table[Index(StringId::kOnboardingSubtitle)] = "配对设备后，即可在任意位置使用语音输入。";
    table[Index(StringId::kOnboardingGetStarted)] = "开始使用";
    table[Index(StringId::kOnboardingSetupTitle)] = "设置 VoiceStick";
    table[Index(StringId::kOnboardingDeviceStep)] = "设备";
    table[Index(StringId::kOnboardingAsrStep)] = "语音识别";
    table[Index(StringId::kOnboardingReadyStep)] = "就绪";
    table[Index(StringId::kOnboardingBack)] = "上一步";
    table[Index(StringId::kOnboardingNext)] = "下一步";
    table[Index(StringId::kOnboardingFinish)] = "完成";
    table[Index(StringId::kOnboardingDevicePairedContinue)] = "设备已配对，请继续设置语音识别。";
    table[Index(StringId::kOnboardingPairDeviceTitle)] = "配对你的 VoiceStick 设备。";
    table[Index(StringId::kOnboardingPairDeviceDescription)] = "打开 StickS3，然后在配对窗口中选择它。";
    table[Index(StringId::kOnboardingPairDeviceInstruction)] = "按下设备正面按键，即可向当前应用进行语音输入。";
    table[Index(StringId::kOnboardingPairAnotherDevice)] = "配对其他设备...";
    table[Index(StringId::kOnboardingPairDeviceButton)] = "配对设备...";
    table[Index(StringId::kOnboardingChooseAsr)] = "选择语音识别服务。";
    table[Index(StringId::kOnboardingProvider)] = "服务方：";
    table[Index(StringId::kOnboardingApiKey)] = "API Key：";
    table[Index(StringId::kOnboardingResource)] = "资源：";
    table[Index(StringId::kOnboardingReadyTitle)] = "VoiceStick 已就绪。";
    table[Index(StringId::kOnboardingReadyInstruction)] = "按下设备正面按键，即可向当前应用进行语音输入。";
    table[Index(StringId::kOnboardingApplyingTrial)] = "正在申请试用 API Key...";
    table[Index(StringId::kOnboardingTrialApplied)] = "试用 API Key 已应用。";
    table[Index(StringId::kOnboardingTrialOpenFailed)] = "无法打开试用申请页面。";
    table[Index(StringId::kOnboardingTrialPageOpened)] = "已打开试用申请页面。";
    table[Index(StringId::kOnboardingTrialApplyFailed)] = "无法申请试用 API Key。";
    table[Index(StringId::kOnboardingPairDeviceFirst)] = "请先配对 VoiceStick 设备。";
    table[Index(StringId::kOnboardingEnterApiKey)] = "请输入 API Key 或申请试用 Key。";
    table[Index(StringId::kOnboardingDeviceNotPaired)] = "设备：未配对";
    table[Index(StringId::kOnboardingDeviceSummary)] = "设备：VS-%s";
    table[Index(StringId::kOnboardingAsrSummary)] = "语音识别：%s";
    table[Index(StringId::kFirmwareUpdateTitle)] = "固件更新";
    table[Index(StringId::kFirmwareUpdateAvailable)] = "有可用的固件更新。";
    table[Index(StringId::kFirmwareUpdateRequired)] = "需要更新固件。";
    table[Index(StringId::kFirmwareUpdateButton)] = "更新";
    table[Index(StringId::kFirmwareUpdateUpToDate)] = "固件已是最新。";
    table[Index(StringId::kFirmwareUpdating)] = "正在更新固件...";
    table[Index(StringId::kFirmwareUpdateSuccess)] = "固件更新成功。";
    table[Index(StringId::kFirmwareUpdateFailed)] = "固件更新失败。";
    table[Index(StringId::kFirmwareUpdateFinalizing)] = "正在完成固件更新...";
    table[Index(StringId::kFirmwareUpdateTransferring)] = "正在通过 BLE 传输固件...";
    table[Index(StringId::kFirmwareUpdatedTitle)] = "固件已更新";
    table[Index(StringId::kFirmwareUpdatedDetail)] = "设备正在重启到新固件。";
    table[Index(StringId::kFirmwareCancellingTitle)] = "正在取消固件更新";
    table[Index(StringId::kFirmwareCancellingDetail)] = "正在停止传输并通知设备中止。";
    table[Index(StringId::kFirmwareDownloading)] = "正在下载 OTA 固件 %s...";
    table[Index(StringId::kFirmwareCheckFailed)] = "固件检查失败";
    table[Index(StringId::kFirmwareChecking)] = "正在检查固件更新...";
    table[Index(StringId::kFirmwareUpdateAvailableMenu)] = "可更新版本：%s";
    table[Index(StringId::kFirmwareLatestMenu)] = "最新固件 %s";
    table[Index(StringId::kFirmwareUpdateTo)] = "更新到 %s...";
    table[Index(StringId::kFirmwareUpdatePromptTitleRequired)] = "建议更新固件";
    table[Index(StringId::kFirmwareUpdatePromptTitleAvailable)] = "有可用固件更新";
    table[Index(StringId::kFirmwareUpdatePromptBody)] = "VS-%s 当前运行固件 %s。\n\n最新固件为 %s。";
    table[Index(StringId::kHotkeyTitle)] = "热键设置";
    table[Index(StringId::kHotkeyEnabled)] = "启用全局热键";
    table[Index(StringId::kHotkeyPreset)] = "预设";
    table[Index(StringId::kHotkeyCustomKey)] = "自定义按键";
    table[Index(StringId::kHotkeyCurrent)] = "当前快捷键：";
    table[Index(StringId::kHotkeyCaptureButton)] = "点击录制快捷键";
    table[Index(StringId::kHotkeyCapturePrompt)] = "请按下快捷键组合...";
    table[Index(StringId::kHotkeyHint)] = "提示：至少需要 1 个修饰键（Ctrl/Alt/Shift/Win）+ 1 个主键";
    table[Index(StringId::kHotkeyMissingModifier)] = "错误：至少需要 1 个修饰键（Ctrl/Alt/Shift/Win）";
    table[Index(StringId::kHotkeyConflictTitle)] = "热键冲突";
    table[Index(StringId::kHotkeyConflictMessage)] = "该快捷键已被其他程序占用，请选择其他组合。";
    table[Index(StringId::kCloudNeedsAttentionTitle)] = "VoiceStick Cloud 需要处理";
    table[Index(StringId::kCloudOpenPageQuestion)] = "是否打开 VoiceStick Cloud 页面？";
    table[Index(StringId::kNotificationPairedTitle)] = "VoiceStick 已配对";
    table[Index(StringId::kNotificationManualPairSavedTitle)] = "VoiceStick 配对已保存";
    table[Index(StringId::kNotificationManualPairSavedBody)] = "正在等待 VS-%s 广播。";
    table[Index(StringId::kNotificationFirmwareUpdatedTitle)] = "VoiceStick 固件已更新";
    table[Index(StringId::kNotificationFirmwareUpdatedBody)] = "设备正在重启到新固件。";
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

std::string BatteryStatusText(int level_percent, bool charging, bool usb_powered, UiLanguage language) {
    std::string text = std::to_string(level_percent) + "%";
    if (charging) {
        text += language == UiLanguage::kSimplifiedChinese ? "，充电中" : ", charging";
    } else if (usb_powered) {
        text += language == UiLanguage::kSimplifiedChinese ? "，外接电源" : ", plugged in";
    }
    return text;
}

std::wstring DeviceTitleWithBattery(const std::wstring& title,
                                    int level_percent,
                                    bool charging,
                                    bool usb_powered,
                                    UiLanguage language) {
    return title + L" (" + Utf16FromUtf8(BatteryStatusText(level_percent, charging, usb_powered, language)) + L")";
}

bool LocalizationTablesAreComplete() {
    for (std::size_t i = 0; i < kStringCount; ++i) {
        if (kEnglish[i].empty() || kChinese[i].empty()) return false;
    }
    return true;
}

} // namespace voicestick
