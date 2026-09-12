#pragma once

// 小米蓝牙遥控器 2 Pro 的按键 ID 常量表（与 resources/xiaomi_remote_2pro.png
// 画布热区、localization kXiaomiButton* 文案一一对应）。mic 为语音输入专用键，
// 不参与按键映射，故不在可映射列表内。
#include <array>
#include <string_view>

namespace voicestick {

// 可映射按键 ID（12 键）。顺序仅约定展示/文档一致，无运行时语义。
inline constexpr std::array<std::string_view, 12> kXiaomiMappableButtons{
    "power", "up", "down", "left", "right", "ok", "back", "home",
    "volume_up", "volume_down", "menu", "tv",
};

// id 是否为可映射按键（mic/空串/未知 id 均不可映射）。
inline bool IsXiaomiMappableButton(std::string_view id) {
    for (std::string_view button : kXiaomiMappableButtons) {
        if (button == id) return true;
    }
    return false;
}

} // namespace voicestick
