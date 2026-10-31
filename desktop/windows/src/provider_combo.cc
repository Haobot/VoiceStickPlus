// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "provider_combo.h"

namespace voicestick {

// 云端段条目数：Volcengine + Tencent，legacy cloud 插入 0 号位时 +1。
namespace {
constexpr int kCloudEntries(bool has_cloud) { return has_cloud ? 3 : 2; }
}  // namespace

int ProviderComboLocalIndex(bool has_cloud) {
    return kCloudEntries(has_cloud);
}

bool ProviderComboIsLocal(int index, bool has_cloud) {
    return index == ProviderComboLocalIndex(has_cloud);
}

AsrProvider ProviderComboCloudAt(int index, bool has_cloud) {
    if (has_cloud) {
        if (index <= 0) return AsrProvider::kVoiceStickCloud;
        --index;
    }
    return index == 1 ? AsrProvider::kTencent : AsrProvider::kVolcengine;
}

int ProviderComboCloudIndexOf(AsrProvider provider, bool has_cloud) {
    if (provider == AsrProvider::kVoiceStickCloud) return 0;  // 仅 has_cloud 时存在
    const int idx = (provider == AsrProvider::kTencent) ? 1 : 0;
    return has_cloud ? idx + 1 : idx;
}

}  // namespace voicestick
