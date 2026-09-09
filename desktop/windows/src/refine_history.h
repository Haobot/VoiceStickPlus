#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace voicestick {

struct RefineTurn {
    std::string raw_asr;    // 当轮 ASR 原文（诊断用）
    std::string refined;    // 当轮精修结果（跨轮上下文以精修版为准，方案 §3.4）
    // 当轮模型指令输出（纠正指令管线；空 = 无指令/单句管线轮次）。
    // KV 续写重放的 assistant 侧用它（形态自洽，防模型漂移为文本输出——
    // 重放 refined 文本实测 3 轮起漂移，smoke 2026-09-10）。
    std::string instruction;
};

// 跨轮精修历史环形缓冲：全局最近 5 轮，2 分钟无新轮即整体过期。
// 会话取消/失败不调用 Add（由协调器保证，缓冲本身不感知会话状态）。
// 时钟可注入（测试用）；过期在读取时惰性判定并清空。
// 线程安全：协调器在会话回调线程读 Turns()、在精修完成的后台线程 Add。
class RefineHistory {
public:
    using NowMs = std::function<std::int64_t()>;

    explicit RefineHistory(std::size_t max_turns = 5,
                           std::int64_t ttl_ms = 120'000,
                           NowMs now = DefaultNowMs);

    void Add(std::string raw_asr, std::string refined,
             std::string instruction = {});

    // 过期则惰性清空并返回空；否则返回按时间序的最近 <=max_turns 轮。
    std::vector<RefineTurn> Turns() const;

    // 跨轮上文文本：各轮 refined 以「。」拼接（spike/生产 prompt 同款口径）。
    std::string ContextText() const;

    void Clear();

    static std::int64_t DefaultNowMs();

private:
    // 调用方须持 mutex_（公开方法加锁后转调）
    void ExpireIfStale() const;

    std::size_t max_turns_;
    std::int64_t ttl_ms_;
    NowMs now_;
    mutable std::mutex mutex_;
    mutable std::vector<RefineTurn> turns_;
    mutable std::int64_t last_add_ms_ = 0;
};

} // namespace voicestick
