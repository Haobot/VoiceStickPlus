#include "localization.h"

#include <Windows.h>

#include <array>
#include <string_view>

namespace voicestick {

namespace {

constexpr std::size_t kStringCount = static_cast<std::size_t>(StringId::kHotwordTrimBodySuffix) + 1;

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
    table[Index(StringId::kSettingsRefineText)] = "Refine text (remove pause spaces, fix punctuation, drop fillers)";
    table[Index(StringId::kSettingsRefinePrompt)] = "Refine Prompt";
    table[Index(StringId::kSettingsLaunchAtLogin)] = "Start VoiceStick when Windows starts";
    table[Index(StringId::kSettingsSelectionHotword)] = "Show 'Add to Hotwords' button when selecting text";
    table[Index(StringId::kSettingsSelectionHotwordHint)] = "Select text in any app to quickly add it as a hotword.";
    table[Index(StringId::kSettingsSectionHotwordProcess)] = "Hotword Processing";
    table[Index(StringId::kSettingsHotwordProcessEnable)] = "Extract hotwords from selected text with LLM";
    table[Index(StringId::kSettingsHotwordProcessPrompt)] = "Extraction prompt";
    table[Index(StringId::kSettingsDebugAudio)] = "Save debug audio files";
    table[Index(StringId::kSettingsShowImuDebug)] = "Show accelerometer debug values";
    table[Index(StringId::kSettingsImuWakeSensitivity)] = "Wake Sensitivity";
    table[Index(StringId::kSettingsImuWakeSensitivityLow)] = "Low";
    table[Index(StringId::kSettingsImuWakeSensitivityMedium)] = "Medium";
    table[Index(StringId::kSettingsImuWakeSensitivityHigh)] = "High";
    table[Index(StringId::kSettingsTapToArrow)] = "Double-tap device to press Down arrow";
    table[Index(StringId::kSettingsTapSensitivity)] = "Tap Sensitivity";
    table[Index(StringId::kSettingsAirMouseSensitivityX)] = "Left/Right Sensitivity";
    table[Index(StringId::kSettingsAirMouseSensitivityY)] = "Up/Down Sensitivity";
    table[Index(StringId::kSettingsOutputTarget)] = "Output Target";
    table[Index(StringId::kSettingsOutputTargetFocusedApp)] = "Focused App";
    table[Index(StringId::kSettingsOutputTargetSubtitle)] = "Subtitle";
    table[Index(StringId::kSettingsOutputTargetWechatInputMethod)] = "Third-party Input Method";
    table[Index(StringId::kSettingsWechatHotkey)] = "Voice Hotkey";
    table[Index(StringId::kSettingsWechatVirtualMic)] = "Virtual Microphone Playback Name";
    table[Index(StringId::kSettingsWechatAutoSwitch)] = "Auto-switch default recording device during recording";
    table[Index(StringId::kSettingsWechatVirtualMicCapture)] = "Virtual Microphone Capture Name";
    table[Index(StringId::kSettingsTriggerMode)] = "Trigger Mode";
    table[Index(StringId::kSettingsTriggerModeHold)] = "Hold (WeChat Input Method)";
    table[Index(StringId::kSettingsTriggerModeClick)] = "Click (Typeless, etc.)";
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
    table[Index(StringId::kSettingsSectionGeneral)] = "General";
    table[Index(StringId::kSettingsSectionAsr)] = "Speech Recognition";
    table[Index(StringId::kSettingsSectionRefine)] = "Text Refinement";
    table[Index(StringId::kSettingsSectionOutput)] = "Output";
    table[Index(StringId::kSettingsSectionDevice)] = "Device Interaction";
    table[Index(StringId::kSettingsSectionSystem)] = "System";
    table[Index(StringId::kSettingsDeveloperMode)] = "Developer Mode (show all advanced settings)";
    table[Index(StringId::kMenuPairDevice)] = "Pair Device...";
    table[Index(StringId::kMenuSettings)] = "Settings...";
    table[Index(StringId::kMenuQuit)] = "Quit";
    table[Index(StringId::kMenuRelaunchElevated)] = "Restart as Administrator";
    table[Index(StringId::kMenuWebsite)] = "Website";
    table[Index(StringId::kMenuCheckAppUpdates)] = "Check for App Updates...";
    table[Index(StringId::kMenuFlashTool)] = "Firmware Flash Tool...";
    table[Index(StringId::kMenuRestoreLastInput)] = "Restore Last Input";
    table[Index(StringId::kMenuInteraction)] = "Interaction";
    table[Index(StringId::kMenuHoldToTalk)] = "Hold to Talk";
    table[Index(StringId::kMenuClickToTalk)] = "Click to Talk";
    table[Index(StringId::kMenuAutoEnter)] = "Press Return After Paste";
    table[Index(StringId::kMenuLaunchAtLogin)] = "Start at Login";
    table[Index(StringId::kMenuSelectionHotword)] = "Selection Hotword";
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
    table[Index(StringId::kMenuUpdateFirmwareFromFile)] = "Update Firmware from File...";
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
    table[Index(StringId::kStatusAirMouseActive)] = "Air Mouse active (side key to exit)";
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
    table[Index(StringId::kPairSavedManualWaiting)] = "Saved %s; waiting for advertisement...";
    table[Index(StringId::kPairPairingDevice)] = "Pairing %s...";
    table[Index(StringId::kPairConnectedFinishing)] = "Connected to %s. Finishing up...";
    table[Index(StringId::kPairPairedDevice)] = "Paired %s";
    table[Index(StringId::kPairPairedDeviceFirmware)] = "Paired %s firmware %s";
    table[Index(StringId::kPairTimedOut)] = "Pairing timed out";
    table[Index(StringId::kPairFoundCount)] = "%s found";
    table[Index(StringId::kPairScanningCount)] = "Scanning (%s advertisements)";
    table[Index(StringId::kDeviceTypeVoiceStick)] = "Voice Stick";
    table[Index(StringId::kDeviceTypeXiaomiRemote)] = "Xiaomi Remote";
    table[Index(StringId::kPairXiaomiOsPairing)] = "Pairing with Windows Bluetooth...";
    table[Index(StringId::kPairXiaomiBondFailed)] =
        "Windows pairing failed. Remove the remote in Bluetooth settings, then re-add it and rescan.";
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
    table[Index(StringId::kOnboardingSkipDeviceConfirm)] =
        "No device paired yet. You can finish setup now, flash firmware later via the tray menu "
        "\"Firmware Flash Tool...\", then pair the device.\n\nContinue without pairing a device?";
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
    table[Index(StringId::kFirmwareUpdateAdvancedComFlash)] = "Advanced: COM Flash...";
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

