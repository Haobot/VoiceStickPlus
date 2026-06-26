#pragma once

#include "app_config.h"

#include <string>
#include <string_view>

namespace voicestick {

// 稳定语义 key，不要用英文原文做 key，以免文案微调破坏索引。
enum class StringId {
    // 通用
    kOk,
    kCancel,
    kSave,
    kClose,

    // 设置窗口
    kSettingsTitle,
    kSettingsLanguage,
    kSettingsLanguageSystem,
    kSettingsLanguageEnglish,
    kSettingsLanguageChineseSimplified,
    kSettingsProvider,
    kSettingsApiKey,
    kSettingsResourceId,
    kSettingsHotwords,
    kSettingsHotwordsHint,
    kSettingsLlmBaseUrl,
    kSettingsLlmApiKey,
    kSettingsLlmModel,
    kSettingsRefineText,
    kSettingsPromptTone,
    kSettingsLaunchAtLogin,
    kSettingsDebugAudio,
    kSettingsShowImuDebug,
    kSettingsImuWakeSensitivity,
    kSettingsImuWakeSensitivityLow,
    kSettingsImuWakeSensitivityMedium,
    kSettingsImuWakeSensitivityHigh,
    kSettingsShowDeviceWifiInfo,
    kSettingsDeviceWifiSsid,
    kSettingsDeviceWifiIp,
    kSettingsDeviceWifiIdle,
    kSettingsDebugDir,
    kSettingsChooseDir,
    kSettingsOpenConfigFolder,
    kSettingsApplyTrial,
    kSettingsApplyingTrial,
    kSettingsTrialApplied,
    kSettingsTrialPageOpened,
    kSettingsTrialFailedTitle,
    kSettingsTrialFailedMessage,
    kSettingsSaved,
    kSettingsSaveFailed,

    // 托盘菜单
    kMenuPairDevice,
    kMenuSettings,
    kMenuQuit,
    kMenuWebsite,
    kMenuCheckAppUpdates,
    kMenuRestoreLastInput,
    kMenuInteraction,
    kMenuHoldToTalk,
    kMenuClickToTalk,
    kMenuAutoEnter,
    kMenuLaunchAtLogin,
    kMenuOutput,
    kMenuOutputFocusedApp,
    kMenuOutputSubtitle,
    kMenuTranslation,
    kMenuOriginal,
    kMenuThemeColor,
    kMenuThemeSize,
    kMenuOverlayPosition,
    kMenuForgetDevice,
    kMenuUpdateFirmware,
    kMenuFirmwareUpToDate,
    kMenuHotkey,
    kMenuHotkeyEnabled,
    kMenuHotkeyCustom,
    kMenuHotkeyConflictTitle,
    kMenuHotkeyConflictBody,

    // 设备状态
    kStatusNoPairedDevices,
    kStatusScanning,
    kStatusConnected,
    kStatusDisconnected,
    kStatusReady,

    // 配对窗口
    kPairTitle,
    kPairScanning,
    kPairNoDevices,
    kPairConnect,
    kPairConnecting,
    kPairConnected,
    kPairFailed,
    kPairSignal,
    kPairBluetoothAddress,
    kPairManualIdHint,
    kPairButton,
    kPairRetry,
    kPairStillScanning,
    kPairScanFailed,
    kPairAlreadyPaired,
    kPairWaitingForName,
    kPairSelectDeviceWithId,
    kPairSelectDevice,
    kPairEnterManualId,
    kPairSavedManualWaiting,
    kPairPairingDevice,
    kPairConnectedFinishing,
    kPairPairedDevice,
    kPairPairedDeviceFirmware,
    kPairTimedOut,
    kPairFoundCount,
    kPairScanningCount,

    // 引导窗口
    kOnboardingTitle,
    kOnboardingSubtitle,
    kOnboardingGetStarted,
    kOnboardingSetupTitle,
    kOnboardingDeviceStep,
    kOnboardingAsrStep,
    kOnboardingReadyStep,
    kOnboardingBack,
    kOnboardingNext,
    kOnboardingFinish,
    kOnboardingDevicePairedContinue,
    kOnboardingPairDeviceTitle,
    kOnboardingPairDeviceDescription,
    kOnboardingPairDeviceInstruction,
    kOnboardingPairAnotherDevice,
    kOnboardingPairDeviceButton,
    kOnboardingChooseAsr,
    kOnboardingProvider,
    kOnboardingApiKey,
    kOnboardingResource,
    kOnboardingReadyTitle,
    kOnboardingReadyInstruction,
    kOnboardingApplyingTrial,
    kOnboardingTrialApplied,
    kOnboardingTrialOpenFailed,
    kOnboardingTrialPageOpened,
    kOnboardingTrialApplyFailed,
    kOnboardingPairDeviceFirst,
    kOnboardingEnterApiKey,
    kOnboardingDeviceNotPaired,
    kOnboardingDeviceSummary,
    kOnboardingAsrSummary,

    // 固件更新
    kFirmwareUpdateTitle,
    kFirmwareUpdateAvailable,
    kFirmwareUpdateRequired,
    kFirmwareUpdateButton,
    kFirmwareUpdateUpToDate,
    kFirmwareUpdating,
    kFirmwareUpdateSuccess,
    kFirmwareUpdateFailed,
    kFirmwareUpdateFinalizing,
    kFirmwareUpdateTransferring,
    kFirmwareUpdatedTitle,
    kFirmwareUpdatedDetail,
    kFirmwareCancellingTitle,
    kFirmwareCancellingDetail,
    kFirmwareDownloading,
    kFirmwareCheckFailed,
    kFirmwareChecking,
    kFirmwareUpdateAvailableMenu,
    kFirmwareLatestMenu,
    kFirmwareUpdateTo,
    kFirmwareUpdatePromptTitleRequired,
    kFirmwareUpdatePromptTitleAvailable,
    kFirmwareUpdatePromptBody,

    // 热键设置
    kHotkeyTitle,
    kHotkeyEnabled,
    kHotkeyPreset,
    kHotkeyCustomKey,
    kHotkeyCurrent,
    kHotkeyCaptureButton,
    kHotkeyCapturePrompt,
    kHotkeyHint,
    kHotkeyMissingModifier,
    kHotkeyConflictTitle,
    kHotkeyConflictMessage,

    // 云服务提示
    kCloudNeedsAttentionTitle,
    kCloudOpenPageQuestion,

    // 通知
    kNotificationPairedTitle,
    kNotificationManualPairSavedTitle,
    kNotificationManualPairSavedBody,
    kNotificationFirmwareUpdatedTitle,
    kNotificationFirmwareUpdatedBody,

    // 悬浮窗
    kOverlayListening,
    kOverlayThinking,
    kOverlayError,
    kOverlayAsrError,
    kOverlayNoSpeech,

    // 通知
    kNotificationHotkeyConflictTitle,
    kNotificationHotkeyConflictBody,
};

// 返回 UTF-8 本地化文本
std::string Tr(StringId id, UiLanguage language);

// 返回 UTF-16 本地化文本，适用于 Win32 宽字符 API
std::wstring TrW(StringId id, UiLanguage language);

std::string BatteryStatusText(int level_percent, bool charging, bool usb_powered, UiLanguage language);
std::wstring DeviceTitleWithBattery(const std::wstring& title,
                                    int level_percent,
                                    bool charging,
                                    bool usb_powered,
                                    UiLanguage language);

// 检查英文表和中文表是否对所有 StringId 都有非空条目
bool LocalizationTablesAreComplete();

} // namespace voicestick
