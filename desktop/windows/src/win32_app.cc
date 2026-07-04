#include "win32_app.h"

#include "asr_client_win.h"
#include "asr_client_tencent.h"
#include "ble_central_win.h"
#include "localization.h"
#include "log.h"
#include "resource.h"

#include <Shellapi.h>
#include <winsparkle.h>
#include <winrt/base.h>
#include <taskschd.h>
#include <comdef.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <fstream>
#include <stdexcept>

namespace voicestick {

namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kUiDispatchMessage = WM_APP + 2;
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuRestore = 1001;
constexpr UINT kMenuSettings = 1002;
constexpr UINT kMenuQuit = 1005;
constexpr UINT kMenuPairScan = 1006;
constexpr UINT kMenuCheckAppUpdates = 1008;
constexpr UINT kMenuAirMouseTuning = 1007;
constexpr UINT kMenuHoldToTalk = 1009;
constexpr UINT kMenuClickToTalk = 1010;
constexpr UINT kMenuAutoEnter = 1011;
constexpr UINT kMenuLaunchAtLogin = 1014;
constexpr UINT kMenuOutputFocusedApp = 1012;
constexpr UINT kMenuOutputSubtitle = 1013;
constexpr UINT kMenuForgetBase = 2100;
constexpr UINT kMenuForgetEnd = 2199;
constexpr UINT kMenuUpdateFirmwareBase = 2200;
constexpr UINT kMenuUpdateFirmwareEnd = 2299;
constexpr UINT kMenuThemeColorBase = 2300;
constexpr UINT kMenuThemeColorEnd = 2899;
constexpr UINT kMenuThemeSizeBase = 2900;
constexpr UINT kMenuThemeSizeEnd = 3399;
constexpr UINT kMenuOverlayPositionBase = 3400;
constexpr UINT kMenuOverlayPositionEnd = 3999;
constexpr UINT kMenuTranslationBase = 4000;
constexpr UINT kMenuTranslationEnd = 5799;
constexpr UINT kMenuOptionsPerDevice = 24;
constexpr UINT kMenuTranslationsPerDevice = 24;
constexpr UINT kMenuHotkeyEnabled = 5801;
constexpr UINT kMenuHotkeyCustom = 5802;
constexpr UINT kMenuHotkeyBase = 5810;
constexpr UINT kMenuHotkeyEnd = 5899;

struct HotkeyPreset {
    const char* name;
    const wchar_t* display_name;
};

constexpr HotkeyPreset kHotkeyPresets[] = {
    {"Alt+X", L"Alt + X"},
    {"Win+Alt+X", L"Win + Alt + X"},
    {"Ctrl+Alt+X", L"Ctrl + Alt + X"},
};

#ifndef VOICESTICK_APPCAST_URL
#define VOICESTICK_APPCAST_URL "https://78.github.io/voicestick/appcast.xml"
#endif

void LogLine(std::string_view message) {
    voicestick::LogApp(message);
}

std::wstring CurrentExecutableCommand() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    if (length == 0) return {};
    path.resize(length);
    return L"\"" + path + L"\"";
}

std::wstring Utf16FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Utf8FromUtf16(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring FormatText(std::wstring text, std::initializer_list<std::wstring> values) {
    for (const auto& value : values) {
        const auto pos = text.find(L"%s");
        if (pos == std::wstring::npos) break;
        text.replace(pos, 2, value);
    }
    return text;
}

std::string FormatUtf8(std::string text, std::initializer_list<std::string> values) {
    for (const auto& value : values) {
        const auto pos = text.find("%s");
        if (pos == std::string::npos) break;
        text.replace(pos, 2, value);
    }
    return text;
}

std::wstring FirmwareIdentityText(const std::string& hardware, const std::string& version) {
    if (!hardware.empty() && !version.empty()) {
        return Utf16FromUtf8(hardware + " " + version);
    }
    if (!hardware.empty()) {
        return Utf16FromUtf8(hardware);
    }
    if (!version.empty()) {
        return L"Firmware " + Utf16FromUtf8(version);
    }
    return L"Firmware Unknown";
}

constexpr OverlayThemeColor kOverlayThemeColors[] = {
    OverlayThemeColor::kAuto,
    OverlayThemeColor::kWhite,
    OverlayThemeColor::kBlack,
    OverlayThemeColor::kPink,
    OverlayThemeColor::kGreen,
    OverlayThemeColor::kYellow,
    OverlayThemeColor::kBlue,
    OverlayThemeColor::kPurple,
};

constexpr OverlayThemeSize kOverlayThemeSizes[] = {
    OverlayThemeSize::kBig,
    OverlayThemeSize::kMedium,
    OverlayThemeSize::kSmall,
};

constexpr OverlayPosition kOverlayPositions[] = {
    OverlayPosition::kCenter,
    OverlayPosition::kBottomCenter,
    OverlayPosition::kTopLeft,
    OverlayPosition::kTopRight,
    OverlayPosition::kBottomLeft,
    OverlayPosition::kBottomRight,
};

struct TranslationTarget {
    const char* code;
    const wchar_t* name;
};

constexpr TranslationTarget kTranslationTargets[] = {
    {"en", L"English"},
    {"zh-Hans", L"Chinese (Simplified)"},
    {"zh-Hant", L"Chinese (Traditional)"},
    {"ja", L"Japanese"},
    {"ko", L"Korean"},
    {"ru", L"Russian"},
    {"fr", L"French"},
    {"de", L"German"},
    {"es", L"Spanish"},
    {"it", L"Italian"},
    {"pt", L"Portuguese"},
    {"nl", L"Dutch"},
    {"sv", L"Swedish"},
    {"pl", L"Polish"},
    {"tr", L"Turkish"},
    {"ar", L"Arabic"},
    {"hi", L"Hindi"},
    {"id", L"Indonesian"},
    {"vi", L"Vietnamese"},
    {"th", L"Thai"},
};

std::wstring LocalizedThemeColorName(OverlayThemeColor color, UiLanguage language) {
    if (language != UiLanguage::kSimplifiedChinese) return Utf16FromUtf8(OverlayThemeColorDisplayName(color));
    switch (color) {
    case OverlayThemeColor::kAuto: return L"自动";
    case OverlayThemeColor::kBlack: return L"黑色";
    case OverlayThemeColor::kPink: return L"粉色";
    case OverlayThemeColor::kGreen: return L"绿色";
    case OverlayThemeColor::kYellow: return L"黄色";
    case OverlayThemeColor::kBlue: return L"蓝色";
    case OverlayThemeColor::kPurple: return L"紫色";
    case OverlayThemeColor::kWhite:
    default:
        return L"白色";
    }
}

std::wstring LocalizedThemeSizeName(OverlayThemeSize size, UiLanguage language) {
    if (language != UiLanguage::kSimplifiedChinese) return Utf16FromUtf8(OverlayThemeSizeDisplayName(size));
    switch (size) {
    case OverlayThemeSize::kMedium: return L"中";
    case OverlayThemeSize::kSmall: return L"小";
    case OverlayThemeSize::kBig:
    default:
        return L"大";
    }
}

std::wstring LocalizedOverlayPositionName(OverlayPosition position, UiLanguage language) {
    if (language != UiLanguage::kSimplifiedChinese) return Utf16FromUtf8(OverlayPositionDisplayName(position));
    switch (position) {
    case OverlayPosition::kBottomCenter: return L"底部居中";
    case OverlayPosition::kTopLeft: return L"左上";
    case OverlayPosition::kTopRight: return L"右上";
    case OverlayPosition::kBottomLeft: return L"左下";
    case OverlayPosition::kBottomRight: return L"右下";
    case OverlayPosition::kCenter:
    default:
        return L"居中";
    }
}

std::wstring LocalizedTranslationTargetName(const TranslationTarget& target, UiLanguage language) {
    if (language != UiLanguage::kSimplifiedChinese) return target.name;
    const std::string_view code(target.code);
    if (code == "en") return L"英文";
    if (code == "zh-Hans") return L"简体中文";
    if (code == "zh-Hant") return L"繁体中文";
    if (code == "ja") return L"日文";
    if (code == "ko") return L"韩文";
    if (code == "ru") return L"俄文";
    if (code == "fr") return L"法文";
    if (code == "de") return L"德文";
    if (code == "es") return L"西班牙文";
    if (code == "it") return L"意大利文";
    if (code == "pt") return L"葡萄牙文";
    if (code == "nl") return L"荷兰文";
    if (code == "sv") return L"瑞典文";
    if (code == "pl") return L"波兰文";
    if (code == "tr") return L"土耳其文";
    if (code == "ar") return L"阿拉伯文";
    if (code == "hi") return L"印地文";
    if (code == "id") return L"印尼文";
    if (code == "vi") return L"越南文";
    if (code == "th") return L"泰文";
    return target.name;
}

} // namespace