    // Restart as administrator
    table[Index(StringId::kRelaunchFailedTitle)] = "Could Not Restart as Administrator";
    table[Index(StringId::kRelaunchFailedPath)] =
        "Could not get the program path. Right-click VoiceStick.exe and choose \"Run as administrator\".";
    table[Index(StringId::kRelaunchFailedUac)] =
        "UAC was not confirmed or failed. Right-click VoiceStick.exe and choose \"Run as administrator\".";
    table[Index(StringId::kElevationNeededTitle)] = "VoiceStick Needs Administrator Rights";
    table[Index(StringId::kElevationNeededBody)] =
        "%s is running with elevated privileges, so voice input is blocked by the system. "
        "Right-click the tray icon -> Restart as Administrator, then retry.";

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

    // 划词添加热词
    table[Index(StringId::kSelectionHotwordButton)] = "Add to Hotwords";
    table[Index(StringId::kSelectionHotwordAddedTitle)] = "Hotword Added";
    table[Index(StringId::kSelectionHotwordAddedBody)] = "Added to hotwords: ";
    table[Index(StringId::kSelectionHotwordDuplicateTitle)] = "Already a Hotword";
    table[Index(StringId::kSelectionHotwordDuplicateBody)] = "Already in hotwords: ";
    table[Index(StringId::kSelectionHotwordEmptyTitle)] = "No Text Selected";
    table[Index(StringId::kSelectionHotwordEmptyBody)] = "No selectable text was found.";
    table[Index(StringId::kSelectionHotwordTooLongTitle)] = "Selection Too Long";
    table[Index(StringId::kSelectionHotwordTooLongBody)] = "Selection is too long to be a hotword and was ignored.";
    table[Index(StringId::kHotwordCandidateNotifyTitle)] = "Hotword Suggestion";
    table[Index(StringId::kHotwordCandidateNotifyBodySuffix)] =
        " was corrected repeatedly. Review it in Settings - Hotwords.";
    table[Index(StringId::kSettingsHotwordCandidatesLabel)] =
        "Suggested hotwords (auto-mined from corrections, added only on your confirmation):";
    table[Index(StringId::kSettingsHotwordCandidateAddButton)] = "Add";
    table[Index(StringId::kSettingsHotwordCandidateDismissButton)] = "Dismiss";

