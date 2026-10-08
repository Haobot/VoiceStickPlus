#include "win32_app.h"

#include "asr_client_win.h"
#include "asr_client_tencent.h"
#include "ble_central_win.h"
#include "hotword_extractor.h"
#include "hotword_selector.h"  // ValidateHotword（B14 统一口径）
#include "license_runtime.h"
#include "local_asr_client_win.h"
#include "machine_guid_win.h"
#ifdef VOICESTICK_LOCAL_REFINE_ENABLED
#include "llama_cpp_engine.h"
#include "local_refinement_client.h"
#endif
#include "localization.h"
#include "log.h"
#include "mic_mode_hotkey.h"
#include "push_to_talk_key.h"
#include "resource.h"
#include "wasapi_mic_capture.h"

#include <Shellapi.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <winsparkle.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "win32_app_util.h"
namespace voicestick {
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
        ApplyUpdatedConfig();
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
        ApplyUpdatedConfig();
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
    // 对话框按当前网关状态初始化（此后由 SetConnectedDevices/SetDeviceGatewayMode 刷新）。
    SyncGatewayModeSuppression();
    pair_device_dialog_->SetManualPairHandler([this](std::string device_id) {
        PairDeviceByManualId(device_id);
    });
    // VS 设备系统配对（Bond）软降级告警：对话框关闭后状态栏就看不到了，改弹托盘
    // 气泡把「按键直通可能不可用 + 怎么补救」讲清楚（复用既有通知通道）。
    pair_device_dialog_->SetPairWarningHandler([this](std::string message) {
        ShowNotification(Tr(StringId::kStaleSessionTitle, EffectiveUiLanguage(config_.ui_language)),
                         message);
        LogLine("pair warning: " + message);
    });
    pair_device_dialog_->on_pair_timeout = [this](std::string device_id) {
        // 类别感知日志前缀：与 SetPairingError 同模式，reset 前先判定。
        const bool is_xiaomi =
            pending_pairing_entry_ && pending_pairing_entry_->device_id == device_id &&
            BleProtocol::DeviceClassFromName(pending_pairing_entry_->name)
                    .value_or(DeviceClass::kStickS3) == DeviceClass::kXiaomiRemote2Pro;
        pending_pairing_entry_.reset();
        if (coordinator_) coordinator_->CancelPendingConnect(device_id);
        LogLine("Pairing timed out " + std::string(is_xiaomi ? "RC-" : "VS-") + device_id);
    };
    pair_device_dialog_->Show();
}

void Win32App::ShowSettings() {
    // 每次重新进入时重建（与 ShowEncoderSettingsDialog 同模式）：SettingsDialog 内部
    // 持有 config_ 的快照，复用旧实例会用过期快照覆盖当前 config_，丢失其他对话框
    // （如编码器设置）在两次打开之间所做的按设备覆盖修改。
    settings_dialog_ = std::make_unique<SettingsDialog>(
        instance_, hwnd_, config_,
        coordinator_ ? coordinator_->ConnectedDeviceIds() : std::vector<std::string>{},
        ReadMachineGuid().value_or(std::string{}));
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
            ApplyUpdatedConfig();
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
            ApplyUpdatedConfig();
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
            ApplyUpdatedConfig();
            LogLine("Interaction settings saved for VS-" + id);
        };
    interaction_settings_dialog_->Show();
}

void Win32App::ShowRemoteSettingsDialog(const std::string& device_id) {
    // 单实例策略：模态对话框同时只开一个，重新进入时重建（Show 内同步阻塞至关闭）。
    remote_settings_dialog_ = std::make_unique<RemoteSettingsDialog>(
        instance_, hwnd_, device_id,
        config_.XiaomiSettingsForDevice(device_id),
        config_.default_xiaomi_settings,
        config_.ui_language);
    remote_settings_dialog_->on_settings_changed =
        [this](const std::string& id, std::optional<XiaomiSettings> override) {
            if (override.has_value()) {
                config_.device_xiaomi_settings[id] = *override;
            } else {
                // 与全局默认一致：清除覆盖，回落默认。
                config_.device_xiaomi_settings.erase(id);
            }
            // Save() 可能因 config.toml 被占用抛异常，与 SaveDeviceOutputProfile 同模式捕获。
            try {
                config_.SavePreservingDiskCredentials();
            } catch (const std::exception& e) {
                LogLine(std::string("Remote settings: config_.Save failed: ") + e.what());
                return;
            }
            ApplyUpdatedConfig();
            LogLine("Remote settings saved for RC-" + id);
        };
    remote_settings_dialog_->Show();
}