Win32App::Win32App(HINSTANCE instance) : instance_(instance), config_(AppConfig::Load()) {
    LogApp("Config loaded from: " + AppConfig::ConfigPath().string() +
           " portable_mode=" + std::string(config_.portable_mode ? "true" : "false") +
           " provider=" + AsrProviderName(config_.asr_provider));
    if (config_.asr_provider == AsrProvider::kTencent) {
        LogApp("Tencent config appid=" + config_.tencent_appid +
               " secret_id=" + config_.tencent_secret_id.substr(0, 8) + "..." +
               " secret_key_len=" + std::to_string(config_.tencent_secret_key.size()));
    }
    paired_device_ids_ = config_.paired_device_ids;
    for (const auto& entry : config_.paired_devices) {
        if (entry.device_id.empty()) continue;
        if (!entry.hardware.empty() || !entry.firmware_version.empty()) {
            device_info_map_[entry.device_id] = DeviceInfo{
                entry.device_id,
                entry.hardware,
                entry.firmware_version,
            };
        }
    }
}

int Win32App::Run() {
    try {
        LogLine("VoiceStickApp starting");
        ui_thread_id_ = GetCurrentThreadId();
        LogLine("Creating main window");
        if (!CreateWindowInternal()) {
            LogLine("CreateWindowInternal failed");
            return 1;
        }
        LogLine("Main window created");

        RegisterTaskbarMessage();
        AddTrayIcon();
        if (!config_.portable_mode) {
            try {
                SyncLaunchAtLogin();
            } catch (const std::exception& error) {
                LogLine(std::string("Launch at login sync skipped: ") + error.what());
            }
        } else {
            LogLine("Portable mode — skipping launch-at-login registration");
        }

        if (!config_.portable_mode) {
            LogLine("Initializing WinSparkle");
            win_sparkle_set_appcast_url(VOICESTICK_APPCAST_URL);
            win_sparkle_set_automatic_check_for_updates(1);
            win_sparkle_set_update_check_interval(86400);
            win_sparkle_init();
            LogLine("WinSparkle initialized");
        } else {
            LogLine("Portable mode — skipping WinSparkle init");
        }

        LogLine("Creating BLE coordinator");
        auto ble = std::make_unique<BleCentralWin>(config_.paired_device_ids, hwnd_);
        ble_central_ = ble.get();

        // 根据 asr_provider 创建对应的 ASR 客户端
        auto make_asr = [](const AppConfig& cfg) -> std::unique_ptr<AsrClient> {
            LogApp("make_asr: provider=" + AsrProviderName(cfg.asr_provider) +
                   " appid=" + cfg.tencent_appid);
            if (cfg.asr_provider == AsrProvider::kTencent) {
                LogApp("make_asr: creating AsrClientTencent");
                return std::make_unique<AsrClientTencent>(cfg);
            }
            LogApp("make_asr: creating AsrClientWin");
            return std::make_unique<AsrClientWin>(cfg);
        };

        coordinator_ = std::make_unique<VoiceStickCoordinator>(
            config_,
            std::move(ble),
            make_asr(config_),
            this,
            &input_injector_,
            [make_asr](const AppConfig& config) {
                return make_asr(config);
            });
        LogLine("Starting coordinator");
        coordinator_->on_air_mouse_active_changed = [this](bool active) {
            // 有设备进入体感时启动 60Hz 定时器驱动 AirMouseTick；全部退出时停止。
            if (active && !air_mouse_timer_active_) {
                SetTimer(hwnd_, kAirMouseTimerId, kAirMouseTickIntervalMs, nullptr);
                air_mouse_timer_active_ = true;
            } else if (!active && air_mouse_timer_active_) {
                KillTimer(hwnd_, kAirMouseTimerId);
                air_mouse_timer_active_ = false;
            }
        };
        coordinator_->Start();
        LogLine("Coordinator started");

        LogLine("Initializing global hotkey");
        global_hotkey_ = std::make_unique<GlobalHotkeyWin>(hwnd_);
        global_hotkey_->on_pressed = [this] {
            if (coordinator_) coordinator_->HandleGlobalHotkeyPressed();
        };
        global_hotkey_->on_released = [this] {
            if (coordinator_) coordinator_->HandleGlobalHotkeyReleased();
        };
        if (config_.global_hotkey_enabled) {
            if (!global_hotkey_->Register(config_.global_hotkey)) {
                LogLine("Global hotkey registration failed, hotkey conflict or invalid");
                if (config_.debug_audio_cache) {
                    const auto language = EffectiveUiLanguage(config_.ui_language);
                    ShowNotification(Tr(StringId::kNotificationHotkeyConflictTitle, language),
                                     config_.global_hotkey + " " + Tr(StringId::kNotificationHotkeyConflictBody, language));
                }
                SetStatus("Hotkey registration failed: " + config_.global_hotkey + " (conflict)");
            } else {
                LogLine("Global hotkey registered: " + config_.global_hotkey);
                SetStatus("Global hotkey: " + config_.global_hotkey);
            }
        } else {
            LogLine("Global hotkey disabled by config");
        }

        for (const auto& entry : config_.paired_devices) {
            if (entry.bluetooth_address != 0) {
                LogLine("Queueing paired device connect VS-" + entry.device_id);
                coordinator_->ConnectPairedDevice(entry.device_id, entry.bluetooth_address,
                                                 entry.address_kind, entry.name);
            }
        }

        LogLine("Checking onboarding");
        if (!ShowOnboardingIfNeeded()) {
            LogLine("Onboarding did not complete; exiting");
            ShutdownAndQuit();
            win_sparkle_cleanup();
            RemoveTrayIcon();
            return 0;
        }
        LogLine("Startup complete; entering message loop");

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        win_sparkle_cleanup();
        RemoveTrayIcon();
        return static_cast<int>(message.wParam);
    } catch (const winrt::hresult_error& error) {
        LogLine("Fatal WinRT error: hr=" + std::to_string(static_cast<std::int32_t>(error.code())) +
                " message=" + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        LogLine(std::string("Fatal exception: ") + error.what());
    } catch (...) {
        LogLine("Fatal unknown exception");
    }
    RemoveTrayIcon();
    return 1;
}

void Win32App::SetStatus(const std::string& status) {
    DispatchToUi([this, status] {
        status_ = status;
        RebuildTooltip();
    });
}

void Win32App::SetConnectedDevices(const std::vector<ConnectedDevice>& devices) {
    DispatchToUi([this, devices] {
        connected_devices_ = devices;
        if (pair_device_dialog_) pair_device_dialog_->SetConnectedDevices(devices);
        UpdateTrayIcon();
    });
}

void Win32App::SetDeviceInfo(const DeviceInfo& info) {
    DispatchToUi([this, info] {
        LogLine("SetDeviceInfo VS-" + info.device_id +
                " hardware=" + (info.hardware.empty() ? "<empty>" : info.hardware) +
                " firmware=" + (info.firmware_version.empty() ? "<empty>" : info.firmware_version));
        device_info_map_[info.device_id] = info;
        if (pair_device_dialog_) {
            pair_device_dialog_->SetDeviceInfo(info);
        }
    });
}

void Win32App::SetDeviceBattery(const std::string& device_id, int level_percent,
                                 bool charging, bool usb_powered) {
    DispatchToUi([this, device_id, level_percent, charging, usb_powered] {
        LogLine("SetDeviceBattery VS-" + device_id +
                " level=" + std::to_string(level_percent) +
                " charging=" + std::to_string(charging) +
                " usb=" + std::to_string(usb_powered));
        device_battery_map_[device_id] = {level_percent, charging, usb_powered};
        UpdateTrayIcon();
    });
}

void Win32App::SetFirmwareInfo(const std::map<std::string, DeviceFirmwareInfo>& info_by_device_id) {
    DispatchToUi([this, info_by_device_id] {
        firmware_info_map_ = info_by_device_id;
    });
}

void Win32App::HandlePairingCompleted(const std::string& device_id, std::optional<DeviceInfo> info) {
    LogLine("Pairing completed VS-" + device_id +
            (info && !info->firmware_version.empty()
                 ? " firmware=" + info->firmware_version
                 : " firmware=<unknown>"));
    if (pending_pairing_entry_ && pending_pairing_entry_->device_id == device_id) {
        if (info) {
            pending_pairing_entry_->hardware = info->hardware;
            pending_pairing_entry_->firmware_version = info->firmware_version;
        }
        config_.SavePairedDevice(*pending_pairing_entry_);
        pending_pairing_entry_.reset();
        paired_device_ids_ = config_.paired_device_ids;
        if (coordinator_) coordinator_->ConfirmPairedDeviceIds(config_.paired_device_ids);
        if (coordinator_) coordinator_->CheckFirmwareAfterPairing(device_id);
        LogLine("Confirmed paired device VS-" + device_id);
    }
    std::string detail = "VS-" + device_id + " paired";
    if (info && !info->hardware.empty()) detail += " (" + info->hardware + ")";
    if (info && !info->firmware_version.empty()) {
        detail += ", firmware " + info->firmware_version;
    }
    ShowNotification(Tr(StringId::kNotificationPairedTitle, EffectiveUiLanguage(config_.ui_language)), detail);
    RebuildTooltip();
}

void Win32App::SetPairingError(const std::string& device_id, const std::string& message) {
    DispatchToUi([this, device_id, message] {
        if (pending_pairing_entry_ && pending_pairing_entry_->device_id == device_id) {
            pending_pairing_entry_.reset();
        }
        if (pair_device_dialog_) pair_device_dialog_->SetPairingError(device_id, message);
        LogLine("Pairing error VS-" + device_id + ": " + message);
    });
}

void Win32App::ShowFirmwareUpdatePrompt(const std::string& device_id,
                                        const std::string& current_version,
                                        const std::string& latest_version,
                                        bool is_below_minimum) {
    DispatchToUi([this, device_id, current_version, latest_version, is_below_minimum] {
        const auto language = EffectiveUiLanguage(config_.ui_language);
        const auto message = FormatText(TrW(StringId::kFirmwareUpdatePromptBody, language),
                                        {Utf16(device_id), Utf16(current_version), Utf16(latest_version)});
        const int result = MessageBoxW(
            hwnd_,
            message.c_str(),
            TrW(is_below_minimum ? StringId::kFirmwareUpdatePromptTitleRequired
                                 : StringId::kFirmwareUpdatePromptTitleAvailable,
                language).c_str(),
            MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON1);
        if (result == IDYES) {
            StartFirmwareUpdate(device_id);
        }
    });
}

void Win32App::SetPairedDeviceIds(const std::vector<std::string>& ids) {
    DispatchToUi([this, ids] {
        paired_device_ids_ = ids;
    });
}

void Win32App::SetHasRecoverableInput(bool has_recoverable_input) {
    DispatchToUi([this, has_recoverable_input] {
        has_recoverable_input_ = has_recoverable_input;
    });
}

void Win32App::ShowListening(const std::optional<std::string>& device_id) {
    DispatchToUi([this, device_id] {
        ApplyOverlayStyle(std::optional<std::string>(device_id));
        if (overlay_) overlay_->ShowListening();
    });
}

void Win32App::ShowPartial(const std::string& text, const std::optional<std::string>& device_id) {
    DispatchToUi([this, text, device_id] {
        ApplyOverlayStyle(std::optional<std::string>(device_id));
        if (overlay_) overlay_->ShowPartial(text);
    });
}

void Win32App::AppendPartial(const std::string& text, const std::optional<std::string>& device_id) {
    DispatchToUi([this, text, device_id] {
        ApplyOverlayStyle(std::optional<std::string>(device_id));
        if (overlay_) overlay_->AppendPartial(text);
    });
}

void Win32App::ShowRefining(const std::string& text, const std::optional<std::string>& device_id) {
    DispatchToUi([this, text, device_id] {
        ApplyOverlayStyle(std::optional<std::string>(device_id));
        if (overlay_) overlay_->ShowRefining(text);
    });
}

void Win32App::ShowFinalCountdown(const std::string& text,
                                  const std::optional<std::string>& device_id,
                                  std::function<void()> on_complete) {
    DispatchToUi([this, text, device_id, on_complete = std::move(on_complete)]() mutable {
        ApplyOverlayStyle(device_id);
        if (overlay_) overlay_->ShowFinalCountdown(text, std::move(on_complete));
    });
}

void Win32App::ShowPausedFinal(const std::string& text, const std::optional<std::string>& device_id) {
    DispatchToUi([this, text, device_id] {
        ApplyOverlayStyle(device_id);
        if (overlay_) overlay_->ShowPausedFinal(text);
    });
}

void Win32App::ShowError(const std::string& text,
                         const std::optional<std::string>& device_id,
                         std::function<void()> on_complete) {
    DispatchToUi([this, text, device_id, on_complete = std::move(on_complete)]() mutable {
        ApplyOverlayStyle(device_id);
        if (overlay_) overlay_->ShowError(text, std::move(on_complete));
    });
}

void Win32App::ShowCloudUpgrade(const std::string& message,
                                const std::string& url,
                                const std::optional<std::string>& device_id) {
    DispatchToUi([this, message, url, device_id] {
        ApplyOverlayStyle(device_id);
        auto show_dialog = [this, message, url] {
            const auto language = EffectiveUiLanguage(config_.ui_language);
            const auto text = Utf16(message) + L"\n\n" + TrW(StringId::kCloudOpenPageQuestion, language);
            const int result = MessageBoxW(hwnd_, text.c_str(),
                                           TrW(StringId::kCloudNeedsAttentionTitle, language).c_str(),
                                           MB_ICONINFORMATION | MB_YESNO | MB_DEFBUTTON1);
            if (result == IDYES) {
                const auto wide_url = Utf16(url);
                ShellExecuteW(hwnd_, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        };
        if (overlay_) {
            overlay_->Hide(std::move(show_dialog));
        } else {
            show_dialog();
        }
    });
}

void Win32App::HideOverlay(std::function<void()> on_hidden) {
    DispatchToUi([this, on_hidden = std::move(on_hidden)]() mutable {
        if (overlay_) overlay_->Hide(std::move(on_hidden));
    });
}

void Win32App::ShowSubtitle(const std::string& text,
                            const std::string& device_id,
                            OverlayThemeColor color) {
    DispatchToUi([this, text, device_id, color] {
        if (subtitles_) subtitles_->ShowSubtitle(text, device_id, color);
    });
}

void Win32App::HideSubtitles() {
    DispatchToUi([this] {
        if (subtitles_) subtitles_->HideAll();
    });
}

LRESULT CALLBACK Win32App::WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* app = reinterpret_cast<Win32App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        app = reinterpret_cast<Win32App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        return TRUE;
    }
    return app ? app->HandleMessage(message, w_param, l_param) : DefWindowProcW(hwnd, message, w_param, l_param);
}


LRESULT Win32App::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == kUiDispatchMessage) {
        std::unique_ptr<std::function<void()>> action(
            reinterpret_cast<std::function<void()>*>(w_param));
        if (action && *action) (*action)();
        return 0;
    }
    if (message == BleCentralWin::WM_BLE_DISPATCH) {
        if (ble_central_) ble_central_->ProcessDispatchedCallbacks();
        return 0;
    }
    if (global_hotkey_ && global_hotkey_->HandleMessage(message, w_param, l_param)) {
        return 0;
    }
    if (message == taskbar_created_message_) {
        AddTrayIcon();
        return 0;
    }
    if (message == kTrayCallbackMessage) {
        const auto event = static_cast<UINT>(LOWORD(l_param));
        if (event == NIN_POPUPOPEN || event == WM_MOUSEMOVE) {
            RebuildTooltip();
            RequestConnectedBatteryStatus();
            return 0;
        }
        if (event == WM_RBUTTONUP || event == WM_LBUTTONUP ||
            event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT) {
            ShowTrayMenu();
            return 0;
        }
    }
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kMenuRestore:
            if (coordinator_) coordinator_->RestoreLastInputConfirmation();
            return 0;
        case kMenuPairScan:
            ShowPairDeviceDialog();
            return 0;
        case kMenuCheckAppUpdates:
            if (!config_.portable_mode) {
                win_sparkle_check_update_with_ui();
            }
            return 0;
        case kMenuHoldToTalk:
            config_.interaction_mode = InteractionMode::kHoldToTalk;
            SaveInputOptions();
            return 0;
        case kMenuClickToTalk:
            config_.interaction_mode = InteractionMode::kClickToTalk;
            SaveInputOptions();
            return 0;
        case kMenuAutoEnter:
            config_.auto_enter = !config_.auto_enter;
            SaveInputOptions();
            return 0;
        case kMenuLaunchAtLogin:
            config_.launch_at_login = !config_.launch_at_login;
            SaveInputOptions();
            return 0;
        case kMenuOutputFocusedApp:
            config_.default_output_profile.target = OutputTarget::kFocusedApp;
            SaveInputOptions();
            return 0;
        case kMenuOutputSubtitle:
            config_.default_output_profile.target = OutputTarget::kSubtitle;
            SaveInputOptions();
            return 0;
        case kMenuSettings:
            ShowSettings();
            return 0;
        case kMenuAirMouseTuning:
            ShowAirMouseTuning();
            return 0;
        case kMenuQuit:
            ShutdownAndQuit();
            return 0;
        case kMenuHotkeyEnabled:
            config_.global_hotkey_enabled = !config_.global_hotkey_enabled;
            SaveInputOptions();
            if (config_.global_hotkey_enabled) {
                SetStatus("Global hotkey enabled: " + config_.global_hotkey);
            } else {
                SetStatus("Global hotkey disabled");
            }
            return 0;
        case kMenuHotkeyCustom: {
            auto dialog = std::make_unique<HotkeySettingsDialog>(
                instance_, hwnd_, EffectiveUiLanguage(config_.ui_language));
            dialog->on_hotkey_confirmed = [this](const std::string& hotkey) {
                config_.global_hotkey = hotkey;
                config_.global_hotkey_enabled = true;
                SaveInputOptions();
                SetStatus("Global hotkey set to: " + hotkey);
            };
            dialog->Show();
            return 0;
        }
        default: {
            UINT cmd = LOWORD(w_param);
            if (cmd >= kMenuHotkeyBase && cmd <= kMenuHotkeyEnd) {
                std::size_t index = cmd - kMenuHotkeyBase;
                if (index < sizeof(kHotkeyPresets) / sizeof(kHotkeyPresets[0])) {
                    config_.global_hotkey = kHotkeyPresets[index].name;
                    config_.global_hotkey_enabled = true;
                    SaveInputOptions();
                    SetStatus("Global hotkey set to: " + config_.global_hotkey);
                }
                return 0;
            }
            if (cmd >= kMenuForgetBase && cmd <= kMenuForgetEnd) {
                std::size_t index = cmd - kMenuForgetBase;
                if (index < paired_device_ids_.size() && coordinator_) {
                    auto device_id = paired_device_ids_[index];
                    coordinator_->RemovePairedDevice(device_id);
                    config_.RemovePairedDevice(device_id);
                    LogLine("Forgot device VS-" + device_id);
                }
            } else if (cmd >= kMenuUpdateFirmwareBase && cmd <= kMenuUpdateFirmwareEnd) {
                std::size_t index = cmd - kMenuUpdateFirmwareBase;
                if (index < paired_device_ids_.size()) {
                    StartFirmwareUpdate(paired_device_ids_[index]);
                }
            } else if (cmd >= kMenuThemeColorBase && cmd <= kMenuThemeColorEnd) {
                const std::size_t offset = cmd - kMenuThemeColorBase;
                const std::size_t index = offset / kMenuOptionsPerDevice;
                const std::size_t color_index = offset % kMenuOptionsPerDevice;
                if (index < paired_device_ids_.size() &&
                    color_index < (sizeof(kOverlayThemeColors) / sizeof(kOverlayThemeColors[0]))) {
                    SaveDeviceThemeColor(paired_device_ids_[index], kOverlayThemeColors[color_index]);
                }
            } else if (cmd >= kMenuThemeSizeBase && cmd <= kMenuThemeSizeEnd) {
                const std::size_t offset = cmd - kMenuThemeSizeBase;
                const std::size_t index = offset / kMenuOptionsPerDevice;
                const std::size_t size_index = offset % kMenuOptionsPerDevice;
                if (index < paired_device_ids_.size() &&
                    size_index < (sizeof(kOverlayThemeSizes) / sizeof(kOverlayThemeSizes[0]))) {
                    SaveDeviceThemeSize(paired_device_ids_[index], kOverlayThemeSizes[size_index]);
                }
            } else if (cmd >= kMenuOverlayPositionBase && cmd <= kMenuOverlayPositionEnd) {
                const std::size_t offset = cmd - kMenuOverlayPositionBase;
                const std::size_t index = offset / kMenuOptionsPerDevice;
                const std::size_t position_index = offset % kMenuOptionsPerDevice;
                if (index < paired_device_ids_.size() &&
                    position_index < (sizeof(kOverlayPositions) / sizeof(kOverlayPositions[0]))) {
                    SaveDeviceOverlayPosition(paired_device_ids_[index], kOverlayPositions[position_index]);
                }
            } else if (cmd >= kMenuTranslationBase && cmd <= kMenuTranslationEnd) {
                const std::size_t offset = cmd - kMenuTranslationBase;
                const std::size_t index = offset / kMenuTranslationsPerDevice;
                const std::size_t translation_index = offset % kMenuTranslationsPerDevice;
                if (index < paired_device_ids_.size()) {
                    auto profile = config_.OutputProfileForDevice(paired_device_ids_[index]);
                    if (translation_index == 0) {
                        profile.transform = TextTransform::kOriginal;
                    } else {
                        const std::size_t target_index = translation_index - 1;
                        if (target_index < std::size(kTranslationTargets)) {
                            profile.transform = TextTransform::kTranslate;
                            profile.translation_target = kTranslationTargets[target_index].code;
                        }
                    }
                    SaveDeviceOutputProfile(paired_device_ids_[index], profile);
                }
            }
            return 0;
        }
        }
        return 0;
    case WM_TIMER:
        if (w_param == kAirMouseTimerId && coordinator_) {
            coordinator_->AirMouseTick();
            return 0;
        }
        break;
    case WM_DESTROY:
        pair_device_dialog_.reset();
        ble_central_ = nullptr;
        coordinator_.reset();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, w_param, l_param);
    }
    return DefWindowProcW(hwnd_, message, w_param, l_param);
}

