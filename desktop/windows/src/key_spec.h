#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace voicestick {

// 一次按键注入的规格：修饰键（VK_CONTROL/VK_MENU/VK_SHIFT/VK_LWIN，固定
// Ctrl/Alt/Shift/Win 序）+ 主键 VK + 规范化显示文本（如 "Ctrl+Shift+V"）。
// 供编码器旋转/单击/双击的自定义按键注入使用。
struct KeySpec {
    std::vector<UINT> modifiers;
    UINT vk = 0;
    std::string display_text;
};

// 解析热键语法：单键（up/down/left/right/enter|return/esc|escape/tab/space/
// backspace/delete/insert/pageup/pagedown/home/end/volumeup/volumedown/volumemute/
// f1-f24/单字符 A-Z0-9）或修饰键组合（ctrl|control/alt/shift/win|windows|meta
// + 单键，"+" 分隔，大小写与前后空白不敏感）。
// 仅修饰键、未知键名、多个主键均返回 nullopt。
std::optional<KeySpec> ParseKeySpec(const std::string& text);

// 由捕获结果构造 KeySpec（modifiers 须为 VK_CONTROL/VK_MENU/VK_SHIFT/VK_LWIN 子集，
// 固定 Ctrl/Alt/Shift/Win 序；vk 不可识别时 display_text 退化为 "VK0xXX"）。恒成功。
KeySpec MakeKeySpecFromVk(const std::vector<UINT>& modifiers, UINT vk);

} // namespace voicestick