void Win32App::ShowXiaomiKeymapDialog(const std::string& device_id) {
    // 单实例策略：模态对话框同时只开一个，重新进入时重建（Show 内同步阻塞至关闭）。
    xiaomi_keymap_dialog_ = std::make_unique<XiaomiKeymapDialog>(
        instance_, hwnd_, device_id,
        config_.XiaomiSettingsForDevice(device_id),
        config_.default_xiaomi_settings,
        config_.ui_language);
    xiaomi_keymap_dialog_->on_repeat_interval_changed =
        [this](int interval_ms) {
            // 与网关对话框共享同一全局设置（直连模式映射的连发走直触发节拍，
            // 但滑块值全局持久化，两侧语义一致：网关路由键的连发速度）。
            config_.xiaomi_gateway_repeat_interval_ms = interval_ms;
            try {
                config_.SavePreservingDiskCredentials();
            } catch (const std::exception& e) {
                LogLine(std::string("Keymap: config_.Save failed: ") + e.what());
                return;
            }
            ApplyUpdatedConfig();
            LogLine("Gateway repeat interval set to " + std::to_string(interval_ms) + "ms");
        };
    xiaomi_keymap_dialog_->on_settings_changed =
        [this](const std::string& id, std::optional<XiaomiSettings> override) {
            if (override.has_value()) {
                config_.device_xiaomi_settings[id] = *override;
            } else {
                // 与全局默认一致：清除覆盖，回落默认。
                config_.device_xiaomi_settings.erase(id);
            }
            // Save() 可能因 config.toml 被占用抛异常，与 SaveDeviceOutputProfile 同模式捕获。
            try {
                config_.SavePreservingDiskCredentials();
            } catch (const std::exception& e) {
                LogLine(std::string("Keymap: config_.Save failed: ") + e.what());
                return;
            }
            ApplyUpdatedConfig();
            LogLine("Keymap saved for RC-" + id);
        };
    // 增强按键识别开关：即时保存设备级设置并同步钩子（UAC 授权流程即时反馈）。
    xiaomi_keymap_dialog_->on_hid_tap_changed =
        [this](const std::string& id, bool enabled) {
            // 覆盖表里可能已有该设备的其他字段，先取现有覆盖再改字段。
            XiaomiSettings settings = config_.XiaomiSettingsForDevice(id);
            settings.hid_tap_enabled = enabled;
            config_.device_xiaomi_settings[id] = settings;
            try {
                config_.SavePreservingDiskCredentials();
            } catch (const std::exception& e) {
                LogLine(std::string("Keymap: config_.Save failed: ") + e.what());
                return;
            }
            ApplyUpdatedConfig();
            LogLine("HidTap " + std::string(enabled ? "enabled" : "disabled") +
                    " for RC-" + id);
        };
    // 链路状态行查询：探针未启用/钩子未运行返回 nullopt（显示「未启用」）。
    xiaomi_keymap_dialog_->tap_state_query = [this] {
        return xiaomi_keymap_hook_ ? xiaomi_keymap_hook_->tap_state()
                                   : std::nullopt;
    };
    xiaomi_keymap_dialog_->Show();
}

// D10b：回执文本 = i18n 标签前缀 + "key=route, …"（key/route 均 ASCII，直接宽化）。
static std::wstring FormatGatewayReceipt(
    const std::vector<StateEvent::KeyRoute>& routes, UiLanguage language) {
    std::wstring text = TrW(StringId::kGatewayDeviceRoutesLabel, language);
    text += L" ";
    bool first = true;
    for (const auto& route : routes) {
        if (!first) text += L", ";
        first = false;
        text += std::wstring(route.key.begin(), route.key.end());
        text += L"=";
        text += std::wstring(route.route.begin(), route.route.end());
    }
    return text;
}

