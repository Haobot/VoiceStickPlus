#!/usr/bin/env python3
"""一次性：按 sidecar 从 session_1.adpcm 复现解码，先断言与已提交
session_1.raw.wav 逐样本一致（验证解码分帧正确），再按 sidecar gain_db
跑新 postprocess 链覆写 session_1.wav（.adpcm/.raw.wav/sidecar 不动）。"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from atvv_capture import (ImaAdpcmDecoder, postprocess, read_wav_samples,
                          write_wav)

d = HERE / "fixtures" / "xiaomi" / "demo_synthetic"
sidecar = json.loads((d / "session_1.json").read_text("utf-8"))
adpcm = (d / sidecar["files"]["adpcm"]).read_bytes()

dec = ImaAdpcmDecoder()
pcm = []
for seg in sidecar["segments"]:
    dec.reset(seg["predictor"], seg["step_index"])
    pcm += dec.decode(adpcm[seg["offset"]:seg["offset"] + seg["bytes"]])

raw = read_wav_samples(d / sidecar["files"]["raw_wav"])
assert pcm == raw, f"decode != raw.wav: {len(pcm)} vs {len(raw)}"
print(f"decode == raw.wav OK ({len(pcm)} samples)")

out = postprocess(pcm, sidecar["gain_db"])
write_wav(d / sidecar["files"]["wav"], out, sidecar["sample_rate"])
print(f"session_1.wav regenerated: gain_db={sidecar['gain_db']} "
      f"peak={max(abs(s) for s in out)}")
