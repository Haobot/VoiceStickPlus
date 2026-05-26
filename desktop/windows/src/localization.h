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
    kSettingsPromptTone,
    kSettingsDebugAudio,
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

    // 引导窗口
    kOnboardingTitle,
    kOnboardingSubtitle,
    kOnboardingGetStarted,

    // 固件更新
    kFirmwareUpdateTitle,
    kFirmwareUpdateAvailable,
    kFirmwareUpdateRequired,
    kFirmwareUpdateButton,
    kFirmwareUpdateUpToDate,
    kFirmwareUpdating,
    kFirmwareUpdateSuccess,
    kFirmwareUpdateFailed,

    // 热键设置
    kHotkeyTitle,
    kHotkeyEnabled,
    kHotkeyPreset,
    kHotkeyCustomKey,

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

// 检查英文表和中文表是否对所有 StringId 都有非空条目
bool LocalizationTablesAreComplete();

} // namespace voicestick