void Win32App::ShutdownAndQuit() {
    if (is_shutting_down_) return;
    is_shutting_down_ = true;
    if (global_hotkey_) {
        global_hotkey_->Unregister();
    }
    pair_device_dialog_.reset();
    if (coordinator_) coordinator_->Shutdown();
    DestroyWindow(hwnd_);
}

void Win32App::DispatchToUi(std::function<void()> action) {
    if (!action) return;
    if (ui_thread_id_ == 0 || GetCurrentThreadId() == ui_thread_id_) {
        action();
        return;
    }

    auto* heap_action = new std::function<void()>(std::move(action));
    if (!PostMessageW(hwnd_, kUiDispatchMessage, reinterpret_cast<WPARAM>(heap_action), 0)) {
        std::unique_ptr<std::function<void()>> cleanup(heap_action);
    }
}



bool Win32App::CreateWindowInternal() {
    LogLine("Registering window class");
    WNDCLASSW wc{};
    wc.lpfnWndProc = Win32App::WindowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"VoiceStickWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_VOICESTICK_APP));
    RegisterClassW(&wc);
    LogLine("Creating hidden app window");
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"VoiceStick", 0, 0, 0, 0, 0,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;

    LogLine("Creating overlay window object");
    overlay_ = std::make_unique<OverlayWindow>(instance_, hwnd_);
    LogLine("Creating subtitle window object");
    subtitles_ = std::make_unique<SubtitleWindow>(instance_, hwnd_);
    return true;
}

