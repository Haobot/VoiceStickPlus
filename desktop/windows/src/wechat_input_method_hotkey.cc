// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "wechat_input_method_hotkey.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace voicestick {

namespace {

std::string Lowercase(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(delimiter, start);
    const auto part = (end == std::string_view::npos)
                          ? value.substr(start)
                          : value.substr(start, end - start);
    if (!part.empty()) {
      parts.emplace_back(part);
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

int VkCodeFromName(std::string_view name) {
  const std::string lower = Lowercase(name);
  if (lower == "ctrl" || lower == "control") return VK_CONTROL;
  if (lower == "alt") return VK_MENU;
  // 左右 ALT 分别命名：alt 复用兼容旧配置（解析为通用 VK_MENU），
  // ralt/lalt 映射到 VK_RMENU/VK_LMENU，Typeless 等点按式输入法靠右ALT触发。
  if (lower == "ralt") return VK_RMENU;
  if (lower == "lalt") return VK_LMENU;
  if (lower == "shift") return VK_SHIFT;
  if (lower == "win" || lower == "windows" || lower == "command") return VK_LWIN;
  if (lower == "enter" || lower == "return") return VK_RETURN;
  if (lower == "space") return VK_SPACE;
  if (lower == "tab") return VK_TAB;
  if (lower == "esc" || lower == "escape") return VK_ESCAPE;
  if (lower == "backspace") return VK_BACK;
  if (lower == "delete" || lower == "del") return VK_DELETE;
  if (lower == "up") return VK_UP;
  if (lower == "down") return VK_DOWN;
  if (lower == "left") return VK_LEFT;
  if (lower == "right") return VK_RIGHT;

  if (lower.size() == 1) {
    const char ch = lower[0];
    // 字母：直接映射到 VK_A..VK_Z。
    if (ch >= 'a' && ch <= 'z') return 'A' + (ch - 'a');
    // 数字：映射到 VK_0..VK_9（主键盘区）。
    if (ch >= '0' && ch <= '9') return '0' + (ch - '0');
  }

  // F1..F24。
  if (lower.size() > 1 && lower[0] == 'f') {
    try {
      const int n = std::stoi(lower.substr(1));
      if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    } catch (...) {
    }
  }

  return 0;
}

bool SendInputForKeys(const std::vector<int>& vk_codes, bool key_up) {
  if (vk_codes.empty()) return false;

  std::vector<INPUT> inputs;
  inputs.reserve(vk_codes.size());
  for (int vk : vk_codes) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    DWORD flags = 0;
    // 右ALT(VK_RMENU)/右Ctrl(VK_RCONTROL) 是扩展键，必须加 KEYEVENTF_EXTENDEDKEY，
    // 否则系统会把它当成左ALT/左Ctrl，监听右ALT的第三方输入法（如 Typeless）不会触发。
    if (vk == VK_RMENU || vk == VK_RCONTROL) {
      flags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (key_up) flags |= KEYEVENTF_KEYUP;
    input.ki.dwFlags = flags;
    inputs.push_back(input);
  }

  const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(),
                              sizeof(INPUT));
  return sent == inputs.size();
}

}  // namespace

WechatInputMethodHotkey::WechatInputMethodHotkey(const std::string& hotkey) {
  const auto parts = Split(hotkey, '+');
  for (const auto& part : parts) {
    const int vk = VkCodeFromName(part);
    if (vk != 0) {
      vk_codes_.push_back(vk);
    }
  }
}

bool WechatInputMethodHotkey::SendDown() const {
  return SendInputForKeys(vk_codes_, false);
}

bool WechatInputMethodHotkey::SendUp() const {
  return SendInputForKeys(vk_codes_, true);
}

}  // namespace voicestick
