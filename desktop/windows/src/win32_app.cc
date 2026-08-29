#include "win32_app.h"

#include "asr_client_win.h"
#include "asr_client_tencent.h"
#include "ble_central_win.h"
#include "hotword_extractor.h"
#include "localization.h"
#include "log.h"
#include "resource.h"

#include <Shellapi.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <winsparkle.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <filesystem>
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
constexpr UINT kMenuRelaunchElevated = 1015;
constexpr UINT kMenuSelectionHotword = 1016;
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
constexpr UINT kMenuUpdateFirmwareFromFileBase = 5900;
constexpr UINT kMenuUpdateFirmwareFromFileEnd = 5999;
// 设备级编码器设置入口：每设备一项（kMenuEncoderSettingsBase + 设备索引）。
constexpr UINT kMenuEncoderSettingsBase = 6000;
constexpr UINT kMenuEncoderSettingsEnd = 6199;
// 设备级电池电压监测入口：每设备一项（kMenuBatteryMonitorBase + 设备索引，仅连接设备显示）。
constexpr UINT kMenuBatteryMonitorBase = 6400;
constexpr UINT kMenuBatteryMonitorEnd = 6599;
// 设备级交互设置入口：每设备一项（kMenuInteractionSettingsBase + 设备索引）。
constexpr UINT kMenuInteractionSettingsBase = 6200;
constexpr UINT kMenuInteractionSettingsEnd = 6399;
constexpr UINT kMenuOptionsPerDevice = 24;
constexpr UINT kMenuTranslationsPerDevice = 24;
constexpr UINT kMenuHotkeyEnabled = 5801;
constexpr UINT kMenuHotkeyCustom = 5802;
constexpr UINT kMenuHotkeyBase = 5810;
constexpr UINT kMenuHotkeyEnd = 5899;
// COM 口固件烧录工具入口（VoiceStickFlash.exe），菜单 ID 新段 7000 起。
constexpr UINT kMenuFlashTool = 7000;

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
#define VOICESTICK_APPCAST_URL "https://haobot.github.io/VoiceStickPlus/appcast.xml"
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