void Win32App::AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_VOICESTICK_TRAY), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    if (!data.hIcon) data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_VOICESTICK_APP));
    if (!data.hIcon) data.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"VoiceStick - Not connected");
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        LogLine("Shell_NotifyIcon NIM_ADD failed: " + std::to_string(GetLastError()));
        return;
    }

    data.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
        LogLine("Shell_NotifyIcon NIM_SETVERSION failed: " + std::to_string(GetLastError()));
        return;
    }
    LogLine("Shell_NotifyIcon registered");
}

void Win32App::RemoveTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void Win32App::ShowTrayMenu() {
    RequestConnectedBatteryStatus();
    HMENU menu = CreatePopupMenu();
    const UiLanguage language = EffectiveUiLanguage(config_.ui_language);
    if (has_recoverable_input_) {
        AppendMenuW(menu, MF_STRING, kMenuRestore,
                    TrW(StringId::kMenuRestoreLastInput, language).c_str());
    }
    AppendMenuW(menu, MF_STRING, kMenuPairScan, TrW(StringId::kMenuPairDevice, language).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (paired_device_ids_.empty() && connected_devices_.empty()) {
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0,
                    TrW(StringId::kStatusNoPairedDevices, language).c_str());
    }

    auto find_connected = [&](const std::string& id) -> const ConnectedDevice* {
        for (const auto& device : connected_devices_) {
            if (device.id == id) return &device;
        }
        return nullptr;
    };

    for (std::size_t i = 0; i < paired_device_ids_.size() && i < 100; ++i) {
        const auto& id = paired_device_ids_[i];
        const auto* connected = find_connected(id);
        const std::string title = connected
            ? (connected->name.empty() ? "VS-" + id : connected->name)
            : "VS-" + id;

        HMENU submenu = CreatePopupMenu();

        // Status
        auto status_text = TrW(connected ? StringId::kStatusConnected : StringId::kStatusScanning, language);
        AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, status_text.c_str());

        // Hardware + firmware version
        auto info_it = device_info_map_.find(id);
        auto firmware_it = firmware_info_map_.find(id);
        const std::string hardware =
            info_it != device_info_map_.end() && !info_it->second.hardware.empty()
                ? info_it->second.hardware
                : (firmware_it != firmware_info_map_.end() ? firmware_it->second.hardware : std::string{});
        const std::string firmware_version =
            info_it != device_info_map_.end() && !info_it->second.firmware_version.empty()
                ? info_it->second.firmware_version
                : (firmware_it != firmware_info_map_.end() ? firmware_it->second.current_version : std::string{});
        auto identity_text = FirmwareIdentityText(hardware, firmware_version);
        AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, identity_text.c_str());

        HMENU theme_menu = CreatePopupMenu();
        const auto theme_it = config_.device_theme_colors.find(id);
        const auto current_theme = theme_it != config_.device_theme_colors.end()
            ? theme_it->second
            : DefaultOverlayThemeColor();
        for (std::size_t color_index = 0;
             color_index < sizeof(kOverlayThemeColors) / sizeof(kOverlayThemeColors[0]);
             ++color_index) {
            const auto color = kOverlayThemeColors[color_index];
            AppendMenuW(
                theme_menu,
                MF_STRING | (current_theme == color ? MF_CHECKED : 0),
                kMenuThemeColorBase + static_cast<UINT>(i * kMenuOptionsPerDevice + color_index),
                LocalizedThemeColorName(color, language).c_str());
        }
        AppendMenuW(submenu, MF_POPUP, reinterpret_cast<UINT_PTR>(theme_menu),
                    TrW(StringId::kMenuThemeColor, language).c_str());

        HMENU size_menu = CreatePopupMenu();
        const auto size_it = config_.device_theme_sizes.find(id);
        const auto current_size = size_it != config_.device_theme_sizes.end()
            ? size_it->second
            : OverlayThemeSize::kBig;
        for (std::size_t size_index = 0;
             size_index < sizeof(kOverlayThemeSizes) / sizeof(kOverlayThemeSizes[0]);
             ++size_index) {
            const auto sz = kOverlayThemeSizes[size_index];
            AppendMenuW(
                size_menu,
                MF_STRING | (current_size == sz ? MF_CHECKED : 0),
                kMenuThemeSizeBase + static_cast<UINT>(i * kMenuOptionsPerDevice + size_index),
                LocalizedThemeSizeName(sz, language).c_str());
        }
        AppendMenuW(submenu, MF_POPUP, reinterpret_cast<UINT_PTR>(size_menu),
                    TrW(StringId::kMenuThemeSize, language).c_str());

        HMENU position_menu = CreatePopupMenu();
        const auto position_it = config_.device_overlay_positions.find(id);
        const auto current_position = position_it != config_.device_overlay_positions.end()
            ? position_it->second
            : DefaultOverlayPosition();
        for (std::size_t position_index = 0;
             position_index < sizeof(kOverlayPositions) / sizeof(kOverlayPositions[0]);
             ++position_index) {
            const auto position = kOverlayPositions[position_index];
            AppendMenuW(
                position_menu,
                MF_STRING | (current_position == position ? MF_CHECKED : 0),
                kMenuOverlayPositionBase + static_cast<UINT>(i * kMenuOptionsPerDevice + position_index),
                LocalizedOverlayPositionName(position, language).c_str());
        }
        AppendMenuW(submenu, MF_POPUP, reinterpret_cast<UINT_PTR>(position_menu),
                    TrW(StringId::kMenuOverlayPosition, language).c_str());

        HMENU translation_menu = CreatePopupMenu();
        const auto current_profile = config_.OutputProfileForDevice(id);
        AppendMenuW(
            translation_menu,
            MF_STRING | (current_profile.transform == TextTransform::kOriginal ? MF_CHECKED : 0),
            kMenuTranslationBase + static_cast<UINT>(i * kMenuTranslationsPerDevice),
            TrW(StringId::kMenuOriginal, language).c_str());
        AppendMenuW(translation_menu, MF_SEPARATOR, 0, nullptr);
        for (std::size_t target_index = 0; target_index < std::size(kTranslationTargets); ++target_index) {
            const auto& target = kTranslationTargets[target_index];
            const auto checked = current_profile.transform == TextTransform::kTranslate &&
                                 current_profile.translation_target == target.code;
            auto title = language == UiLanguage::kSimplifiedChinese
                ? std::wstring(L"翻译为 ") + LocalizedTranslationTargetName(target, language)
                : std::wstring(L"Translate to ") + LocalizedTranslationTargetName(target, language);
            AppendMenuW(
                translation_menu,
                MF_STRING | (checked ? MF_CHECKED : 0),
                kMenuTranslationBase + static_cast<UINT>(i * kMenuTranslationsPerDevice + target_index + 1),
                title.c_str());
        }
        AppendMenuW(submenu, MF_POPUP, reinterpret_cast<UINT_PTR>(translation_menu),
                    TrW(StringId::kMenuTranslation, language).c_str());

        if (firmware_it != firmware_info_map_.end()) {
            const auto& firmware = firmware_it->second;
            if (firmware.is_checking) {
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0,
                            TrW(StringId::kFirmwareChecking, language).c_str());
            } else if (!firmware.error_message.empty()) {
                auto error_text = TrW(StringId::kFirmwareCheckFailed, language);
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, error_text.c_str());
            } else if (firmware.update_available && !firmware.latest_version.empty()) {
                auto update_text = FormatText(TrW(StringId::kFirmwareUpdateAvailableMenu, language),
                                              {Utf16(firmware.latest_version)});
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, update_text.c_str());
                auto update_action = FormatText(TrW(StringId::kFirmwareUpdateTo, language),
                                                {Utf16(firmware.latest_version)});
                AppendMenuW(submenu,
                            connected ? MF_STRING : (MF_STRING | MF_DISABLED),
                            kMenuUpdateFirmwareBase + static_cast<UINT>(i),
                            update_action.c_str());
            } else if (!firmware.latest_version.empty() && !firmware.current_version.empty()) {
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0,
                            TrW(StringId::kMenuFirmwareUpToDate, language).c_str());
            } else if (!firmware.latest_version.empty()) {
                auto latest_text = FormatText(TrW(StringId::kFirmwareLatestMenu, language),
                                             {Utf16(firmware.latest_version)});
                AppendMenuW(submenu, MF_STRING | MF_DISABLED, 0, latest_text.c_str());
                auto update_action = FormatText(TrW(StringId::kFirmwareUpdateTo, language),
                                                {Utf16(firmware.latest_version)});
                AppendMenuW(submenu,
                            connected ? MF_STRING : (MF_STRING | MF_DISABLED),
                            kMenuUpdateFirmwareBase + static_cast<UINT>(i),
                            update_action.c_str());
            }
        }

        AppendMenuW(submenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(submenu, MF_STRING, kMenuForgetBase + static_cast<UINT>(i),
                    TrW(StringId::kMenuForgetDevice, language).c_str());

        std::wstring menu_title = Utf16(title);
        const auto battery_it = device_battery_map_.find(id);
        if (battery_it != device_battery_map_.end()) {
            const auto& battery = battery_it->second;
            menu_title = DeviceTitleWithBattery(menu_title,
                                                battery.level_percent,
                                                battery.charging,
                                                battery.usb_powered,
                                                language);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), menu_title.c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU interaction_menu = CreatePopupMenu();
    AppendMenuW(interaction_menu,
                MF_STRING | (config_.interaction_mode == InteractionMode::kHoldToTalk ? MF_CHECKED : 0),
                kMenuHoldToTalk,
                TrW(StringId::kMenuHoldToTalk, language).c_str());
    AppendMenuW(interaction_menu,
                MF_STRING | (config_.interaction_mode == InteractionMode::kClickToTalk ? MF_CHECKED : 0),
                kMenuClickToTalk,
                TrW(StringId::kMenuClickToTalk, language).c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(interaction_menu),
                TrW(StringId::kMenuInteraction, language).c_str());

    HMENU hotkey_menu = CreatePopupMenu();
    AppendMenuW(hotkey_menu,
                MF_STRING | (config_.global_hotkey_enabled ? MF_CHECKED : 0),
                kMenuHotkeyEnabled,
                TrW(StringId::kMenuHotkeyEnabled, language).c_str());
    AppendMenuW(hotkey_menu, MF_SEPARATOR, 0, nullptr);
    bool is_custom_hotkey = true;
    for (std::size_t i = 0; i < sizeof(kHotkeyPresets) / sizeof(kHotkeyPresets[0]); ++i) {
        const auto& preset = kHotkeyPresets[i];
        const auto checked = config_.global_hotkey_enabled && config_.global_hotkey == preset.name;
        if (config_.global_hotkey == preset.name) {
            is_custom_hotkey = false;
        }
        AppendMenuW(
            hotkey_menu,
            MF_STRING | (checked ? MF_CHECKED : 0),
            kMenuHotkeyBase + static_cast<UINT>(i),
            preset.display_name);
    }
    AppendMenuW(hotkey_menu, MF_SEPARATOR, 0, nullptr);
    std::wstring custom_menu_text = TrW(StringId::kMenuHotkeyCustom, language);
    UINT custom_menu_flags = MF_STRING;
    if (is_custom_hotkey && !config_.global_hotkey.empty()) {
        custom_menu_text = TrW(StringId::kMenuHotkeyCustom, language) + L" " + Utf16FromUtf8(config_.global_hotkey);
        if (config_.global_hotkey_enabled) {
            custom_menu_flags |= MF_CHECKED;
        }
    }
    AppendMenuW(hotkey_menu, custom_menu_flags, kMenuHotkeyCustom, custom_menu_text.c_str());
    if (global_hotkey_ && !global_hotkey_->IsRegistered()) {
        AppendMenuW(hotkey_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hotkey_menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                    TrW(StringId::kMenuHotkeyConflictTitle, language).c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hotkey_menu),
                TrW(StringId::kMenuHotkey, language).c_str());

    HMENU output_menu = CreatePopupMenu();
    AppendMenuW(output_menu,
                MF_STRING | (config_.default_output_profile.target == OutputTarget::kFocusedApp ? MF_CHECKED : 0),
                kMenuOutputFocusedApp,
                TrW(StringId::kMenuOutputFocusedApp, language).c_str());
    AppendMenuW(output_menu,
                MF_STRING | (config_.default_output_profile.target == OutputTarget::kSubtitle ? MF_CHECKED : 0),
                kMenuOutputSubtitle,
                TrW(StringId::kMenuOutputSubtitle, language).c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(output_menu),
                TrW(StringId::kMenuOutput, language).c_str());

    AppendMenuW(menu,
                MF_STRING | (config_.auto_enter ? MF_CHECKED : 0),
                kMenuAutoEnter,
                TrW(StringId::kMenuAutoEnter, language).c_str());
    if (!config_.portable_mode) {
        AppendMenuW(menu,
                    MF_STRING | (config_.launch_at_login ? MF_CHECKED : 0),
                    kMenuLaunchAtLogin,
                    TrW(StringId::kMenuLaunchAtLogin, language).c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, TrW(StringId::kMenuSettings, language).c_str());
    AppendMenuW(menu, MF_STRING, kMenuAirMouseTuning, L"体感鼠标调参（热调参）");
    if (!config_.portable_mode) {
        AppendMenuW(menu, MF_STRING, kMenuCheckAppUpdates,
                    TrW(StringId::kMenuCheckAppUpdates, language).c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, TrW(StringId::kMenuQuit, language).c_str());
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void Win32App::SyncLaunchAtLogin() {
    if (config_.portable_mode) {
        LogLine("Portable mode — skipping launch-at-login sync");
        return;
    }
    // exe 清单要求 requireAdministrator，HKCU\...\Run 启动提权程序会被 UAC 阻挡，
    // 因此自启改用任务计划程序：登录触发 + RunLevel=Highest（等价管理员），由系统拉起。
    constexpr wchar_t kTaskName[] = L"VoiceStickAutoStart";
    try {
        using namespace winrt;
        // 主线程已在 main.cc 以 STA 初始化 COM，这里必须用同模型；
        // 无参 init_apartment() 默认 MTA，与已存在的 STA 冲突会抛 RPC_E_CHANGED_MODE，
        // 导致任务注册整体失败、登录后不自启。同模型重复 init 只返回 S_FALSE。
        init_apartment(apartment_type::single_threaded);
        // 用 COM 任务计划程序 API（taskschd.h）注册/删除任务。
        com_ptr<ITaskService> service;
        check_hresult(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(service.put())));
        check_hresult(service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()));

        com_ptr<ITaskFolder> root_folder;
        check_hresult(service->GetFolder(_bstr_t(L"\\"), root_folder.put()));

        if (!config_.launch_at_login) {
            const HRESULT hr = root_folder->DeleteTask(_bstr_t(kTaskName), 0);
            if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
                check_hresult(hr);
            }
            LogLine("Launch at login disabled (task deleted)");
            return;
        }

        const auto command = CurrentExecutableCommand();
        if (command.empty()) {
            throw std::runtime_error("failed to resolve executable path");
        }

        com_ptr<ITaskDefinition> definition;
        check_hresult(service->NewTask(0, definition.put()));

        com_ptr<ITriggerCollection> triggers;
        check_hresult(definition->get_Triggers(triggers.put()));
        com_ptr<ITrigger> trigger;
        check_hresult(triggers->Create(TASK_TRIGGER_LOGON, trigger.put()));
        com_ptr<ILogonTrigger> logon_trigger;
        check_hresult(trigger->QueryInterface(IID_PPV_ARGS(logon_trigger.put())));

        com_ptr<IActionCollection> actions;
        check_hresult(definition->get_Actions(actions.put()));
        com_ptr<IAction> action;
        check_hresult(actions->Create(TASK_ACTION_EXEC, action.put()));
        com_ptr<IExecAction> exec_action;
        check_hresult(action->QueryInterface(IID_PPV_ARGS(exec_action.put())));
        // CurrentExecutableCommand 返回带引号的路径，ExecAction 的 Path 不接受引号，
        // 取去掉外层引号后的纯路径作为可执行文件路径。
        std::wstring exe_path = command;
        if (exe_path.size() >= 2 && exe_path.front() == L'"' && exe_path.back() == L'"') {
            exe_path = exe_path.substr(1, exe_path.size() - 2);
        }
        check_hresult(exec_action->put_Path(_bstr_t(exe_path.c_str())));

        com_ptr<ITaskSettings> settings;
        check_hresult(definition->get_Settings(settings.put()));
        check_hresult(settings->put_StartWhenAvailable(VARIANT_TRUE));

        com_ptr<IRegistrationInfo> reg_info;
        check_hresult(definition->get_RegistrationInfo(reg_info.put()));
        check_hresult(reg_info->put_Author(_bstr_t(L"VoiceStick")));

        com_ptr<IRegisteredTask> registered;
        check_hresult(root_folder->RegisterTaskDefinition(
            _bstr_t(kTaskName), definition.get(), TASK_CREATE_OR_UPDATE,
            _variant_t(), _variant_t(),
            TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(), registered.put()));
        LogLine("Launch at login enabled (scheduled task registered)");
    } catch (const winrt::hresult_error& error) {
        LogLine("Launch at login sync failed: hr=" +
                std::to_string(static_cast<std::int32_t>(error.code())) +
                " msg=" + winrt::to_string(error.message()));
        throw std::runtime_error("failed to sync launch-at-login scheduled task");
    }
}

