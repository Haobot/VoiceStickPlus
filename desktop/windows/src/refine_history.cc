#include "refine_history.h"

#include <chrono>

namespace voicestick {

std::int64_t RefineHistory::DefaultNowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

RefineHistory::RefineHistory(std::size_t max_turns, std::int64_t ttl_ms, NowMs now)
    : max_turns_(max_turns), ttl_ms_(ttl_ms), now_(std::move(now)) {}

void RefineHistory::Add(std::string raw_asr, std::string refined,
                        std::string instruction) {
    std::lock_guard lock(mutex_);
    ExpireIfStale();  // 过期后新轮从零起算，陈旧上下文不得混入
    last_add_ms_ = now_();
    turns_.push_back({std::move(raw_asr), std::move(refined),
                      std::move(instruction)});
    if (turns_.size() > max_turns_) {
        turns_.erase(turns_.begin());
    }
}

std::vector<RefineTurn> RefineHistory::Turns() const {
    std::lock_guard lock(mutex_);
    ExpireIfStale();
    return turns_;
}

std::string RefineHistory::ContextText() const {
    std::lock_guard lock(mutex_);
    ExpireIfStale();
    std::string out;
    for (std::size_t i = 0; i < turns_.size(); ++i) {
        if (i > 0) out += "。";
        out += turns_[i].refined;
    }
    return out;
}

void RefineHistory::Clear() {
    std::lock_guard lock(mutex_);
    turns_.clear();
}

// 调用方须持 mutex_（const 语义下由各公开方法内加锁后转调）
void RefineHistory::ExpireIfStale() const {
    if (!turns_.empty() && now_() - last_add_ms_ > ttl_ms_) {
        turns_.clear();
    }
}

} // namespace voicestick
