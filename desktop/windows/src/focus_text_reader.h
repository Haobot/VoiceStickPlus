#pragma once

// 聚焦输入框尾部文本 + 光标/选区读取（Windows UIA TextPattern）。
// 仅用于语音编辑 L1 校验（Doc/Plan/voice-edit-commands.md §6.3）：
//   GetFocusedElement → UIA_TextPatternId → get_DocumentRange 读尾部约 512 个
//   UTF-16 码元；GetSelection 要求恰 1 个折叠选区且末端 == 文档末端；
//   IsPassword 检查。
//
// 本头不包含 Win32/UIA 头：IFocusTextReader 由协调器（core）持有，生产实现
// CreateFocusTextReader() 在 focus_text_reader.cc 内用 UIA COM（必须在 UI 线程 /
// COM STA 调用，与 selection_hotword_manager 同范式）；测试注入 Fake 即可。

#include "voice_edit_command.h"

#include <cstddef>
#include <memory>

namespace voicestick {

// 整框域全文读取上限（UTF-16 code unit；与 macOS kVoiceTargetedFieldScopeMaxUTF16
// 同值）：超过按 field_too_long 拒绝（voice_targeted_domain.h）。
inline constexpr std::size_t kFieldScopeFullTextMaxUtf16 = 200000;

class IFocusTextReader {
public:
    virtual ~IFocusTextReader() = default;

    // 读取当前焦点控件状态，写入 out（先整体重置）。返回 out->readable。
    // 失败（无焦点/不支持 TextPattern/COM 失败）返回 false 且 out->readable=false，
    // 不抛异常；密码框返回 false 并置 out->is_password=true。
    // 只能在 UI 线程（COM STA）调用。
    virtual bool ReadFocusedField(VoiceEditFieldState* out) = 0;

    // 批次 2（整框搜索域）：读取字段**全文**（GetDocumentRange → GetText，上限
    // kFieldScopeFullTextMaxUtf16 码元）写入 out->full_text / full_utf16_length；
    // 超上限置 full_truncated=true 且 full_text 为空（调用方按 field_too_long
    // 拒绝）；尾窗/选区/光标字段与 ReadFocusedField 同步填充（一次 UIA 往返内
    // 完成，尾部窗口相关既有逻辑保持可用）。只能在 UI 线程调用。
    // TODO(win): 待 Windows 机编译+CTest 验证（全文 GetText 路径）。
    virtual bool ReadFocusedFieldFullText(VoiceEditFieldState* out) = 0;
};

// 生产实现（UIA）。须在 UI 线程调用 ReadFocusedField*。
std::unique_ptr<IFocusTextReader> CreateFocusTextReader();

// targeted 编辑平台出口（UIA TextRange 精确选区；Doc targeted 章节）：
// 只写选区、不写整个字段 Value；全部操作必须在 UI 线程（COM STA）。
class ITargetedFieldEditor {
public:
    virtual ~ITargetedFieldEditor() = default;

    // 选中字段尾部窗口内 [tail_relative_start, +length)（UTF-16 码元，相对
    // focus_text_reader 尾部窗口起点换算后的文档绝对位置由实现完成），并读回
    // 实际选中文本（UTF-8）写入 selected_text。成功返回 true；失败返回 false
    // （调用方按不可验证拒绝，绝不降级为盲操作/全选重贴/Ctrl+Z）。
    virtual bool SelectAndReadBack(std::size_t tail_relative_start_utf16,
                                   std::size_t length_utf16,
                                   std::string* selected_text) = 0;

    // 批次 2（整框搜索域）：以**文档绝对偏移**设置选区并读回（整框域全文坐标；
    // 与尾窗坐标无关）。TODO(win): 待 Windows 机编译+CTest 验证。
    virtual bool SelectAndReadBackAbsolute(std::size_t abs_start_utf16,
                                            std::size_t length_utf16,
                                            std::string* selected_text) = 0;

    // 光标折叠定位到文档末尾（修改后的安全光标恢复）。
    virtual bool MoveCaretToEnd() = 0;

    // 批次 1（字段内定位）：光标折叠定位到尾部窗口内偏移 tail_relative_utf16
    //（MoveCaretToEnd 的泛化；尾删前置光标移动用）。失败返回 false（调用方按
    // caret_move_failed 拒绝，零退格）。
    virtual bool MoveCaretTo(std::size_t tail_relative_utf16) = 0;

    // 批次 2（整框搜索域）：光标折叠定位到**文档绝对偏移**（整框域光标恢复用）。
    // TODO(win): 待 Windows 机编译+CTest 验证。
    virtual bool MoveCaretToAbsolute(std::size_t abs_utf16) = 0;
};

// 生产实现（UIA TextPattern）。须在 UI 线程调用。
std::unique_ptr<ITargetedFieldEditor> CreateTargetedFieldEditor();

}  // namespace voicestick