    // 热词处理（LLM 提炼）
    table[Index(StringId::kHotwordProcessExtracting)] = "Extracting hotwords...";
    table[Index(StringId::kHotwordProcessAdded)] = "Hotwords added: ";
    table[Index(StringId::kHotwordProcessAllDuplicate)] = "No new hotwords (all already exist)";
    table[Index(StringId::kHotwordProcessEmptyResult)] = "No hotwords extracted";
    table[Index(StringId::kHotwordProcessFailed)] = "Hotword extraction failed";
    table[Index(StringId::kHotwordProcessNoKey)] = "LLM API key not configured";

    // 编码器设置节
    table[Index(StringId::kSettingsSectionEncoder)] = "Encoder";
    table[Index(StringId::kSettingsEncoderToArrow)] = "Inject keys on rotate";
    table[Index(StringId::kSettingsEncoderRotationInvert)] = "Invert rotation direction";
    table[Index(StringId::kSettingsEncoderRotateCwKey)] = "Clockwise key";
    table[Index(StringId::kSettingsEncoderRotateCcwKey)] = "Counter-clockwise key";
    table[Index(StringId::kSettingsEncoderRotateFastThreshold)] = "Fast threshold (detents/s)";
    table[Index(StringId::kSettingsEncoderRotateCwFastKey)] = "Fast clockwise key";
    table[Index(StringId::kSettingsEncoderRotateCcwFastKey)] = "Fast counter-clockwise key";
    table[Index(StringId::kSettingsEncoderLedColor)] = "Recording LED color";
    table[Index(StringId::kSettingsEncoderPressAction)] = "Press action";
    table[Index(StringId::kSettingsEncoderDoubleClickAction)] = "Double-click action";
    table[Index(StringId::kSettingsEncoderActionRecording)] = "Recording";
    table[Index(StringId::kSettingsEncoderActionKey)] = "Custom key";
    table[Index(StringId::kSettingsEncoderLedRed)] = "Red";
    table[Index(StringId::kSettingsEncoderLedGreen)] = "Green";
    table[Index(StringId::kSettingsEncoderLedBlue)] = "Blue";
    table[Index(StringId::kSettingsEncoderLedYellow)] = "Yellow";
    table[Index(StringId::kSettingsEncoderLedPurple)] = "Purple";
    table[Index(StringId::kSettingsEncoderLedCyan)] = "Cyan";
    table[Index(StringId::kSettingsEncoderLedWhite)] = "White";
    table[Index(StringId::kSettingsEncoderLedOff)] = "Off";
    table[Index(StringId::kSettingsEncoderInvalidKey)] = "Invalid encoder key syntax (e.g. \"down\", \"ctrl+z\"); the field was not saved.";
    table[Index(StringId::kSettingsEncoderRotateDecideWindow)] = "Decide window (ms)";
    table[Index(StringId::kSettingsEncoderPressKey)] = "Press key";
    table[Index(StringId::kSettingsEncoderDoubleClickKey)] = "Double-click key";
    table[Index(StringId::kMenuEncoderSettings)] = "Encoder settings...";
    table[Index(StringId::kEncoderSettingsTitle)] = "Encoder settings - VS-{0}";
    table[Index(StringId::kEncoderSettingsRestoreDefaults)] = "Restore defaults";
    table[Index(StringId::kMenuInteractionSettings)] = "Device interaction settings...";
    table[Index(StringId::kInteractionSettingsTitle)] = "Device interaction - VS-{0}";
    table[Index(StringId::kMenuRemoteSettings)] = "Remote settings...";
    table[Index(StringId::kRemoteSettingsTitle)] = "Remote settings - RC-{0}";
    table[Index(StringId::kSettingsRemoteGainDb)] = "Gain (dB, -24 to 24)";
    table[Index(StringId::kSettingsRemoteDoubleClickMs)] = "Double-click window (ms, 200 to 600)";
    table[Index(StringId::kRemoteSettingsEffectiveNextConnect)] =
        "Changes take effect on the next connection.";
    table[Index(StringId::kMenuBatteryMonitor)] = "Battery voltage monitor...";
    table[Index(StringId::kBatteryMonitorTitle)] = "Battery Voltage Monitor - VS-{0}";
    table[Index(StringId::kBatteryMonitorStart)] = "Start";
    table[Index(StringId::kBatteryMonitorStop)] = "Stop";
    table[Index(StringId::kBatteryMonitorExportCsv)] = "Export CSV...";
    table[Index(StringId::kBatteryMonitorExportPng)] = "Export PNG...";
    table[Index(StringId::kBatteryMonitorClose)] = "Close";
    table[Index(StringId::kBatteryMonitorStatusIdle)] =
        "Idle. Click Start to run a 60-minute monitoring session (one sample per minute).";
    table[Index(StringId::kBatteryMonitorStatusAnchoring)] = "Synchronizing time anchor...";
    table[Index(StringId::kBatteryMonitorStatusProbing)] = "Probing log baseline...";
    table[Index(StringId::kBatteryMonitorStatusMonitoring)] =
        "Monitoring: cycle {0}/60, {1} points, next sample in {2}";
    table[Index(StringId::kBatteryMonitorStatusFinished)] =
        "Finished: {0} points collected.";
    table[Index(StringId::kBatteryMonitorStatusError)] = "Aborted: {0}";
    table[Index(StringId::kBatteryMonitorWarnUsbPower)] =
        "Tip: keep the device on USB power while monitoring; on battery it powers off "
        "after about 10 minutes idle.";
    table[Index(StringId::kBatteryMonitorErrProbeTimeout)] = "log probe timed out (device not responding)";
    table[Index(StringId::kBatteryMonitorErrDumpTimeout)] = "voltage log export timed out repeatedly";
    table[Index(StringId::kBatteryMonitorErrRestart)] = "device restarted (uptime went backwards)";
    table[Index(StringId::kBatteryMonitorErrDisconnected)] = "device disconnected";
    table[Index(StringId::kBatteryMonitorStatusWaitingReconnect)] =
        "Disconnected; waiting for reconnection to resume (device keeps logging)";
    table[Index(StringId::kBatteryMonitorErrCleared)] = "device log was cleared (total shrank)";
    table[Index(StringId::kBatteryMonitorErrSaveFailed)] = "save failed: {0}";
    table[Index(StringId::kBatteryMonitorSavedTo)] = "Saved: ";
    table[Index(StringId::kBatteryMonitorAxisTime)] = "Time (min)";
    table[Index(StringId::kBatteryMonitorAxisVoltage)] = "Voltage (mV)";
    // {0}=设备ID {1}=点数
    table[Index(StringId::kBatteryMonitorChartTitle)] = "VS-{0} battery voltage ({1} points)";
    table[Index(StringId::kBatteryMonitorUsbAutoOff)] = "Auto power-off on USB (10 min)";
    table[Index(StringId::kHotwordTrimTitle)] = "Hotword Budget Trimmed";
    table[Index(StringId::kHotwordTrimBodyPrefix)] =
        "Hotwords exceed the per-session budget; kept by usage frequency: ";
    table[Index(StringId::kHotwordTrimBodySuffix)] =
        ". The rest are skipped this session (see log for details).";
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
    table[Index(StringId::kSettingsRefineText)] = "精修文本（去除停顿空格、修正标点、清理口头语）";
    table[Index(StringId::kSettingsRefinePrompt)] = "精修提示词";
    table[Index(StringId::kSettingsLaunchAtLogin)] = "Windows 启动时自动运行 VoiceStick";
    table[Index(StringId::kSettingsSelectionHotword)] = "划选文本时显示\"添加到热词\"按钮";
    table[Index(StringId::kSettingsSelectionHotwordHint)] = "在任意应用中划选文本即可快速添加为热词。";
    table[Index(StringId::kSettingsSectionHotwordProcess)] = "热词处理";
    table[Index(StringId::kSettingsHotwordProcessEnable)] = "划词后用 LLM 提炼热词";
    table[Index(StringId::kSettingsHotwordProcessPrompt)] = "提炼提示词";
    table[Index(StringId::kSettingsDebugAudio)] = "保存调试音频文件";
    table[Index(StringId::kSettingsShowImuDebug)] = "显示加速度调试数值";
    table[Index(StringId::kSettingsImuWakeSensitivity)] = "拿起灵敏度";
    table[Index(StringId::kSettingsImuWakeSensitivityLow)] = "低";
    table[Index(StringId::kSettingsImuWakeSensitivityMedium)] = "中";
    table[Index(StringId::kSettingsImuWakeSensitivityHigh)] = "高";
    table[Index(StringId::kSettingsTapToArrow)] = "双击设备按下方向键↓";
    table[Index(StringId::kSettingsTapSensitivity)] = "敲击灵敏度";
    table[Index(StringId::kSettingsAirMouseSensitivityX)] = "左右灵敏度";
    table[Index(StringId::kSettingsAirMouseSensitivityY)] = "上下灵敏度";
    table[Index(StringId::kSettingsOutputTarget)] = "输出目标";
    table[Index(StringId::kSettingsOutputTargetFocusedApp)] = "当前应用";
    table[Index(StringId::kSettingsOutputTargetSubtitle)] = "字幕";
    table[Index(StringId::kSettingsOutputTargetWechatInputMethod)] = "第三方输入法";
    table[Index(StringId::kSettingsWechatHotkey)] = "语音热键";
    table[Index(StringId::kSettingsWechatVirtualMic)] = "虚拟麦克风播放端名称";
    table[Index(StringId::kSettingsWechatAutoSwitch)] = "录音时自动切换默认录音设备";
    table[Index(StringId::kSettingsWechatVirtualMicCapture)] = "虚拟麦克风录音端名称";
    table[Index(StringId::kSettingsTriggerMode)] = "触发方式";
    table[Index(StringId::kSettingsTriggerModeHold)] = "长按式（微信输入法）";
    table[Index(StringId::kSettingsTriggerModeClick)] = "点按式（Typeless 等）";
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
    table[Index(StringId::kSettingsSectionGeneral)] = "通用";
    table[Index(StringId::kSettingsSectionAsr)] = "语音识别";
    table[Index(StringId::kSettingsSectionRefine)] = "文本精修";
    table[Index(StringId::kSettingsSectionOutput)] = "输出";
    table[Index(StringId::kSettingsSectionDevice)] = "设备交互";
    table[Index(StringId::kSettingsSectionSystem)] = "系统";
    table[Index(StringId::kSettingsDeveloperMode)] = "开发者模式（显示全部高级设置）";
    table[Index(StringId::kMenuPairDevice)] = "配对设备...";
    table[Index(StringId::kMenuSettings)] = "设置...";
    table[Index(StringId::kMenuQuit)] = "退出";
    table[Index(StringId::kMenuRelaunchElevated)] = "以管理员身份重启";
    table[Index(StringId::kMenuWebsite)] = "官网";
    table[Index(StringId::kMenuCheckAppUpdates)] = "检查应用更新...";
    table[Index(StringId::kMenuFlashTool)] = "固件烧录工具…";
    table[Index(StringId::kMenuRestoreLastInput)] = "恢复上次输入";
    table[Index(StringId::kMenuInteraction)] = "交互方式";
    table[Index(StringId::kMenuHoldToTalk)] = "按住说话";
    table[Index(StringId::kMenuClickToTalk)] = "点击说话";
    table[Index(StringId::kMenuAutoEnter)] = "粘贴后按回车";
    table[Index(StringId::kMenuLaunchAtLogin)] = "开机自启动";
    table[Index(StringId::kMenuSelectionHotword)] = "划词添加热词";
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
    table[Index(StringId::kMenuUpdateFirmwareFromFile)] = "从本地文件更新固件...";
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
    table[Index(StringId::kStatusAirMouseActive)] = "体感鼠标已开启（侧键退出）";
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
    table[Index(StringId::kPairSavedManualWaiting)] = "已保存 %s，正在等待设备广播...";
    table[Index(StringId::kPairPairingDevice)] = "正在配对 %s...";
    table[Index(StringId::kPairConnectedFinishing)] = "已连接 %s，正在完成配对...";
    table[Index(StringId::kPairPairedDevice)] = "已配对 %s";
    table[Index(StringId::kPairPairedDeviceFirmware)] = "已配对 %s，固件版本 %s";
    table[Index(StringId::kPairTimedOut)] = "配对超时";
    table[Index(StringId::kPairFoundCount)] = "找到 %s 个设备";
    table[Index(StringId::kPairScanningCount)] = "正在扫描（已收到 %s 条广播）";
    table[Index(StringId::kDeviceTypeVoiceStick)] = "语音棒";
    table[Index(StringId::kDeviceTypeXiaomiRemote)] = "小米遥控器";
    table[Index(StringId::kPairXiaomiOsPairing)] = "正在通过 Windows 蓝牙配对…";
    table[Index(StringId::kPairXiaomiBondFailed)] =
        "系统配对失败。请先在 Windows 蓝牙设置中删除并重新添加遥控器，然后重新扫描。";
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
    table[Index(StringId::kOnboardingSkipDeviceConfirm)] =
        "尚未配对设备。可以先完成设置，之后通过托盘菜单的「固件烧录工具…」烧写固件，"
        "再回来配对设备。\n\n是否暂不配对、继续下一步？";
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
    table[Index(StringId::kFirmwareUpdateAdvancedComFlash)] = "高级… COM 口烧录";
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