// 探测前台窗口所属进程是否高于本进程完整性：asInvoker（Medium）对 High 进程
// OpenProcess(PROCESS_QUERY_INFORMATION) 返回 ERROR_ACCESS_DENIED，借此判断 UIPI 隔离。
// 进程名用 Toolhelp32 快照获取（不依赖对目标进程二次 OpenProcess，免受其 DACL 影响）。
class Win32ForegroundProcessProbe : public IForegroundProcessProbe {
public:
    bool IsForegroundHigherIntegrity(std::wstring& process_name) override {
        const HWND foreground = GetForegroundWindow();
        if (!foreground) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        if (pid == 0) return false;
        // QUERY_INFORMATION(0x0400) 对同会话同级进程默认放行；失败且 ACCESS_DENIED 提示对方更高 IL。
        const HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (handle) {
            CloseHandle(handle);
            return false;
        }
        if (GetLastError() != ERROR_ACCESS_DENIED) return false;
        process_name = ProcessExeNameByPid(pid);
        return true;
    }

private:
    static std::wstring ProcessExeNameByPid(DWORD pid) {
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return L"(unknown)";
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        std::wstring name = L"(unknown)";
        if (Process32FirstW(snap, &entry)) {
            do {
                if (entry.th32ProcessID == pid) {
                    name = entry.szExeFile;
                    break;
                }
            } while (Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
        return name;
    }
};

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

// 拉起 VoiceStickFlash.exe（COM 口固件烧录工具）：与 VoiceStick.exe 同级目录，
// 开发构建在 build-x64，MSI 安装在 INSTALLFOLDER。未安装时给出明确提示。
void LaunchFlashToolExe(HWND owner) {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    if (length == 0) return;
    path.resize(length);
    const auto flash_exe = std::filesystem::path(path).parent_path() / L"VoiceStickFlash.exe";
    std::error_code ec;
    if (!std::filesystem::exists(flash_exe, ec)) {
        MessageBoxW(owner,
                    L"未找到 VoiceStickFlash.exe（应与 VoiceStick.exe 同目录）。\n"
                    L"MSI 安装版自带该工具；便携版暂不包含。",
                    L"VoiceStick", MB_OK | MB_ICONWARNING);
        return;
    }
    ShellExecuteW(owner, L"open", flash_exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
        // 根据配置启用划词监测（默认关闭）。
        SyncSelectionHotword();

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
        coordinator_->on_encoder_rotate_pending_changed = [this](bool pending) {
            // 慢速注入挂起时启动 30ms 定时器驱动 EncoderRotateTick 到期冲刷；清空后停止。
            if (pending && !encoder_rotate_pending_timer_active_) {
                SetTimer(hwnd_, kEncoderRotatePendingTimerId, kEncoderRotatePendingTickMs, nullptr);
                encoder_rotate_pending_timer_active_ = true;
            } else if (!pending && encoder_rotate_pending_timer_active_) {
                KillTimer(hwnd_, kEncoderRotatePendingTimerId);
                encoder_rotate_pending_timer_active_ = false;
            }
        };
        // power_log 分片路由到电池电压监测窗口（UI 线程回调，窗口未开时丢弃）。
        coordinator_->on_power_log_fragment = [this](std::string device_id,
                                                     PowerLogFragment fragment) {
            if (battery_monitor_dialog_) {
                battery_monitor_dialog_->OnPowerLogFragment(device_id, fragment);
            }
        };
        // 供电态（USB）自动关机开关状态：缓存最新值 + 路由到监测窗口勾选框。
        coordinator_->on_power_mgmt_state = [this](std::string device_id, bool usb_auto_off) {
            usb_auto_off_state_[device_id] = usb_auto_off;
            if (battery_monitor_dialog_) {
                battery_monitor_dialog_->OnPowerMgmtState(device_id, usb_auto_off);
            }
        };
        // 注入前台进程完整性探测：asInvoker 实例在微信等高权限前台按下设备键时气泡提醒提权。
        coordinator_->SetForegroundProbe(std::make_unique<Win32ForegroundProcessProbe>());
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
        // 电池电压监测窗口：监测中设备断连立即中止（避免干等导出超时）。
        if (battery_monitor_dialog_) {
            for (const auto& dev : devices) {
                if (battery_monitor_dialog_->IsSameDevice(dev.id)) return;
            }
            battery_monitor_dialog_->NotifyAllDisconnected();
        }
        // 命令行 --ota 自启动场景：连上设备后触发 pending 请求。
        if (pending_ota_request_ && !connected_devices_.empty()) {
            auto req = std::move(*pending_ota_request_);
            pending_ota_request_.reset();
            StartOtaFromFile(req.file_path, req.device_id);
        }
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

void Win32App::SetDeviceEncoderPresent(const std::string& device_id, bool present) {
    DispatchToUi([this, device_id, present] {
        LogLine("SetDeviceEncoderPresent VS-" + device_id +
                " encoder_present=" + (present ? "true" : "false"));
        // device_info 可能因 MTU 截断解析失败而缺条目，这里按 device_id 兜底建条目。
        auto& info = device_info_map_[device_id];
        if (info.device_id.empty()) info.device_id = device_id;
        info.encoder_present = present;
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
    if (message == WM_COPYDATA) {
        const auto* cds = reinterpret_cast<COPYDATASTRUCT*>(l_param);
        if (cds && cds->dwData == kOtaCopyDataId && cds->lpData && cds->cbData > 0) {
            // payload 格式："<path>\n<device_id>"，device_id 可缺。
            std::string payload(static_cast<const char*>(cds->lpData), cds->cbData);
            // 去末尾 \0（SendMessage 跨进程 cbData 含终止符）。
            const auto nul = payload.find('\0');
            if (nul != std::string::npos) payload.resize(nul);
            std::string path;
            std::optional<std::string> device_id;
            const auto nl = payload.find('\n');
            if (nl == std::string::npos) {
                path = payload;
            } else {
                path = payload.substr(0, nl);
                device_id = payload.substr(nl + 1);
            }
            if (!path.empty()) {
                DispatchToUi([this, path, device_id] {
                    StartOtaFromFile(path, device_id);
                });
            }
        }
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
        case kMenuFlashTool:
            LaunchFlashToolExe(hwnd_);
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
        case kMenuSelectionHotword:
            config_.selection_hotword_enabled = !config_.selection_hotword_enabled;
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
        case kMenuRelaunchElevated:
            RelaunchElevatedAndQuit();
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
            } else if (cmd >= kMenuUpdateFirmwareFromFileBase &&
                       cmd <= kMenuUpdateFirmwareFromFileEnd) {
                std::size_t index = cmd - kMenuUpdateFirmwareFromFileBase;
                if (index < paired_device_ids_.size()) {
                    StartFirmwareUpdateFromFile(paired_device_ids_[index]);
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
            } else if (cmd >= kMenuEncoderSettingsBase && cmd <= kMenuEncoderSettingsEnd) {
                std::size_t index = cmd - kMenuEncoderSettingsBase;
                if (index < paired_device_ids_.size()) {
                    ShowEncoderSettingsDialog(paired_device_ids_[index]);
                }
            } else if (cmd >= kMenuBatteryMonitorBase && cmd <= kMenuBatteryMonitorEnd) {
                std::size_t index = cmd - kMenuBatteryMonitorBase;
                if (index < paired_device_ids_.size()) {
                    ShowBatteryMonitorDialog(paired_device_ids_[index]);
                }
            } else if (cmd >= kMenuInteractionSettingsBase && cmd <= kMenuInteractionSettingsEnd) {
                std::size_t index = cmd - kMenuInteractionSettingsBase;
                if (index < paired_device_ids_.size()) {
                    ShowInteractionSettingsDialog(paired_device_ids_[index]);
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
        if (w_param == kEncoderRotatePendingTimerId && coordinator_) {
            coordinator_->EncoderRotateTick();
            return 0;
        }
        if (w_param == kResumeRestartTimerId) {
            KillTimer(hwnd_, kResumeRestartTimerId);
            LogLine("resume timer fired: restarting BLE after power resume");
            if (ble_central_) ble_central_->RestartForResume();
            return 0;
        }
        break;
    case WM_POWERBROADCAST:
        // 休眠/睡眠恢复后 BluetoothLEAdvertisementWatcher 会静默失效：仍报告
        // Started 却不再投递广告包。延迟 1.5s 等蓝牙无线电就绪后彻底重启扫描
        // 与会话。SetTimer 对同一 id 重复设置会重置计时器，自动去抖连续事件；
        // 非 resume 类电源事件放行给 DefWindowProcW。
        if (w_param == PBT_APMRESUMEAUTOMATIC ||
            w_param == PBT_APMRESUMESUSPEND ||
            w_param == PBT_APMRESUMECRITICAL) {
            LogLine(std::string("power broadcast: resume event=") +
                    std::to_string(w_param) + "; scheduling BLE restart");
            // 休眠期间 ASR 保活 WebSocket 底层 TCP 已断，但 AsrClientWin 状态机仍认为
            // kReady，唤醒后首次录音会复用死连接致握手失败。立即标记失效，下次录音
            // 强制重新握手（与 BLE 重启不同，ASR 不依赖无线电就绪，无需延迟）。
            if (coordinator_) {
                LogLine("power resume: invalidating ASR connection");
                coordinator_->InvalidateAsrConnection();
            }
            SetTimer(hwnd_, kResumeRestartTimerId, kResumeRestartDelayMs, nullptr);
            return TRUE;
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

void Win32App::RelaunchElevatedAndQuit() {
    // 取自身 exe 纯路径（不带引号，ShellExecuteW lpFile 需纯路径）。
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    if (length == 0) {
        LogLine("RelaunchElevatedAndQuit: GetModuleFileNameW failed err=" + std::to_string(GetLastError()));
        const auto language = EffectiveUiLanguage(config_.ui_language);
        ShowNotification(Tr(StringId::kRelaunchFailedTitle, language),
                         Tr(StringId::kRelaunchFailedPath, language));
        return;
    }
    path.resize(length);
    // runas 触发 UAC 提权；--relaunch 参数让新实例跳过单例立即退出、改为等待旧实例释放
    // 单例 Mutex 后再接管（否则旧实例尚未退出、新实例拿到 ALREADY_EXISTS 被赶走，新旧都没了）。
    const HINSTANCE inst = ShellExecuteW(hwnd_, L"runas", path.c_str(), L"--relaunch", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(inst) <= 32) {
        LogLine("RelaunchElevatedAndQuit: ShellExecuteW runas failed code=" +
                std::to_string(static_cast<int>(reinterpret_cast<INT_PTR>(inst))));
        const auto language = EffectiveUiLanguage(config_.ui_language);
        ShowNotification(Tr(StringId::kRelaunchFailedTitle, language),
                         Tr(StringId::kRelaunchFailedUac, language));
        return;
    }
    ShutdownAndQuit();
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
    LogLine("Creating selection hotword manager");
    selection_hotword_manager_ = std::make_unique<SelectionHotwordManager>(instance_, hwnd_);
    if (selection_hotword_manager_) {
        selection_hotword_manager_->SetLanguage(
            EffectiveUiLanguage(config_.ui_language));
        // on_add_hotword 回调：规范化后写入 config_.asr_hotwords，去重后保存并通知。
        selection_hotword_manager_->on_add_hotword =
            [this](const std::string& text) {
                if (text.empty()) {
                    const auto lang = EffectiveUiLanguage(config_.ui_language);
                    ShowNotification(Tr(StringId::kSelectionHotwordEmptyTitle, lang),
                                     Tr(StringId::kSelectionHotwordEmptyBody, lang));
                    return;
                }
                // 热词处理：长文送 LLM 提炼，只把提炼结果写入热词表。
                if (config_.hotword_process_enabled) {
                    ProcessHotwordWithLlm(text);
                    return;
                }
                const auto lang = EffectiveUiLanguage(config_.ui_language);
                auto& hotwords = config_.asr_hotwords;
                if (std::find(hotwords.begin(), hotwords.end(), text) != hotwords.end()) {
                    ShowNotification(
                        Tr(StringId::kSelectionHotwordDuplicateTitle, lang),
                        Tr(StringId::kSelectionHotwordDuplicateBody, lang) + text);
                    return;
                }
                hotwords.push_back(text);
                try {
                    config_.SavePreservingDiskCredentials();
                } catch (const std::exception& e) {
                    LogLine(std::string("Save config on hotword add failed: ") + e.what());
                }
                if (coordinator_) coordinator_->UpdateConfig(config_);
                ShowNotification(
                    Tr(StringId::kSelectionHotwordAddedTitle, lang),
                    Tr(StringId::kSelectionHotwordAddedBody, lang) + text);
            };
    }
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

    // 体感鼠标模式激活时在顶部提示，避免用户不知情下主键变鼠标左键误判"按下没反应"。
    if (coordinator_) {
        bool any_air_mouse = false;
        for (const auto& dev : connected_devices_) {
            if (coordinator_->IsAirMouseActive(dev.id)) { any_air_mouse = true; break; }
        }
        if (any_air_mouse) {
            AppendMenuW(menu, MF_STRING | MF_DISABLED, 0,
                        TrW(StringId::kStatusAirMouseActive, language).c_str());
        }
    }

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

        // 编码器设置入口：仅当该设备编码器在线（encoder_present）时显示；
        // 未知（老固件/未上报）默认显示，避免误隐藏。
        const bool encoder_present =
            info_it == device_info_map_.end() || info_it->second.encoder_present;
        if (encoder_present) {
            AppendMenuW(submenu, MF_STRING,
                        kMenuEncoderSettingsBase + static_cast<UINT>(i),
                        TrW(StringId::kMenuEncoderSettings, language).c_str());
        }

        // 设备交互设置入口：所有设备均显示（IMU 唤醒灵敏度/敲击方向键/体感灵敏度等按设备覆盖）。
        AppendMenuW(submenu, MF_STRING,
                    kMenuInteractionSettingsBase + static_cast<UINT>(i),
                    TrW(StringId::kMenuInteractionSettings, language).c_str());

        // 电池电压监测入口：仅连接设备显示（power_log 导出需要活跃 BLE 连接）。
        if (connected) {
            AppendMenuW(submenu, MF_STRING,
                        kMenuBatteryMonitorBase + static_cast<UINT>(i),
                        TrW(StringId::kMenuBatteryMonitor, language).c_str());
        }

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

        if (connected) {
            AppendMenuW(submenu, MF_STRING,
                        kMenuUpdateFirmwareFromFileBase + static_cast<UINT>(i),
                        TrW(StringId::kMenuUpdateFirmwareFromFile, language).c_str());
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
    AppendMenuW(menu,
                MF_STRING | (config_.selection_hotword_enabled ? MF_CHECKED : 0),
                kMenuSelectionHotword,
                TrW(StringId::kMenuSelectionHotword, language).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, TrW(StringId::kMenuSettings, language).c_str());
    // 体感鼠标调参入口已从托盘菜单隐藏；kMenuAirMouseTuning 命令处理与
    // air_mouse_tuning_window 保留，需要时可恢复菜单项重新启用。
    if (!config_.portable_mode) {
        AppendMenuW(menu, MF_STRING, kMenuCheckAppUpdates,
                    TrW(StringId::kMenuCheckAppUpdates, language).c_str());
    }
    AppendMenuW(menu, MF_STRING, kMenuFlashTool,
                TrW(StringId::kMenuFlashTool, language).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuRelaunchElevated,
                TrW(StringId::kMenuRelaunchElevated, language).c_str());
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
    // 清单为 asInvoker 不提权，开机自启用标准 HKCU\...\Run 注册表项即可，无需任务计划程序。
    // 一次性迁移：删除历史 requireAdministrator 方案遗留的"VoiceStickAutoStart"任务计划程序
    // 任务，避免它仍以 Highest 权限拉起本 asInvoker exe（导致去管理员后仍弹 UAC）。best-effort，
    // 失败忽略（任务从未注册过时 schtasks 退出码非 0，属正常情况）。
    static std::once_flag legacy_task_cleaned;
    std::call_once(legacy_task_cleaned, [] {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring cmd =
            L"C:\\Windows\\System32\\schtasks.exe /Delete /TN VoiceStickAutoStart /F";
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            LogLine("Legacy autostart scheduled task cleanup attempted");
        } else {
            LogLine("Legacy autostart cleanup skipped (schtasks launch failed)");
        }
    });
    constexpr const wchar_t* kRunKeyPath =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* kRunValueName = L"VoiceStick";

    const std::wstring command = CurrentExecutableCommand();
    if (command.empty()) {
        LogLine("Launch at login sync failed: failed to resolve executable path");
        throw std::runtime_error("failed to resolve executable path for launch-at-login");
    }

    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0,
                                   KEY_SET_VALUE, &key);
    if (status != ERROR_SUCCESS) {
        LogLine("Launch at login sync failed: RegOpenKeyExW hr=" + std::to_string(status));
        throw std::runtime_error("failed to open HKCU Run key for launch-at-login");
    }

    if (!config_.launch_at_login) {
        status = RegDeleteValueW(key, kRunValueName);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            LogLine("Launch at login sync failed: RegDeleteValueW hr=" + std::to_string(status));
            throw std::runtime_error("failed to delete launch-at-login run value");
        }
        LogLine("Launch at login disabled (run value deleted)");
        return;
    }

    status = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        LogLine("Launch at login sync failed: RegSetValueExW hr=" + std::to_string(status));
        throw std::runtime_error("failed to set launch-at-login run value");
    }
    LogLine("Launch at login enabled (HKCU Run value set)");
}

void Win32App::SaveInputOptions() {
    try {
        config_.SavePreservingDiskCredentials();
        SyncLaunchAtLogin();
        SyncSelectionHotword();
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

void Win32App::SyncSelectionHotword() {
    if (!selection_hotword_manager_) return;
    selection_hotword_manager_->SetLanguage(EffectiveUiLanguage(config_.ui_language));
    selection_hotword_manager_->SetMaxLength(config_.hotword_process_enabled
                                                 ? SelectionHotwordManager::kMaxProcessLen
                                                 : SelectionHotwordManager::kMaxHotwordLen);
    selection_hotword_manager_->SetEnabled(config_.selection_hotword_enabled);
}

void Win32App::ProcessHotwordWithLlm(const std::string& text) {
    const auto lang = EffectiveUiLanguage(config_.ui_language);
    // Trim 语义判空：纯空白 key 视为未配置。
    if (config_.llm_api_key.find_first_not_of(" \t\r\n") == std::string::npos) {
        const bool session_active = coordinator_ && coordinator_->HasActiveSession();
        if (session_active || !overlay_) {
            ShowNotification(Tr(StringId::kSettingsSectionHotwordProcess, lang),
                             Tr(StringId::kHotwordProcessNoKey, lang));
        } else {
            overlay_->ShowTimedMessage(Tr(StringId::kHotwordProcessNoKey, lang), 3000);
        }
        return;
    }
    // 会话活跃时浮窗被状态机占用（如确认倒计时），静默提炼，最终反馈走托盘气泡。
    const bool session_active = coordinator_ && coordinator_->HasActiveSession();
    if (!session_active && overlay_) {
        overlay_->ShowRefining(Tr(StringId::kHotwordProcessExtracting, lang));
    }
    // ChatAsync 内部拷贝配置并 detached 线程执行，栈上临时对象安全。
    HotwordExtractor(config_).Extract(text, config_.hotword_process_prompt,
        [this](bool ok, std::string result) {
            DispatchToUi([this, ok, result = std::move(result)]() mutable {
                OnHotwordExtracted(ok, result);
            });
        });
}

void Win32App::OnHotwordExtracted(bool ok, const std::string& result) {
    const auto lang = EffectiveUiLanguage(config_.ui_language);
    // 会话活跃时不碰 overlay（避免踩掉确认倒计时等状态机浮窗），反馈统一走托盘气泡。
    const bool session_active = coordinator_ && coordinator_->HasActiveSession();
    const auto feedback = [&](const std::string& message) {
        if (session_active || !overlay_) {
            ShowNotification(Tr(StringId::kSettingsSectionHotwordProcess, lang), message);
        } else {
            overlay_->ShowTimedMessage(message, 3000);
        }
    };
    if (!ok) {
        LogLine("Hotword extraction failed: " + result);
        feedback(Tr(StringId::kHotwordProcessFailed, lang));
        return;
    }
    const auto extracted = HotwordExtractor::ParseExtractResult(result);
    if (extracted.empty()) {
        LogLine("Hotword extraction: no words parsed");
        feedback(Tr(StringId::kHotwordProcessEmptyResult, lang));
        return;
    }
    const auto new_words = HotwordExtractor::DiffNewHotwords(extracted, config_.asr_hotwords);
    if (new_words.empty()) {
        LogLine("Hotword extraction: all duplicates");
        feedback(Tr(StringId::kHotwordProcessAllDuplicate, lang));
        return;
    }
    auto& hotwords = config_.asr_hotwords;
    hotwords.insert(hotwords.end(), new_words.begin(), new_words.end());
    try {
        config_.SavePreservingDiskCredentials();
    } catch (const std::exception& e) {
        LogLine(std::string("Save config on hotword extract failed: ") + e.what());
    }
    if (coordinator_) coordinator_->UpdateConfig(config_);
    // 顿号拼接新词列表用于浮窗展示（重复词不展示）。
    std::string joined;
    for (std::size_t i = 0; i < new_words.size(); ++i) {
        if (i != 0) joined += "、";
        joined += new_words[i];
    }
    feedback(Tr(StringId::kHotwordProcessAdded, lang) + joined);
    LogLine("Hotwords extracted and added: " + joined);
}

void Win32App::SaveDeviceThemeColor(const std::string& device_id, OverlayThemeColor color) {
    try {
        if (color == DefaultOverlayThemeColor()) {
            config_.device_theme_colors.erase(device_id);
        } else {
            config_.device_theme_colors[device_id] = color;
        }
        config_.SavePreservingDiskCredentials();
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
        config_.SavePreservingDiskCredentials();
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
        config_.SavePreservingDiskCredentials();
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
        config_.SavePreservingDiskCredentials();
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
    // 每次重新进入时重建（与 ShowEncoderSettingsDialog 同模式）：SettingsDialog 内部
    // 持有 config_ 的快照，复用旧实例会用过期快照覆盖当前 config_，丢失其他对话框
    // （如编码器设置）在两次打开之间所做的按设备覆盖修改。
    settings_dialog_ = std::make_unique<SettingsDialog>(instance_, hwnd_, config_);
    settings_dialog_->on_config_changed = [this](AppConfig new_config) {
        config_ = std::move(new_config);
        // SaveInputOptions 内部已调用 coordinator_->UpdateConfig(config_) 完成同步。
        SaveInputOptions();
        RebuildTooltip();
        LogLine("Settings saved");
    };
    settings_dialog_->Show();
}

void Win32App::ShowEncoderSettingsDialog(const std::string& device_id) {
    // 单实例策略：模态对话框同时只开一个，重新进入时重建（Show 内同步阻塞至关闭）。
    encoder_settings_dialog_ = std::make_unique<EncoderSettingsDialog>(
        instance_, hwnd_, device_id,
        config_.EncoderSettingsForDevice(device_id),
        config_.default_encoder_settings,
        config_.ui_language);
    encoder_settings_dialog_->on_settings_changed =
        [this](const std::string& id, std::optional<EncoderSettings> override) {
            if (override.has_value()) {
                config_.device_encoder_settings[id] = *override;
            } else {
                // 与全局默认一致：清除覆盖，回落默认。
                config_.device_encoder_settings.erase(id);
            }
            // Save() 可能因 config.toml 被占用抛异常，与 SaveDeviceOutputProfile 同模式捕获。
            try {
                config_.SavePreservingDiskCredentials();
            } catch (const std::exception& e) {
                LogLine(std::string("Encoder settings: config_.Save failed: ") + e.what());
                return;
            }
            if (coordinator_) coordinator_->UpdateConfig(config_);
            LogLine("Encoder settings saved for VS-" + id);
        };
    encoder_settings_dialog_->Show();
}

void Win32App::ShowAirMouseTuning() {
    // 体感鼠标调参按"激活设备"：多个设备进入体感时取第一个激活设备；
    // 无激活设备时取第一个已连接设备（允许用户进入体感前预调）。
    std::string target_device;
    if (coordinator_) {
        for (const auto& dev : connected_devices_) {
            if (coordinator_->IsAirMouseActive(dev.id)) { target_device = dev.id; break; }
        }
    }
    if (target_device.empty()) {
        for (const auto& dev : connected_devices_) { target_device = dev.id; break; }
    }
    if (target_device.empty()) {
        LogLine("ShowAirMouseTuning: no connected device, skip");
        return;
    }
    const std::string device_id = target_device;
    if (!air_mouse_tuning_window_ || !air_mouse_tuning_window_->IsOpen() ||
        air_mouse_tuning_window_->device_id() != device_id) {
        air_mouse_tuning_window_ = std::make_unique<AirMouseTuningWindow>(
            instance_, hwnd_, device_id,
            coordinator_ ? coordinator_->GetAirMouseParamsForTuning(device_id) : AirMouseParams{});
        air_mouse_tuning_window_->on_params_changed = [this, device_id](const AirMouseTuningState& state) {
            if (coordinator_) coordinator_->UpdateAirMouseParams(device_id, state.ToParams());
        };
        air_mouse_tuning_window_->on_save_requested = [this, device_id](const AirMouseTuningState& state) {
            // 灵敏度按设备覆盖写入 InteractionSettings（其余进阶参数仍走全局 config_）。
            InteractionSettings settings = config_.InteractionSettingsForDevice(device_id);
            settings.air_mouse_sensitivity_x = state.sensitivity_x;
            settings.air_mouse_sensitivity_y = state.sensitivity_y;
            if (settings == config_.default_interaction_settings) {
                config_.device_interaction_settings.erase(device_id);
            } else {
                config_.device_interaction_settings[device_id] = settings;
            }
            // 进阶 air_mouse 参数（tau/invert_y/curve/rate/neutral_deadzone/control_mode）保持全局。
            config_.air_mouse_tau = state.tau;
            config_.air_mouse_invert_y = state.invert_y;
            config_.air_mouse_curve_low_thresh = state.curve.low_thresh;
            config_.air_mouse_curve_high_thresh = state.curve.high_thresh;
            config_.air_mouse_curve_low_factor = state.curve.low_factor;
            config_.air_mouse_curve_high_factor = state.curve.high_factor;
            config_.air_mouse_neutral_deadzone = state.neutral_deadzone;
            config_.air_mouse_control_mode = AirMouseControlModeName(state.control_mode);
            config_.air_mouse_rate_gain = state.rate_gain;
            config_.air_mouse_rate_friction = state.rate_friction;
            config_.air_mouse_rate_max_speed = state.rate_max_speed;
            try {
                config_.SavePreservingDiskCredentials();
            } catch (const std::exception& e) {
                LogLine(std::string("Air mouse tuning: config_.Save failed: ") + e.what());
                return;
            }
            if (coordinator_) coordinator_->UpdateConfig(config_);
            LogLine("Air mouse tuning saved for VS-" + device_id);
        };
    }
    air_mouse_tuning_window_->Show();
}

void Win32App::ShowInteractionSettingsDialog(const std::string& device_id) {
    // 单实例策略：模态对话框重入时若目标设备不同则重建。
    interaction_settings_dialog_ = std::make_unique<InteractionSettingsDialog>(
        instance_, hwnd_, device_id,
        config_.InteractionSettingsForDevice(device_id),
        config_.default_interaction_settings,
        config_.ui_language);
    interaction_settings_dialog_->on_settings_changed =
        [this](const std::string& id, std::optional<InteractionSettings> override) {
            if (override.has_value()) {
                config_.device_interaction_settings[id] = *override;
            } else {
                // 与全局默认一致：清除覆盖，回落默认。
                config_.device_interaction_settings.erase(id);
            }
            try {
                config_.SavePreservingDiskCredentials();
            } catch (const std::exception& e) {
                LogLine(std::string("Interaction settings: config_.Save failed: ") + e.what());
                return;
            }
            if (coordinator_) coordinator_->UpdateConfig(config_);
            LogLine("Interaction settings saved for VS-" + id);
        };
    interaction_settings_dialog_->Show();
}

void Win32App::ShowBatteryMonitorDialog(const std::string& device_id) {
    // 单实例非模态窗口：重入（同设备）置前即可，换目标设备则重建（旧监测会话丢弃）。
    if (battery_monitor_dialog_ && battery_monitor_dialog_->IsSameDevice(device_id)) {
        battery_monitor_dialog_->Show();
        return;
    }
    battery_monitor_dialog_ = std::make_unique<BatteryMonitorDialog>(
        instance_, hwnd_, EffectiveUiLanguage(config_.ui_language), device_id);
    battery_monitor_dialog_->on_send_command =
        [this](const std::string& id, ByteVector payload) {
            if (coordinator_) coordinator_->SendPowerLogCommand(id, std::move(payload));
        };
    battery_monitor_dialog_->on_closed = [this] {
        // 窗口销毁后释放对象（分片路由指针随之失效）。
        battery_monitor_dialog_.reset();
    };
    battery_monitor_dialog_->Show();
    // 若已缓存设备上报的开关状态（连接时固件会推送），立即同步勾选框。
    const auto state_it = usb_auto_off_state_.find(device_id);
    if (state_it != usb_auto_off_state_.end()) {
        battery_monitor_dialog_->OnPowerMgmtState(device_id, state_it->second);
    }
    LogLine("Battery monitor opened for VS-" + device_id);
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
    firmware_update_dialog_->on_advanced = [this] { LaunchFlashToolExe(hwnd_); };
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

void Win32App::StartFirmwareUpdateFromFile(const std::string& device_id) {
    if (!coordinator_) return;
    wchar_t path[MAX_PATH] = {};
    const auto language = EffectiveUiLanguage(config_.ui_language);
    const auto title = TrW(StringId::kMenuUpdateFirmwareFromFile, language);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Firmware binary (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = title.c_str();
    if (!GetOpenFileNameW(&ofn)) return;  // 用户取消或打开失败
    StartOtaFromFile(Utf8FromUtf16(path), device_id);
}

void Win32App::StartOtaFromFile(const std::string& file_path,
                                const std::optional<std::string>& device_id) {
    if (!coordinator_) return;
    const auto language = EffectiveUiLanguage(config_.ui_language);

    // 自动选设备：指定且已连接用之；未指定取第一个；均无则提示。
    std::string target_device;
    if (device_id.has_value()) {
        bool connected = false;
        for (const auto& dev : connected_devices_) {
            if (dev.id == *device_id) { connected = true; break; }
        }
        if (!connected) {
            ShowNotification(Tr(StringId::kNotificationFirmwareUpdatedTitle, language),
                            "设备 VS-" + *device_id + " 未连接，无法更新固件");
            return;
        }
        target_device = *device_id;
    } else if (!connected_devices_.empty()) {
        target_device = connected_devices_.front().id;
    } else {
        ShowNotification(Tr(StringId::kNotificationFirmwareUpdatedTitle, language),
                        "无已连接设备，无法更新固件");
        return;
    }

    firmware_update_dialog_ = std::make_unique<FirmwareUpdateDialog>(
        instance_, hwnd_, language, "local file");
    firmware_update_dialog_->on_cancel = [this] {
        if (coordinator_) coordinator_->CancelFirmwareUpdate();
    };
    firmware_update_dialog_->on_advanced = [this] { LaunchFlashToolExe(hwnd_); };
    firmware_update_dialog_->Show();
    coordinator_->UpdateFirmwareFromFile(
        file_path, target_device,
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

void Win32App::SetPendingOtaRequest(std::string file_path,
                                     std::optional<std::string> device_id) {
    pending_ota_request_ = OtaCliRequest{std::move(file_path), std::move(device_id)};
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

void Win32App::ShowTimedMessage(const std::string& message, int duration_ms) {
    // 可能被后台线程调用（如 LLM 提炼回调），封送到 UI 线程再碰 overlay。
    DispatchToUi([this, message, duration_ms]() {
        // 会话活跃时浮窗被状态机占用（确认倒计时等），回退托盘气泡。
        if ((coordinator_ && coordinator_->HasActiveSession()) || !overlay_) {
            ShowNotification({}, message);
            return;
        }
        overlay_->ShowTimedMessage(message, duration_ms);
    });
}

std::wstring Win32App::Utf16(const std::string& text) const {
    return Utf16FromUtf8(text);
}

} // namespace voicestick
