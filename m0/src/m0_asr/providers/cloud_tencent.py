"""腾讯云一句话识别 Provider（SentenceRecognition，同步 HTTPS）。

与桌面端流式（WebSocket）是不同接口，本 Provider 用于 M0 离线音频的
闭环对比评测——口径差异在评估报告中显式声明。

SDK 3.1.170 的请求字段为 Data/DataLen（非旧文档的 AudioData）。
支持 HotwordList 临时热词表（"词|权重"，请求级注入，云端不留存），
cloud_tencent_hotword 变体与本地 SeACo 读同一份 hotwords.txt，保证公平对比。

凭据来自 PlatformConfig 三级解析（env > m0/config.toml > VoiceStick config），
不落仓库。health_check 只验凭据完整性，不发起真实调用。
"""
from __future__ import annotations

import base64
import time
from pathlib import Path

from m0_asr.audio_utils import read_wave
from m0_asr.platform_config import CloudTencentCredentials

from .base import AsrProvider, HealthStatus, TranscribeOutcome

# 临时热词权重：10 为高权重普通热词（11 为超级热词，官方提示过多会影响整体字准率）
HOTWORD_WEIGHT = 10


class CloudTencentProvider(AsrProvider):
    """腾讯云一句话识别（16k_zh，音频数据 base64 上传，可选临时热词表）。"""

    kind = "cloud"
    name = "cloud_tencent"  # 热词变体实例在构造时覆盖

    def __init__(self, credentials: CloudTencentCredentials | None = None,
                 hotword_list: list[str] | None = None,
                 name: str = "cloud_tencent"):
        self.name = name
        self._credentials = credentials
        self.hotword_list = [w for w in (hotword_list or []) if w.strip()]

    def health_check(self) -> HealthStatus:
        if self._credentials is None:
            return HealthStatus(ok=False, detail="腾讯云凭据缺失（env/m0 config/VoiceStick config 三级解析均未命中）")
        detail = "凭据完整（不做真实调用验证）"
        if self.hotword_list:
            detail += f"；临时热词 {len(self.hotword_list)} 个"
        return HealthStatus(ok=True, detail=detail)

    def transcribe_file(self, wav_path: str | Path) -> TranscribeOutcome:
        # SDK 惰性导入：未安装时仅影响云端引擎，本地引擎不受牵连
        from tencentcloud.asr.v20190614 import asr_client, models
        from tencentcloud.common import credential
        from tencentcloud.common.exception.tencent_cloud_sdk_exception import (
            TencentCloudSDKException,
        )

        wav_path = Path(wav_path)
        if self._credentials is None:
            return TranscribeOutcome(text="", elapsed_seconds=0.0,
                                     audio_seconds=0.0, error="凭据缺失")
        started = time.perf_counter()
        try:
            raw = wav_path.read_bytes()
            cred = credential.Credential(self._credentials.secret_id,
                                         self._credentials.secret_key)
            client = asr_client.AsrClient(cred, self._credentials.region)
            req = models.SentenceRecognitionRequest()
            req.ProjectId = 0
            req.SubServiceType = 2  # 一句话识别
            # 官方字段名即 EngSerViceType（历史拼写），值如 16k_zh
            req.EngSerViceType = self._credentials.engine_model_type
            req.SourceType = 1  # 语音数据上传
            req.VoiceFormat = "wav"
            req.Data = base64.b64encode(raw).decode("ascii")
            req.DataLen = len(raw)
            if self.hotword_list:
                req.HotwordList = ",".join(f"{w}|{HOTWORD_WEIGHT}"
                                           for w in self.hotword_list)
            resp = client.SentenceRecognition(req)
            elapsed = time.perf_counter() - started
            samples, sample_rate = read_wave(wav_path)
            return TranscribeOutcome(text=resp.Result, elapsed_seconds=elapsed,
                                     audio_seconds=samples.size / sample_rate)
        except TencentCloudSDKException as exc:  # 云端错误码统一转 Outcome.error
            elapsed = time.perf_counter() - started
            return TranscribeOutcome(text="", elapsed_seconds=elapsed,
                                     audio_seconds=0.0,
                                     error=f"tencent sdk: {exc.code} {exc.message}")
        except Exception as exc:  # noqa: BLE001 - 评测管线要求失败不中断
            elapsed = time.perf_counter() - started
            return TranscribeOutcome(text="", elapsed_seconds=elapsed,
                                     audio_seconds=0.0, error=f"cloud error: {exc}")