void Win32App::SaveInputOptions() {
    try {
        config_.Save();
        SyncLaunchAtLogin();
        if (coordinator_) coordinator_->UpdateConfig(config_);
        if (global_hotkey_) {
            global_hotkey_->Unregister();
            if (config_.global_hotkey_enabled) {
                if (global_hotkey_->Register(config_.global_hotkey)) {
                    LogLine("Global hotkey registered: " + config_.global_hotkey);
                    SetStatus("Global hotkey: " + config_.global_hotkey);
                } else {
                    LogLine("Global hotkey registration failed: " + config_.global_hotkey);
                    if (config_.debug_audio_cache) {
                        const auto language = EffectiveUiLanguage(config_.ui_language);
                        ShowNotification(Tr(StringId::kNotificationHotkeyConflictTitle, language),
                                         config_.global_hotkey + " " + Tr(StringId::kNotificationHotkeyConflictBody, language));
                    }
                    SetStatus("Hotkey registration failed: " + config_.global_hotkey + " (conflict)");
                }
            } else {
                SetStatus("Global hotkey disabled");
                LogLine("Global hotkey disabled");
            }
        }
        LogLine("Input options saved");
    } catch (const std::exception& error) {
        LogLine(std::string("Input options save failed: ") + error.what());
        SetStatus("Input save failed");
    }
}

