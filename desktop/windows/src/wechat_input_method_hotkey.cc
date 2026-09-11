// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "wechat_input_method_hotkey.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <string_view>

namespace voicestick {

namespace {

// SendInput 测试缝：非空时拦截全部注入（仅测试线程在 setup/teardown 时改写，
// 无并发）。定义于本匿名命名空间，SetSendInputForTest 直接改写。
WechatInputMethodHotkey::SendInputFn g_send_input_override = nullptr;

// 长按重复注入周期。物理长按时操作系统 auto-repeat 约 30 次/秒（33ms）；
// 实证 WeType 以 40ms 周期注入即可识别长按并弹语音面板（2026-09-11：
// 单次注入 2.5s 无面板，40ms 重复注入面板弹出）。
constexpr std::chrono::milliseconds kKeyDownRepeatInterval{40};

UINT WINAPI CallSendInput(UINT count, LPINPUT inputs, int size) {
  if (g_send_input_override) return g_send_input_override(count, inputs, size);
  return SendInput(count, inputs, size);
}

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

INPUT BuildKeyboardInput(int vk, bool key_up) {
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = static_cast<WORD>(vk);
  // 设 scan code：物理按键总带 scan code，某些输入法/应用靠低级键盘钩子的 scan code
  // （KBDLLHOOKSTRUCT.scanCode）识别按键，仅发 wVk（scan=0）可能不被识别。
  input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  DWORD flags = 0;
  // 右ALT(VK_RMENU)/右Ctrl(VK_RCONTROL) 是扩展键，必须加 KEYEVENTF_EXTENDEDKEY，
  // 否则系统会把它当成左ALT/左Ctrl，监听右ALT的第三方输入法（如 Typeless）不会触发。
  if (vk == VK_RMENU || vk == VK_RCONTROL) {
    flags |= KEYEVENTF_EXTENDEDKEY;
  }
  if (key_up) flags |= KEYEVENTF_KEYUP;
  input.ki.dwFlags = flags;
  return input;
}

bool SendInputs(std::vector<INPUT>& inputs) {
  if (inputs.empty()) return false;
  const UINT sent = CallSendInput(static_cast<UINT>(inputs.size()), inputs.data(),
                                  sizeof(INPUT));
  return sent == inputs.size();
}

bool SendInputForKeys(const std::vector<int>& vk_codes, bool key_up) {
  if (vk_codes.empty()) return false;

  std::vector<INPUT> inputs;
  inputs.reserve(vk_codes.size());
  for (int vk : vk_codes) {
    inputs.push_back(BuildKeyboardInput(vk, key_up));
  }
  return SendInputs(inputs);
}

}  // namespace

void WechatInputMethodHotkey::SetSendInputForTest(SendInputFn fn) {
  g_send_input_override = std::move(fn);
}

WechatInputMethodHotkey::WechatInputMethodHotkey(const std::string& hotkey) {
  const auto parts = Split(hotkey, '+');
  for (const auto& part : parts) {
    const int vk = VkCodeFromName(part);
    if (vk != 0) {
      vk_codes_.push_back(vk);
    }
  }
}

WechatInputMethodHotkey::~WechatInputMethodHotkey() {
  StopRepeat();
}

void WechatInputMethodHotkey::StopRepeat() const {
  std::lock_guard lock(repeat_mutex_);
  repeating_.store(false, std::memory_order_relaxed);
  if (repeat_thread_.joinable()) {
    repeat_thread_.join();
  }
}

bool WechatInputMethodHotkey::SendDown() const {
  if (!SendInputForKeys(vk_codes_, false)) return false;
  // 防御重入：上一轮 SendDown 未配对 SendUp 时先停旧线程再启动（与 StopRepeat
  // 共用 repeat_mutex_，避免并发 join 同一线程——点按折叠的自动松开线程与
  // 停止路径可能同时 SendUp）。
  {
    std::lock_guard lock(repeat_mutex_);
    repeating_.store(false, std::memory_order_relaxed);
    if (repeat_thread_.joinable()) {
      repeat_thread_.join();
    }
    repeating_.store(true, std::memory_order_relaxed);
    repeat_thread_ = std::thread([this] {
      while (repeating_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(kKeyDownRepeatInterval);
        if (!repeating_.load(std::memory_order_relaxed)) break;
        SendInputForKeys(vk_codes_, false);
      }
    });
  }
  return true;
}

bool WechatInputMethodHotkey::SendUp() const {
  StopRepeat();
  return SendInputForKeys(vk_codes_, true);
}

bool WechatInputMethodHotkey::SendClick() const {
  // 按下+释放序列（修饰符在前），一次 SendInput 发出，模拟完整物理点击。
  // 点按式第三方输入法靠完整 click 触发语音面板，仅按下不释放不会弹框。
  if (vk_codes_.empty()) return false;
  std::vector<INPUT> inputs;
  inputs.reserve(vk_codes_.size() * 2);
  for (int vk : vk_codes_) {
    inputs.push_back(BuildKeyboardInput(vk, false));
    inputs.push_back(BuildKeyboardInput(vk, true));
  }
  return SendInputs(inputs);
}

}  // namespace voicestick
