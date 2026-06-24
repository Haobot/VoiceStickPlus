#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace voicestick {

class WifiCredentialsWin {
public:
    static std::optional<std::wstring> ReadPassword(std::string_view device_id);
    static bool WritePassword(std::string_view device_id, const std::wstring& password);
    static void DeletePassword(std::string_view device_id);

    static std::wstring TargetName(std::string_view device_id);
};

} // namespace voicestick