void Win32App::OnGatewayKeymapReport(
    const std::vector<StateEvent::KeyRoute>& routes) {
    // D10b：完整回执到达 → 现开对话框回显；未开则留存于 coordinator 下次打开预填。
    if (!xiaomi_keymap_dialog_) return;
    xiaomi_keymap_dialog_->SetDeviceReceipt(
        FormatGatewayReceipt(routes, EffectiveUiLanguage(config_.ui_language)));
}

void Win32App::ShowGatewayKeymapDialog() {
    // 网关模式遥控器映射（P1，Doc/Plan/xiaomi-remote-stick-gateway.md §5.3）：
    // 遥控器配对在 StickS3 上、不在桌面端 paired_devices 里，映射编辑直接落在
    // 全局默认 default_xiaomi_settings（[xiaomi.keys]）——协调器路由下发与钩子
    // 快照在「无配对 RC」时取同一口径，保存即经 UpdateConfig 逐键下发固件。
    const XiaomiSettings saved = config_.default_xiaomi_settings;
    xiaomi_keymap_dialog_ = std::make_unique<XiaomiKeymapDialog>(
        instance_, hwnd_, "gateway",
        config_.default_xiaomi_settings,
        XiaomiSettings{},  // 「恢复默认」= 清空全部映射（全局默认即正在编辑的对象）
        EffectiveUiLanguage(config_.ui_language),
        config_.xiaomi_gateway_repeat_interval_ms);
    xiaomi_keymap_dialog_->on_repeat_interval_changed =
        [this](int interval_ms) {
            config_.xiaomi_gateway_repeat_interval_ms = interval_ms;
            try {
                config_.SavePreservingDiskCredentials();
            } catch (const std::exception& e) {
                LogLine(std::string("Keymap: config_.Save failed: ") + e.what());
                return;
            }
            ApplyUpdatedConfig();  // → SyncXiaomiKeymapHook 热更连发节拍
            LogLine("Gateway repeat interval set to " + std::to_string(interval_ms) + "ms");
        };
    xiaomi_keymap_dialog_->on_settings_changed =
        [this, saved](const std::string&, std::optional<XiaomiSettings> override) {
            // 只回写 key_map：gain_db/double_click_ms 等其余字段服务直连模式，
            // 不随网关映射编辑变动。nullopt（编辑结果与空默认全等）回存原映射。
            if (override.has_value()) {
                config_.default_xiaomi_settings.key_map = override->key_map;
            } else {
                config_.default_xiaomi_settings.key_map = saved.key_map;
            }
            try {
                config_.SavePreservingDiskCredentials();
            } catch (const std::exception& e) {
                LogLine(std::string("Keymap: config_.Save failed: ") + e.what());
                return;
            }
            ApplyUpdatedConfig();
            LogLine("Keymap saved for gateway RC");
        };
    xiaomi_keymap_dialog_->on_hid_tap_changed =
        [this](const std::string&, bool enabled) {
            // 网关模式遥控器不经 Windows HID 栈，探针无意义：忽略开关，避免
            // 误写全局默认。对话框仍显示该开关（控件复用），行为在此收口。
            LogLine("HidTap toggle ignored in gateway keymap dialog");
        };
    xiaomi_keymap_dialog_->Show();
    // D10b：打开即回显留存的最近回执，并主动 GET 拉新（分片回执到达后经
    // OnGatewayKeymapReport 刷新）。
    if (coordinator_) {
        const auto& receipt = coordinator_->LastGatewayKeymapReport();
        if (!receipt.empty()) {
            xiaomi_keymap_dialog_->SetDeviceReceipt(
                FormatGatewayReceipt(receipt, EffectiveUiLanguage(config_.ui_language)));
        }
        coordinator_->RequestGatewayKeymapReport();
    }
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
        const auto device_class =
            BleProtocol::DeviceClassFromName(name).value_or(DeviceClass::kStickS3);
        pending_pairing_entry_ = PairedDeviceEntry{device_id, bluetooth_address, address_kind, name};
        // 小米遥控器配对完成时走 finalize 定时器兜底（无 firmware_version 不进
        // HandlePairingSucceeded），hardware 字段不会被 device_info 覆盖；这里按
        // 配对时的设备类别先写入，保证重启后 config 里的 hardware 仍是
        // xiaomi_remote_2_pro，不会回落成 StickS3 直连。
        if (device_class == DeviceClass::kXiaomiRemote2Pro) {
            pending_pairing_entry_->hardware = std::string(kHardwareXiaomiRemote2Pro);
        }
        coordinator_->ConnectPairedDevice(device_id, bluetooth_address, address_kind, name,
                                          device_class);
        LogLine("Pairing device " +
                std::string(device_class == DeviceClass::kXiaomiRemote2Pro ? "RC-" : "VS-") +
                device_id);
    }
}

