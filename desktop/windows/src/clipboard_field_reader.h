#pragma once

#include <memory>
#include <string>

namespace voicestick {

// 剪贴板回读通道：UIA TextPattern 不可用时的兜底读通道。
// 背景：微信 4.2.0.8 的 MMUI 自绘窗口不暴露 UIA 树（2026-11-04 真机实锤，见
// Doc/Expe/wechat-mmui-uia-ondemand-cold-tree-2026-11-04.md 与
// Doc/Plan/wechat-clipboard-readback.md）。该通道用 Ctrl+A / Ctrl+C + 剪贴板
// 回读拿字段全文，供 targeted 整框域（field_scope）使用。
//
// 局限（调用方必须知道）：
//  - 只能拿全文，拿不到光标位置/选区状态 → 只能用于整框域；
//  - 读取会改变目标选区（Ctrl+A 全选，读后右方向键取消选区、光标落末尾）；
//  - 无法区分密码框（UIA 已判密码框的路径不启用本通道）。
class IClipboardFieldReader {
public:
    virtual ~IClipboardFieldReader() = default;

    // 读当前焦点字段全文（UTF-8）。空字段 → 返回 true 且 out 为空串。
    // 以下返回 false：剪贴板快照失败（被占用）/ Ctrl+C 后剪贴板未更新 /
    // 读不到 CF_UNICODETEXT。须在 UI 线程调用（内部 SendInput）。
    virtual bool ReadFieldFullText(std::string* out) = 0;
};

// 生产实现（Win32 SendInput + 剪贴板快照）。须在 UI 线程调用。
std::unique_ptr<IClipboardFieldReader> CreateClipboardFieldReader();

}  // namespace voicestick