    // 提权重启
    table[Index(StringId::kRelaunchFailedTitle)] = "提权重启失败";
    table[Index(StringId::kRelaunchFailedPath)] = "无法获取程序路径，请手动右键 VoiceStick.exe 以管理员身份运行。";
    table[Index(StringId::kRelaunchFailedUac)] = "UAC 未确认或失败，请手动右键 VoiceStick.exe 以管理员身份运行。";
    table[Index(StringId::kElevationNeededTitle)] = "需提权运行 VoiceStick";
    table[Index(StringId::kElevationNeededBody)] =
        "%s 以高权限运行，语音输入被系统拦截。右键托盘 -> 以管理员身份重启后重试。";

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

    // 划词添加热词
    table[Index(StringId::kSelectionHotwordButton)] = "添加到热词";
    table[Index(StringId::kSelectionHotwordAddedTitle)] = "已添加热词";
    table[Index(StringId::kSelectionHotwordAddedBody)] = "已添加到热词：";
    table[Index(StringId::kSelectionHotwordDuplicateTitle)] = "热词已存在";
    table[Index(StringId::kSelectionHotwordDuplicateBody)] = "已在热词库中：";
    table[Index(StringId::kSelectionHotwordEmptyTitle)] = "未选中文本";
    table[Index(StringId::kSelectionHotwordEmptyBody)] = "未找到可划选的文本。";
    table[Index(StringId::kSelectionHotwordTooLongTitle)] = "选区过长";
    table[Index(StringId::kSelectionHotwordTooLongBody)] = "选区过长不适合作为热词，已忽略。";
    table[Index(StringId::kHotwordCandidateNotifyTitle)] = "热词候选建议";
    table[Index(StringId::kHotwordCandidateNotifyBodySuffix)] =
        " 反复被精修纠正，可在设置-热词中确认加入。";
    table[Index(StringId::kSettingsHotwordCandidatesLabel)] =
        "候选热词（从精修纠错中自动挖掘，确认后才会加入）：";
    table[Index(StringId::kSettingsHotwordCandidateAddButton)] = "加入";
    table[Index(StringId::kSettingsHotwordCandidateDismissButton)] = "忽略";