void Win32App::PairDeviceByManualId(const std::string& device_id) {
    // 手动输入存 entry 时 hardware 为空（无法从 ID 判断类别）：小米遥控器（RC-XXXX）
    // 首连前菜单会短暂按 StickS3 显隐（多出交互/编码器等不适用项）。首连时 BLE 层
    // 按名称/ATVV UUID 识别并合成 device_info，hardware 随之落盘自愈，属可接受取舍。
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
    // 新气泡顶掉旧气泡：旧气泡未消费的点击动作随之作废，防止点击语义错位。
    pending_balloon_action_.reset();
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

void Win32App::ShowActionableNotification(const std::string& title, const std::string& body,
                                          BalloonAction action) {
    pending_balloon_action_ = std::move(action);
    ShowNotification(title, body);
}

void Win32App::ShowFirmwareUpdateBalloon(const std::string& device_id,
                                         const std::string& current_version,
                                         const std::string& latest_version,
                                         bool is_below_minimum) {
    DispatchToUi([this, device_id, current_version, latest_version, is_below_minimum] {
        const auto language = EffectiveUiLanguage(config_.ui_language);
        BalloonAction action;
        action.kind = BalloonAction::Kind::kFirmwareUpdate;
        action.device_id = device_id;
        ShowActionableNotification(
            Tr(is_below_minimum ? StringId::kFirmwareUpdatePromptTitleRequired
                                : StringId::kFirmwareUpdatePromptTitleAvailable,
               language),
            FormatUtf8(Tr(StringId::kFirmwareUpdatePromptBody, language),
                       {device_id, current_version, latest_version}),
            std::move(action));
    });
}

void __cdecl Win32App::WinSparkleFoundUpdateBridge() {
    if (active_instance_ != nullptr) {
        active_instance_->OnAppUpdateFound();
    }
}

int __cdecl Win32App::WinSparkleCanShutdownBridge() {
    // 本应用无可阻断的未保存状态（配置即改即落盘），始终允许优雅退出。
    return TRUE;
}

void __cdecl Win32App::WinSparkleShutdownRequestBridge() {
    Win32App* inst = active_instance_;
    if (inst != nullptr) {
        inst->DispatchToUi([inst] { inst->ShutdownAndQuit(); });
    }
}

void Win32App::OnAppUpdateFound() {
    // WinSparkle 工作线程回调：封送 UI 线程。每会话只气泡一次；用户可在标准
    // 对话框里"跳过此版本"永久静音该版本（WinSparkle 注册表机制）。
    DispatchToUi([this] {
        if (app_update_balloon_shown_ || config_.portable_mode) {
            return;
        }
        app_update_balloon_shown_ = true;
        const auto language = EffectiveUiLanguage(config_.ui_language);
        BalloonAction action;
        action.kind = BalloonAction::Kind::kAppUpdate;
        ShowActionableNotification(Tr(StringId::kNotificationAppUpdateTitle, language),
                                   Tr(StringId::kNotificationAppUpdateBody, language),
                                   std::move(action));
        LogLine("app update found: balloon shown");
    });
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