void Win32App::SaveDeviceThemeColor(const std::string& device_id, OverlayThemeColor color) {
    try {
        if (color == DefaultOverlayThemeColor()) {
            config_.device_theme_colors.erase(device_id);
        } else {
            config_.device_theme_colors[device_id] = color;
        }
        config_.Save();
        ApplyOverlayStyle(device_id);
        LogLine("Theme color saved VS-" + device_id + "=" + OverlayThemeColorName(color));
    } catch (const std::exception& error) {
        LogLine(std::string("Theme color save failed: ") + error.what());
        SetStatus("Theme save failed");
    }
}

void Win32App::SaveDeviceThemeSize(const std::string& device_id, OverlayThemeSize size) {
    try {
        if (size == OverlayThemeSize::kBig) {
            config_.device_theme_sizes.erase(device_id);
        } else {
            config_.device_theme_sizes[device_id] = size;
        }
        config_.Save();
        ApplyOverlayStyle(device_id);
        LogLine("Theme size saved VS-" + device_id + "=" + OverlayThemeSizeName(size));
    } catch (const std::exception& error) {
        LogLine(std::string("Theme size save failed: ") + error.what());
        SetStatus("Theme size save failed");
    }
}

void Win32App::SaveDeviceOverlayPosition(const std::string& device_id, OverlayPosition position) {
    try {
        if (position == DefaultOverlayPosition()) {
            config_.device_overlay_positions.erase(device_id);
        } else {
            config_.device_overlay_positions[device_id] = position;
        }
        config_.Save();
        ApplyOverlayStyle(device_id);
        LogLine("Overlay position saved VS-" + device_id + "=" + OverlayPositionName(position));
    } catch (const std::exception& error) {
        LogLine(std::string("Overlay position save failed: ") + error.what());
        SetStatus("Position save failed");
    }
}

