# M0 模型清单

> 由 scripts/download_models.py 自动生成；提交进 git，权重本体被 gitignore。

## sense_voice — `sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17`

- 许可证: **Apache-2.0**（FunAudioLLM/SenseVoice-Small 权重，sherpa-onnx 官方 int8 转换）
- 状态: 已下载
- 文件:
  - `export-onnx.py` (0.0 MB) sha256=`-`
  - `LICENSE` (0.0 MB) sha256=`-`
  - `model.int8.onnx` (239.2 MB) sha256=`c71f0ce00bec95b07744e116345e33d8cbbe08cef896382cf907bf4b51a2cd51`
  - `README.md` (0.0 MB) sha256=`-`
  - `test_wavs/en.wav` (0.2 MB) sha256=`-`
  - `test_wavs/ja.wav` (0.2 MB) sha256=`-`
  - `test_wavs/ko.wav` (0.1 MB) sha256=`-`
  - `test_wavs/yue.wav` (0.2 MB) sha256=`-`
  - `test_wavs/zh.wav` (0.2 MB) sha256=`-`
  - `tokens.txt` (0.3 MB) sha256=`-`

## seaco_paraformer — `sherpa-onnx-paraformer-trilingual-zh-cantonese-en`

- 许可证: **Model License Agreement (ModelScope)**（转换自 ModelScope dengcunqin/speech_seaco_paraformer_large_asr_nat-zh-cantonese-en-16k-common-vocab11666-pytorch（SeACo-Paraformer，上游遵循 FunASR/ModelScope 模型协议，商用前须通读协议原文））
- 状态: 已下载
- 文件:
  - `model.int8.onnx` (244.7 MB) sha256=`eb3cdd288f535cf73258f491cdd7d68ad5a00aee135c0bba4c0884ea8d926144`
  - `tokens.txt` (0.1 MB) sha256=`-`

## qwen3_asr — `sherpa-onnx-qwen3-asr-0.6B-int8-2026-03-25`

- 许可证: **Apache-2.0**（QwenLM/Qwen3-ASR 0.6B 权重，社区 int8 导出（zengshuishui/Qwen3-ASR-onnx））
- 状态: 已下载
- 文件:
  - `conv_frontend.onnx` (44.1 MB) sha256=`d22dc4423e0940e49884e903d2ea2f7e5567c14fc1aed97e4e26d6b8f208ef9e`
  - `decoder.int8.onnx` (756.6 MB) sha256=`61e5f8249f9e7c82d5e01e1938c79fb3f5b3135f91664928033029e42451bd18`
  - `encoder.int8.onnx` (182.5 MB) sha256=`60748d3e6744a57c9c91e1b17424a6c2990567e8adceb0783940c03ed98fa9d9`
  - `tokenizer/merges.txt` (1.7 MB) sha256=`-`
  - `tokenizer/tokenizer_config.json` (0.0 MB) sha256=`-`
  - `tokenizer/vocab.json` (2.8 MB) sha256=`-`
