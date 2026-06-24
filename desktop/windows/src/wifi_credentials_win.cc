#include "wifi_credentials_win.h"

#include <Windows.h>
#include <wincred.h>

#include <vector>

namespace voicestick {

namespace {

std::wstring Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), len);
    return wide;
}

} // namespace

std::wstring WifiCredentialsWin::TargetName(std::string_view device_id) {
    return L"VoiceStick/Wifi/" + Utf16(device_id);
}

std::optional<std::wstring> WifiCredentialsWin::ReadPassword(std::string_view device_id) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(TargetName(device_id).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        return std::nullopt;
    }
    std::wstring value;
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        const auto* begin = reinterpret_cast<const wchar_t*>(credential->CredentialBlob);
        const auto chars = credential->CredentialBlobSize / sizeof(wchar_t);
        value.assign(begin, begin + chars);
    }
    CredFree(credential);
    return value;
}

bool WifiCredentialsWin::WritePassword(std::string_view device_id, const std::wstring& password) {
    CREDENTIALW credential{};
    auto target = TargetName(device_id);
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(password.size() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(password.data()));
    credential.UserName = const_cast<wchar_t*>(L"VoiceStick");
    return CredWriteW(&credential, 0) != FALSE;
}

void WifiCredentialsWin::DeletePassword(std::string_view device_id) {
    CredDeleteW(TargetName(device_id).c_str(), CRED_TYPE_GENERIC, 0);
}

} // namespace voicestick
