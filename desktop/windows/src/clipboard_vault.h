#pragma once

#include <Windows.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace voicestick {

// 剪贴板完整格式快照：逐格式字节级（句柄类格式除外）。
struct ClipboardSnapshot {
    struct Entry {
        UINT format;
        std::string name;  // 标准名或注册名（诊断用）
        std::vector<std::uint8_t> data;
    };
    std::vector<Entry> entries;

    const Entry* Find(UINT format) const;
};

// 借道剪贴板的注入（写文本 + Ctrl+V）用后完整还原用户原内容——文本以外的
// 格式（图片/文件列表/HTML）若只做文本恢复会被永久覆盖。移植自 P1
// clipboard_vault（真机 48/48 验证）：跳过句柄类格式 CF_BITMAP/
// CF_METAFILEPICT/CF_PALETTE/CF_ENHMETAFILE（非 HGLOBAL 无法字节快照；位图
// 场景应用几乎都同时提供 CF_DIB 内存版，丢失为已知限制）。
class ClipboardVault {
public:
    // 打开剪贴板带重试（并发占用是现实，P1 经验 8 次 × 25ms）。
    explicit ClipboardVault(int open_retries = 8, int retry_delay_ms = 25);

    // 完整格式快照。打开失败抛 std::runtime_error——空快照只代表真空剪贴板，
    // 混入“打不开”会让 Restore 误清用户剪贴板。
    ClipboardSnapshot Save();

    // 写回快照全部格式；打开失败返回 false。
    bool Restore(const ClipboardSnapshot& snapshot);

private:
    bool OpenWithRetry();

    int open_retries_;
    int retry_delay_ms_;
};

} // namespace voicestick
