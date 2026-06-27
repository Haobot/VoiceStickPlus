#pragma once

#include "app_config.h"
#include "ble_protocol.h"

#include <Windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace voicestick {

class WifiSettingsDialog {
public:
    struct Options {
        std::string device_id;
        std::string device_name;
        std::string firmware_version;
        UiLanguage language = UiLanguage::kSystem;
        WifiDeviceProfile profile;
        std::optional<std::wstring> saved_password;
        std::optional<WifiStatusSnapshot> status;
    };

    struct Callbacks {
        std::function<void(std::string ssid, std::wstring password)> apply_wifi;
        std::function<void()> clear_wifi;
        std::function<void()> scan_wifi;
        std::function<void()> refresh_status;
        std::function<void(std::string url, std::string sha256)> start_ota;
        std::function<void()> commit_ota;
        std::function<void(WifiDeviceProfile)> save_profile;
    };

    WifiSettingsDialog(HINSTANCE instance, HWND parent, Options options, Callbacks callbacks);
    ~WifiSettingsDialog();

    void Show();
    void UpdateStatus(const WifiStatusSnapshot& status);
    void PopulateWifiScanResults(const std::vector<WifiApInfo>& aps);
    HWND hwnd() const { return hwnd_; }

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);

    LPCDLGTEMPLATE BuildDialogTemplate();
    void BuildControls();
    void DestroyControls();
    void LayoutControls();
    void LoadInitialValues();
    void RefreshStatusText();
    void OnApplyWifi();
    void OnClearWifi();
    void OnScanWifi();
    void OnStartOta();
    void OnCommitOta();
    void ToggleShowPassword();
    std::wstring GetText(HWND control) const;
    void SetText(HWND control, const std::wstring& text);
    std::string Utf8(const std::wstring& text) const;
    std::wstring Utf16(std::string_view text) const;
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    Options options_;
    Callbacks callbacks_;
    std::optional<WifiStatusSnapshot> status_;
    std::vector<BYTE> dialog_template_;

    HWND device_label_ = nullptr;
    HWND ssid_combo_ = nullptr;
    HWND scan_button_ = nullptr;
    HWND password_edit_ = nullptr;
    HWND show_password_check_ = nullptr;
    HWND apply_button_ = nullptr;
    HWND clear_button_ = nullptr;
    HWND refresh_button_ = nullptr;
    HWND status_label_ = nullptr;
    HWND ip_label_ = nullptr;
    HWND error_label_ = nullptr;
    HWND ota_url_edit_ = nullptr;
    HWND ota_sha_edit_ = nullptr;
    HWND ota_button_ = nullptr;
    HWND commit_button_ = nullptr;
    HWND ota_status_label_ = nullptr;
    HWND close_button_ = nullptr;
};

} // namespace voicestick