    // 热词处理（LLM 提炼）
    table[Index(StringId::kHotwordProcessExtracting)] = "热词提炼中…";
    table[Index(StringId::kHotwordProcessAdded)] = "已添加热词：";
    table[Index(StringId::kHotwordProcessAllDuplicate)] = "没有新热词（提炼结果均已存在）";
    table[Index(StringId::kHotwordProcessEmptyResult)] = "未提炼出热词";
    table[Index(StringId::kHotwordProcessFailed)] = "热词提炼失败";
    table[Index(StringId::kHotwordProcessNoKey)] = "未配置 LLM API Key，无法提炼热词";

    // 编码器设置节
    table[Index(StringId::kSettingsSectionEncoder)] = "编码器";
    table[Index(StringId::kSettingsEncoderToArrow)] = "旋转时注入按键";
    table[Index(StringId::kSettingsEncoderRotationInvert)] = "旋转方向翻转";
    table[Index(StringId::kSettingsEncoderRotateCwKey)] = "顺时针按键";
    table[Index(StringId::kSettingsEncoderRotateCcwKey)] = "逆时针按键";
    table[Index(StringId::kSettingsEncoderRotateFastThreshold)] = "快慢阈值（格/秒）";
    table[Index(StringId::kSettingsEncoderRotateCwFastKey)] = "快速顺时针按键";
    table[Index(StringId::kSettingsEncoderRotateCcwFastKey)] = "快速逆时针按键";
    table[Index(StringId::kSettingsEncoderLedColor)] = "录音灯颜色";
    table[Index(StringId::kSettingsEncoderPressAction)] = "单击动作";
    table[Index(StringId::kSettingsEncoderDoubleClickAction)] = "双击动作";
    table[Index(StringId::kSettingsEncoderActionRecording)] = "录音";
    table[Index(StringId::kSettingsEncoderActionKey)] = "自定义按键";
    table[Index(StringId::kSettingsEncoderLedRed)] = "红";
    table[Index(StringId::kSettingsEncoderLedGreen)] = "绿";
    table[Index(StringId::kSettingsEncoderLedBlue)] = "蓝";
    table[Index(StringId::kSettingsEncoderLedYellow)] = "黄";
    table[Index(StringId::kSettingsEncoderLedPurple)] = "紫";
    table[Index(StringId::kSettingsEncoderLedCyan)] = "青";
    table[Index(StringId::kSettingsEncoderLedWhite)] = "白";
    table[Index(StringId::kSettingsEncoderLedOff)] = "关";
    table[Index(StringId::kSettingsEncoderInvalidKey)] = "编码器按键语法无效（示例：down、ctrl+z），该字段未保存。";
    table[Index(StringId::kSettingsEncoderRotateDecideWindow)] = "判定窗口（毫秒）";
    table[Index(StringId::kSettingsEncoderPressKey)] = "单击按键";
    table[Index(StringId::kSettingsEncoderDoubleClickKey)] = "双击按键";
    table[Index(StringId::kMenuEncoderSettings)] = "编码器设置…";
    table[Index(StringId::kEncoderSettingsTitle)] = "编码器设置 - VS-{0}";
    table[Index(StringId::kEncoderSettingsRestoreDefaults)] = "恢复默认";
    table[Index(StringId::kMenuInteractionSettings)] = "设备交互设置…";
    table[Index(StringId::kInteractionSettingsTitle)] = "设备交互 - VS-{0}";
    table[Index(StringId::kMenuRemoteSettings)] = "遥控器设置…";
    table[Index(StringId::kRemoteSettingsTitle)] = "遥控器设置 - RC-{0}";
    table[Index(StringId::kSettingsRemoteGainDb)] = "增益（dB，-24~24）";
    table[Index(StringId::kSettingsRemoteDoubleClickMs)] = "双击窗口（ms，200~600）";
    table[Index(StringId::kRemoteSettingsEffectiveNextConnect)] = "设置将在下次连接时生效。";
    table[Index(StringId::kMenuBatteryMonitor)] = "电池电压监测…";
    table[Index(StringId::kBatteryMonitorTitle)] = "电池电压监测 - VS-{0}";
    table[Index(StringId::kBatteryMonitorStart)] = "开始监测";
    table[Index(StringId::kBatteryMonitorStop)] = "停止";
    table[Index(StringId::kBatteryMonitorExportCsv)] = "导出 CSV…";
    table[Index(StringId::kBatteryMonitorExportPng)] = "导出 PNG…";
    table[Index(StringId::kBatteryMonitorClose)] = "关闭";
    table[Index(StringId::kBatteryMonitorStatusIdle)] =
        "空闲。点击「开始监测」启动 60 分钟监测（每分钟 1 个采样点）。";
    table[Index(StringId::kBatteryMonitorStatusAnchoring)] = "正在同步时间锚点…";
    table[Index(StringId::kBatteryMonitorStatusProbing)] = "正在探测日志基线…";
    table[Index(StringId::kBatteryMonitorStatusMonitoring)] =
        "监测中：第 {0}/60 周期，{1} 个数据点，下次采集 {2}";
    table[Index(StringId::kBatteryMonitorStatusFinished)] =
        "监测完成：共 {0} 个数据点。";
    table[Index(StringId::kBatteryMonitorStatusError)] = "已中止：{0}";
    table[Index(StringId::kBatteryMonitorWarnUsbPower)] =
        "提示：建议监测期间保持设备 USB 供电；电池供电下设备空闲约 10 分钟会自动关机。";
    table[Index(StringId::kBatteryMonitorErrProbeTimeout)] = "日志基线探测超时（设备无响应）";
    table[Index(StringId::kBatteryMonitorErrDumpTimeout)] = "电压日志导出反复超时";
    table[Index(StringId::kBatteryMonitorErrRestart)] = "设备已重启（uptime 回退）";
    table[Index(StringId::kBatteryMonitorErrDisconnected)] = "设备连接已断开";
    table[Index(StringId::kBatteryMonitorStatusWaitingReconnect)] =
        "设备已断开，等待回连后自动继续（设备端持续记录数据）";
    table[Index(StringId::kBatteryMonitorErrCleared)] = "设备端日志被清空（总长度回退）";
    table[Index(StringId::kBatteryMonitorErrSaveFailed)] = "保存失败：{0}";
    table[Index(StringId::kBatteryMonitorSavedTo)] = "已保存：";
    table[Index(StringId::kBatteryMonitorAxisTime)] = "时间（分钟）";
    table[Index(StringId::kBatteryMonitorAxisVoltage)] = "电压（mV）";
    // {0}=设备ID {1}=点数
    table[Index(StringId::kBatteryMonitorChartTitle)] = "VS-{0} 电池电压监测（{1} 点）";
    table[Index(StringId::kBatteryMonitorUsbAutoOff)] = "供电时10分钟自动关机";
    table[Index(StringId::kHotwordTrimTitle)] = "热词预算裁剪";
    table[Index(StringId::kHotwordTrimBodyPrefix)] = "热词超出单次会话直传预算，已按使用频率优先保留 ";
    table[Index(StringId::kHotwordTrimBodySuffix)] = " 个，其余本次不参与识别（明细见日志）。";
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
