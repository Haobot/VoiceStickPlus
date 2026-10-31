#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace voicestick {

// 小米蓝牙遥控器 2 Pro 可映射按键 ID：设置 UI（遥控器图片热点）与配置
// XiaomiSettings.key_map 共用。不含 mic：语音键是语音输入专用，不参与映射。
inline constexpr std::array<std::string_view, 12> kXiaomiMappableButtons = {
    "power", "up", "left", "ok", "right", "down",
    "back", "volume_up", "home", "volume_down", "menu", "tv",
};

inline bool IsXiaomiMappableButton(std::string_view id) {
    return std::find(kXiaomiMappableButtons.begin(), kXiaomiMappableButtons.end(), id) !=
           kXiaomiMappableButtons.end();
}

} // namespace voicestick