void Win32App::SaveDeviceOutputProfile(const std::string& device_id, OutputProfile profile) {
    try {
        profile.target = config_.default_output_profile.target;
        OutputProfile default_profile = config_.default_output_profile;
        default_profile.target = profile.target;
        if (profile.transform == default_profile.transform &&
            profile.translation_target == default_profile.translation_target) {
            config_.device_output_profiles.erase(device_id);
        } else {
            config_.device_output_profiles[device_id] = profile;
        }
        config_.Save();
        if (coordinator_) coordinator_->UpdateConfig(config_);
        LogLine("Output profile saved VS-" + device_id + "=" +
                TextTransformName(profile.transform) + ":" + profile.translation_target);
    } catch (const std::exception& error) {
        LogLine(std::string("Output profile save failed: ") + error.what());
        SetStatus("Output save failed");
    }
}

void Win32App::ApplyOverlayStyle(const std::optional<std::string>& device_id) {
    if (!overlay_) return;
    OverlayThemeColor color = DefaultOverlayThemeColor();
    OverlayThemeSize size = OverlayThemeSize::kBig;
    OverlayPosition position = DefaultOverlayPosition();
    if (device_id.has_value()) {
        if (auto color_it = config_.device_theme_colors.find(*device_id);
            color_it != config_.device_theme_colors.end()) {
            color = color_it->second;
        }
        if (auto size_it = config_.device_theme_sizes.find(*device_id);
            size_it != config_.device_theme_sizes.end()) {
            size = size_it->second;
        }
        if (auto position_it = config_.device_overlay_positions.find(*device_id);
            position_it != config_.device_overlay_positions.end()) {
            position = position_it->second;
        }
    }
    overlay_->SetThemeColor(color);
    overlay_->SetThemeSize(size);
    overlay_->SetPosition(position);
}

void Win32App::RebuildTooltip() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    const UiLanguage language = EffectiveUiLanguage(config_.ui_language);
    std::wstring tip;
    if (connected_devices_.empty()) {
        tip = L"VoiceStick - " + TrW(StringId::kStatusDisconnected, language);
    } else {
        bool first = true;
        for (const auto& device : connected_devices_) {
            if (!first) tip += L", ";
            first = false;
            std::wstring device_text = Utf16(device.name.empty() ? "VS-" + device.id : device.name);
            const auto it = device_battery_map_.find(device.id);
            if (it != device_battery_map_.end()) {
                device_text = DeviceTitleWithBattery(device_text,
                                                     it->second.level_percent,
                                                     it->second.charging,
                                                     it->second.usb_powered,
                                                     language);
            }
            tip += device_text;
        }
    }
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Win32App::UpdateTrayIcon() {
    if (!hwnd_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON;

    HICON icon = nullptr;
    if (connected_devices_.empty()) {
        icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TRAY_DISCONNECTED));
    } else {
        int min_level = 100;
        bool charging = false;
        for (const auto& device : connected_devices_) {
            const auto it = device_battery_map_.find(device.id);
            if (it != device_battery_map_.end()) {
                min_level = std::min(min_level, it->second.level_percent);
                if (it->second.charging) charging = true;
            }
        }
        if (charging) {
            icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TRAY_BATTERY_CHARGING));
        } else if (min_level >= 75) {
            icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TRAY_BATTERY_100));
        } else if (min_level >= 50) {
            icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TRAY_BATTERY_75));
        } else if (min_level >= 25) {
            icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TRAY_BATTERY_50));
        } else if (min_level > 0) {
            icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TRAY_BATTERY_25));
        } else {
            icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TRAY_BATTERY_0));
        }
    }
    if (!icon) {
        icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_VOICESTICK_TRAY));
    }
    data.hIcon = icon;
    Shell_NotifyIconW(NIM_MODIFY, &data);
    if (icon) DestroyIcon(icon);
    RebuildTooltip();
}

