// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 将字符串形式的热键（如 "ctrl+win"）解析为虚拟键码序列，并通过 SendInput 注入。
// 用于 wechat_input_method 模式触发第三方输入法语音输入快捷键。

#ifndef VOICESTICK_WECHAT_INPUT_METHOD_HOTKEY_H_
#define VOICESTICK_WECHAT_INPUT_METHOD_HOTKEY_H_

#include <string>
#include <vector>

namespace voicestick {

// 解析热键字符串并执行按下/释放。
// 支持修饰符：ctrl、alt、shift、win（大小写不敏感）。
// 支持单字母/数字/功能键名称，例如 "f1"、"a"、"1"。
// IWechatInputMethodHotkey 是抽象接口，便于测试注入 fake 解耦 SendInput。
class IWechatInputMethodHotkey {
 public:
  virtual ~IWechatInputMethodHotkey() = default;
  virtual bool IsValid() const = 0;
  virtual bool SendDown() const = 0;
  virtual bool SendUp() const = 0;
  // 发送完整按下+释放（一次物理点击）。点按式第三方输入法（Typeless 等）靠完整
  // click 触发语音面板，仅按下不释放不会触发。hold_to_talk 仍用 SendDown/SendUp。
  virtual bool SendClick() const = 0;
};

class WechatInputMethodHotkey : public IWechatInputMethodHotkey {
 public:
  // 构造时解析 hotkey 字符串；解析失败时 IsValid() 返回 false。
  explicit WechatInputMethodHotkey(const std::string& hotkey);

  bool IsValid() const override { return !vk_codes_.empty(); }

  // 发送所有按键的按下序列（修饰符在前，普通键在后）。
  bool SendDown() const override;
  // 发送所有按键的释放序列（与按下顺序相反）。
  bool SendUp() const override;
  // 发送所有按键的按下+释放序列（一次 SendInput），模拟完整物理点击。
  bool SendClick() const override;

  // 返回解析到的虚拟键码数量。
  std::size_t KeyCount() const { return vk_codes_.size(); }

 private:
  std::vector<int> vk_codes_;
};

}  // namespace voicestick

#endif  // VOICESTICK_WECHAT_INPUT_METHOD_HOTKEY_H_
