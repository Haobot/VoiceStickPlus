#pragma once

// 字段内定位器（语音编辑批次 1：输入框内定位 P1 + 句中插入去句号）——纯逻辑层。
// 与 macOS desktop/macos/Sources/VoiceStickCore/VoiceEditAnchor.swift 同构：
// 同一判定顺序（P1a 光标前缀锚 → P1b 光标后缀锚 → P1c 字段末尾锚 → P2 唯一出现
// 锚）、零容错（禁止模糊/最长前缀匹配）、UTF-16 码元精确相等口径（UTF-8 输入，
// 内部换算 UTF-16 偏移，输出供 UIA 选区用）。
//
// 本文件与 voice_edit_anchor.cc 不依赖任何 Win32/平台头，可在 macOS 上用
// clang++ -std=c++20 编译并链接本地 main 跑通全部测试向量（双端同表）。
//
// 另含交付 C 纯函数：StripMidlinePeriod（句中插入去句末句号）与
// IsMidlineInsertion（句中判定）。

#include <cstddef>
#include <string>
#include <string_view>

namespace voicestick {

struct VoiceEditAnchor {
    enum class Kind {
        kCaretPrefix,
        kCaretSuffix,
        kFieldEnd,
        kUniqueOccurrence,
        // 批次 2（整框搜索域）：合成锚——域文本 = 字段全文（起点 0、终点 = 全长）。
        // 不来自 Locate，仅供 planner 可读性守卫与执行上下文携带域信息。
        kWholeField,
    };
    Kind kind = Kind::kFieldEnd;
    std::size_t anchor_start = 0;  // anchored 段起点（UTF-16 偏移）
    std::size_t anchor_end = 0;    // = anchor_start + anchored 的 UTF-16 长度
};

enum class VoiceEditAnchorFailure {
    kAnchorAmbiguous,  // anchored 在字段中出现 ≥2 次（且 P1 全 miss）
    kFieldMismatch,    // 整段精确匹配全 miss（用户改过段内文本等）
};

struct VoiceEditAnchorResult {
    bool ok = false;
    VoiceEditAnchorFailure failure = VoiceEditAnchorFailure::kAnchorAmbiguous;
    VoiceEditAnchor anchor;
};

// 字段内多级锚定。anchored（remaining + 已记录换行）由调用方构造好传入
//（pressedEnter 时为 remaining + "\n"，否则 remaining）；field_utf8 为字段全文
//（或尾部窗口）；caret_utf16 / selection_len 与 anchored 同基准（窗口内相对偏移）。
// 调用方保证字段可读（读不到 = 不可验证，不进入本函数）。
// anchored_has_recorded_newline：anchored 末尾那个 \n 是否为 auto_enter 已记录
// 换行——true 时 P1c 启用「裸 remaining 回落」（现行后缀校验等价语义）；false 时
// P1c 仅判 anchored 整段后缀（不剥 remaining 自身以换行结尾的口述换行）。
VoiceEditAnchorResult LocateVoiceEditAnchor(std::string_view field_utf8,
                                             std::size_t caret_utf16,
                                             std::size_t selection_len,
                                             std::string_view anchored_utf8,
                                             bool anchored_has_recorded_newline = false);

// P1c 字段末尾锚（现行后缀校验等价实现，供旧 planner 通道复用）：
// pressed_enter 时优先按「remaining+"\n"」判后缀，回落裸 remaining。命中时
// anchor_end 恒为字段末尾；带换行命中的锚段长度 = remaining+1，裸命中 = remaining
// 本身——调用方据此推导 has_recorded_newline。remaining_utf8 非空由调用方保证。
VoiceEditAnchor LocateVoiceEditFieldEndAnchor(std::string_view field_utf8,
                                               std::string_view remaining_utf8,
                                               bool pressed_enter);

// ---- 交付 C：句中插入去句末句号（纯函数；与 macOS 同构）----

// 句中判定：光标（折叠）之后同一逻辑行还有非空白字符（光标后第一个字符存在、
// 不是 \n/\r、非空白）。
bool IsMidlineInsertion(std::string_view field_utf8, std::size_t caret_utf16);

// 句中插入且文本以一个句号结尾 → 去掉末尾一个字符。句号集合：。(U+3002)、
// ．(U+FF0E) 直接剥；.(U+002E) 仅当前一字符不是 ASCII 字母/数字（保「3.」「v1.」）；
// ！？等语气标点不去。裸句号（句号前无实际内容，如标点独词注入的「。」）恒保留。
std::string StripMidlinePeriod(std::string_view text_utf8, bool is_midline);

// 快速预检：文本以可剥离候选句号结尾（协调器据此避免不必要的字段读取）。
bool MidlinePeriodStrippable(std::string_view text_utf8);

}  // namespace voicestick
