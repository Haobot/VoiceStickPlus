// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 本地模型分发清单（Doc/Plan/local-model-distribution.md §4）：编译期内置
// 常量表，描述 SenseVoice 与 Qwen3 GGUF 两个条目的文件构成、期望体积/哈希
// 与多源下载 URL。模型源变动频率远低于应用发版，不做远端清单。

#ifndef VOICESTICK_MODEL_MANIFEST_H_
#define VOICESTICK_MODEL_MANIFEST_H_

#include <cstdint>
#include <string>
#include <vector>

namespace voicestick {

// 条目种类：kAsr = SenseVoice（本地识别必需）；kRefine = Qwen3 GGUF（可选，
// 缺失时精修降级纯规则层，向导允许跳过）。
enum class ModelKind { kAsr, kRefine };

// 单个模型文件的分发描述。
struct ModelFileSpec {
    // 相对 models_dir 的路径（POSIX 分隔符），如
    // "Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf"。
    std::string rel_path;
    // 期望字节数：磁盘预检与响应体超长/截断判定的依据。
    std::uint64_t bytes = 0;
    // 期望 SHA-256（小写 hex）：下载完整性的最终底线，源顺序不影响安全性。
    std::string sha256;
    // 下载源 URL，按优先级排列（OSS → ModelScope → GitHub Release）。
    std::vector<std::string> urls;
};

// 一组共同构成某功能的模型文件。
struct ModelEntrySpec {
    ModelKind kind = ModelKind::kAsr;
    // false = 缺失时功能降级可用（精修退规则层），下载向导允许不勾选。
    bool required = true;
    std::vector<ModelFileSpec> files;
};

// 内置模型清单。哈希取自本仓 m0/models 权威副本实测；OSS URL 为占位，
// 迭代三托管落地后回填；ModelScope 直链发布前须核实远端文件与本仓副本
// 逐字节一致（哈希校验兜底，不一致仅表现为该源失败回退）。
const std::vector<ModelEntrySpec>& BundledModelEntries();

// 清单自洽校验：rel_path 非空、bytes > 0、sha256 为 64 位小写 hex、urls
// 非空且均为 http(s) 绝对地址。向导启动前与单测共用。
bool ModelEntriesWellFormed(const std::vector<ModelEntrySpec>& entries);

}  // namespace voicestick

#endif  // VOICESTICK_MODEL_MANIFEST_H_
