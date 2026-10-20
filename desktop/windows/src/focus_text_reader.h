#pragma once

// 聚焦输入框尾部文本 + 光标/选区读取（Windows UIA TextPattern）。
// 仅用于语音编辑 L1 校验（Doc/Plan/voice-edit-commands.md §6.3）：
//   GetFocusedElement → UIA_TextPatternId → GetDocumentRange 读尾部约 512 个
//   UTF-16 码元；GetSelection 要求恰 1 个折叠选区且末端 == 文档末端；
//   IsPassword 检查。
//
// 本头不包含 Win32/UIA 头：IFocusTextReader 由协调器（core）持有，生产实现
// CreateFocusTextReader() 在 focus_text_reader.cc 内用 UIA COM（必须在 UI 线程 /
// COM STA 调用，与 selection_hotword_manager 同范式）；测试注入 Fake 即可。

#include "voice_edit_command.h"

#include <memory>

namespace voicestick {

class IFocusTextReader {
public:
    virtual ~IFocusTextReader() = default;

    // 读取当前焦点控件状态，写入 out（先整体重置）。返回 out->readable。
    // 失败（无焦点/不支持 TextPattern/COM 失败）返回 false 且 out->readable=false，
    // 不抛异常；密码框返回 false 并置 out->is_password=true。
    // 只能在 UI 线程（COM STA）调用。
    virtual bool ReadFocusedField(VoiceEditFieldState* out) = 0;
};

// 生产实现（UIA）。须在 UI 线程调用 ReadFocusedField。
std::unique_ptr<IFocusTextReader> CreateFocusTextReader();

}  // namespace voicestick
