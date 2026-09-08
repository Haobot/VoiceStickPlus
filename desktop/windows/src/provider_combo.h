// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 语音识别提供方下拉框索引映射（设置页「本机麦克风」合并进「语音识别」，
// Doc/Plan/asr-settings-local-provider-merge.md）：
// 条目顺序 = [VoiceStick Cloud（仅 asr_provider 老配置为 cloud 时临时插 0 号位）]
// + Volcengine + Tencent Cloud ASR + 「本地语音识别」末位虚拟项。
// 虚拟项不进 AsrProvider 枚举：选中映射 [local_asr].enabled，asr_provider 保留
// 云端值驱动设备链路（两者正交，见设计文档 D1）。

#pragma once

#include "app_config.h"

namespace voicestick {

// 本地项在下拉框中的索引（固定末位）。has_cloud 见上。
int ProviderComboLocalIndex(bool has_cloud);

// index 是否为本地虚拟项（含 CB_ERR=-1 等越界防御：一律非本地）。
bool ProviderComboIsLocal(int index, bool has_cloud);

// 云端段映射：index（云端条目索引）→ AsrProvider。越界兜底 kVolcengine。
AsrProvider ProviderComboCloudAt(int index, bool has_cloud);

// 云端段逆映射：AsrProvider → 下拉框索引。
int ProviderComboCloudIndexOf(AsrProvider provider, bool has_cloud);

}  // namespace voicestick
