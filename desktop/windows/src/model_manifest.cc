// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// model_manifest.h 的实现：内置常量表 + 自洽校验。

#include "model_manifest.h"

#include <cctype>

namespace voicestick {

namespace {

bool IsLowerHex(std::string_view text) {
    if (text.size() != 64) return false;
    for (const char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch)) &&
            (ch < 'a' || ch > 'f')) {
            return false;
        }
    }
    return true;
}

bool IsHttpUrl(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

// 分发源与桶布局见 Doc/Ref/cos-distribution.md（权威事实参考）。
// 回退顺序：正式域名直出（DNS 未配时快速失败自动回退，就绪后无需改清单）
// → myqcloud 直出 → ModelScope 免费分流 → GitHub Release 海外回退。
const char* kCosPrimaryBase = "https://dl.davenger.cloud/models";
const char* kCosDirectBase =
    "https://voicestick-dl-1259040144.cos.ap-shanghai.myqcloud.com/models";
const char* kCosModelsPrefix = "/sense-voice-int8-2024-07-17";

ModelEntrySpec MakeAsrEntry() {
    const char* kModelScopeAsr =
        "https://modelscope.cn/models/pengzhendong/"
        "sherpa-onnx-sense-voice-zh-en-ja-ko-yue/resolve/master";
    const char* kGitHubBase =
        "https://github.com/Haobot/VoiceStickPlus/releases/download/model-pack-v1";

    ModelFileSpec onnx;
    onnx.rel_path = "model.int8.onnx";
    onnx.bytes = 239233841;
    onnx.sha256 =
        "c71f0ce00bec95b07744e116345e33d8cbbe08cef896382cf907bf4b51a2cd51";
    onnx.urls = {
        std::string(kCosPrimaryBase) + kCosModelsPrefix + "/model.int8.onnx",
        std::string(kCosDirectBase) + kCosModelsPrefix + "/model.int8.onnx",
        std::string(kModelScopeAsr) + "/model.int8.onnx",
        std::string(kGitHubBase) + "/sense-voice-int8-2024-07-17/model.int8.onnx",
    };

    ModelFileSpec tokens;
    tokens.rel_path = "tokens.txt";
    tokens.bytes = 315894;
    tokens.sha256 =
        "f449eb28dc567533d7fa59be34e2abca8784f771850c78a47fb731a31429a1dc";
    tokens.urls = {
        std::string(kCosPrimaryBase) + kCosModelsPrefix + "/tokens.txt",
        std::string(kCosDirectBase) + kCosModelsPrefix + "/tokens.txt",
        std::string(kModelScopeAsr) + "/tokens.txt",
        std::string(kGitHubBase) + "/sense-voice-int8-2024-07-17/tokens.txt",
    };

    ModelEntrySpec entry;
    entry.kind = ModelKind::kAsr;
    entry.required = true;
    entry.files = {std::move(onnx), std::move(tokens)};
    return entry;
}

ModelEntrySpec MakeRefineEntry() {
    const char* kModelScopeGguf =
        "https://modelscope.cn/models/unsloth/Qwen3-1.7B-GGUF/resolve/master";
    const char* kGitHubBase =
        "https://github.com/Haobot/VoiceStickPlus/releases/download/model-pack-v1";

    ModelFileSpec gguf;
    gguf.rel_path = "Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf";
    gguf.bytes = 1107409472;
    gguf.sha256 =
        "b139949c5bd74937ad8ed8c8cf3d9ffb1e99c866c823204dc42c0d91fa181897";
    gguf.urls = {
        std::string(kCosPrimaryBase) + kCosModelsPrefix +
            "/Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf",
        std::string(kCosDirectBase) + kCosModelsPrefix +
            "/Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf",
        std::string(kModelScopeGguf) + "/Qwen3-1.7B-Q4_K_M.gguf",
        std::string(kGitHubBase) + "/Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf",
    };

    ModelEntrySpec entry;
    entry.kind = ModelKind::kRefine;
    entry.required = false;
    entry.files = {std::move(gguf)};
    return entry;
}

}  // namespace

const std::vector<ModelEntrySpec>& BundledModelEntries() {
    static const std::vector<ModelEntrySpec> kEntries = {
        MakeAsrEntry(),
        MakeRefineEntry(),
    };
    return kEntries;
}

bool ModelEntriesWellFormed(const std::vector<ModelEntrySpec>& entries) {
    if (entries.empty()) return false;
    for (const auto& entry : entries) {
        if (entry.files.empty()) return false;
        for (const auto& file : entry.files) {
            if (file.rel_path.empty() ||
                file.rel_path.find('\\') != std::string::npos) {
                return false;
            }
            if (file.bytes == 0) return false;
            if (!IsLowerHex(file.sha256)) return false;
            if (file.urls.empty()) return false;
            for (const auto& url : file.urls) {
                if (!IsHttpUrl(url)) return false;
            }
        }
    }
    return true;
}

}  // namespace voicestick