void Win32App::RequestConnectedBatteryStatus() {
    if (!ble_central_ || connected_devices_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (last_battery_status_request_ != std::chrono::steady_clock::time_point{} &&
        now - last_battery_status_request_ < std::chrono::seconds(1)) {
        return;
    }
    last_battery_status_request_ = now;
    ble_central_->RequestBatteryStatus(std::nullopt);
}

void Win32App::RegisterTaskbarMessage() {
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
}

bool Win32App::ShowOnboardingIfNeeded() {
    if (!NeedsOnboarding(config_)) return true;
    if (!ShowOnboarding()) {
        LogLine("Initial onboarding cancelled; exiting");
        return false;
    }
    return true;
}

bool Win32App::ShowOnboarding() {
    OnboardingDialog dialog(instance_, hwnd_, config_);
    dialog.on_pair_device_requested = [this] {
        ShowPairDeviceDialog();
    };
    dialog.on_config_completed = [this](AppConfig new_config) {
        config_ = std::move(new_config);
        paired_device_ids_ = config_.paired_device_ids;
        if (coordinator_) coordinator_->UpdateConfig(config_);
        RebuildTooltip();
        LogLine("Onboarding completed");
    };
    return dialog.Show();
}

void Win32App::ShowPairDeviceDialog() {
    pair_device_dialog_ = std::make_unique<PairDeviceDialog>(
        instance_, hwnd_, EffectiveUiLanguage(config_.ui_language), config_.paired_device_ids,
        [this](std::string device_id, std::uint64_t bluetooth_address,
               BluetoothAddressKind address_kind, std::string name) {
            PairDevice(device_id, bluetooth_address, address_kind, name);
        },
        [this](std::string device_id, std::optional<DeviceInfo> info) {
            HandlePairingCompleted(device_id, std::move(info));
        });
    pair_device_dialog_->SetManualPairHandler([this](std::string device_id) {
        PairDeviceByManualId(device_id);
    });
    pair_device_dialog_->on_pair_timeout = [this](std::string device_id) {
        pending_pairing_entry_.reset();
        if (coordinator_) coordinator_->CancelPendingConnect(device_id);
        LogLine("Pairing timed out VS-" + device_id);
    };
    pair_device_dialog_->Show();
}

void Win32App::ShowSettings() {
    if (!settings_dialog_) {
        settings_dialog_ = std::make_unique<SettingsDialog>(instance_, hwnd_, config_);
        settings_dialog_->on_config_changed = [this](AppConfig new_config) {
            config_ = std::move(new_config);
            SaveInputOptions();
            RebuildTooltip();
            LogLine("Settings saved");
        };
    }
    settings_dialog_->Show();
}

void Win32App::ShowAirMouseTuning() {
    if (!air_mouse_tuning_window_ || !air_mouse_tuning_window_->IsOpen()) {
        air_mouse_tuning_window_ = std::make_unique<AirMouseTuningWindow>(
            instance_, hwnd_,
            coordinator_ ? coordinator_->GetAirMouseParamsForTuning() : AirMouseParams{});
        air_mouse_tuning_window_->on_params_changed = [this](const AirMouseTuningState& state) {
            if (coordinator_) coordinator_->UpdateAirMouseParams(state.ToParams());
        };
        air_mouse_tuning_window_->on_save_requested = [this](const AirMouseTuningState& state) {
            config_.air_mouse_sensitivity_x = state.sensitivity_x;
            config_.air_mouse_sensitivity_y = state.sensitivity_y;
            config_.air_mouse_tau = state.tau;
            config_.air_mouse_invert_y = state.invert_y;
            config_.air_mouse_curve_low_thresh = state.curve.low_thresh;
            config_.air_mouse_curve_high_thresh = state.curve.high_thresh;
            config_.air_mouse_curve_low_factor = state.curve.low_factor;
            config_.air_mouse_curve_high_factor = state.curve.high_factor;
            config_.air_mouse_neutral_deadzone = state.neutral_deadzone;
            config_.Save();
            if (coordinator_) coordinator_->UpdateConfig(config_);
            LogLine("Air mouse tuning saved");
        };
    }
    air_mouse_tuning_window_->Show();
}

void Win32App::StartFirmwareUpdate(const std::string& device_id) {
    if (!coordinator_) return;
    auto firmware_it = firmware_info_map_.find(device_id);
    const std::string version = firmware_it != firmware_info_map_.end()
                                    ? firmware_it->second.latest_version
                                    : std::string();
    firmware_update_dialog_ = std::make_unique<FirmwareUpdateDialog>(
        instance_, hwnd_, EffectiveUiLanguage(config_.ui_language), version.empty() ? "latest" : version);
    firmware_update_dialog_->on_cancel = [this] {
        if (coordinator_) coordinator_->CancelFirmwareUpdate();
    };
    firmware_update_dialog_->Show();
    coordinator_->UpdateFirmwareFromLatest(
        device_id,
        [this](FirmwareUpdateProgress progress) {
            DispatchToUi([this, progress] {
                if (firmware_update_dialog_) firmware_update_dialog_->UpdateProgress(progress);
            });
        },
        [this](bool success, std::string message) {
            DispatchToUi([this, success, message] {
                if (firmware_update_dialog_) firmware_update_dialog_->Finish(success, message);
                if (success) {
                    const auto language = EffectiveUiLanguage(config_.ui_language);
                    ShowNotification(Tr(StringId::kNotificationFirmwareUpdatedTitle, language),
                                     Tr(StringId::kNotificationFirmwareUpdatedBody, language));
                }
            });
        });
}

void Win32App::PairDevice(const std::string& device_id, std::uint64_t bluetooth_address,
                          BluetoothAddressKind address_kind, const std::string& name) {
    if (coordinator_) {
        pending_pairing_entry_ = PairedDeviceEntry{device_id, bluetooth_address, address_kind, name};
        coordinator_->ConnectPairedDevice(device_id, bluetooth_address, address_kind, name);
        LogLine("Pairing device VS-" + device_id);
    }
}

void Win32App::PairDeviceByManualId(const std::string& device_id) {
    config_.SavePairedDeviceInfo(device_id, {}, {});
    pending_pairing_entry_.reset();
    paired_device_ids_ = config_.paired_device_ids;
    if (coordinator_) {
        coordinator_->ConfirmPairedDeviceIds(config_.paired_device_ids);
        coordinator_->ReconnectPairedDevices();
    }
    const auto language = EffectiveUiLanguage(config_.ui_language);
    ShowNotification(Tr(StringId::kNotificationManualPairSavedTitle, language),
                     FormatUtf8(Tr(StringId::kNotificationManualPairSavedBody, language), {device_id}));
    RebuildTooltip();
    LogLine("Manual pairing saved VS-" + device_id);
}

void Win32App::ShowNotification(const std::string& title, const std::string& body) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    const auto title_w = Utf16(title);
    const auto body_w = Utf16(body);
    wcsncpy_s(data.szInfoTitle, title_w.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, body_w.c_str(), _TRUNCATE);
    data.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

std::wstring Win32App::Utf16(const std::string& text) const {
    return Utf16FromUtf8(text);
}

} // namespace voicestick
