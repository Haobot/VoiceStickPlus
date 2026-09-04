# M0 开发坑记录（验收材料的一部分）

> 记录开发过程中实际踩到的坑与解决过程。按时间顺序追加，不复盘不美化。

## 1. sherpa-onnx 导出的 SeACo 模型不支持热词偏置（路线一第一次尝试失败）

**现象**：用 sherpa-onnx 的 `sherpa-onnx-paraformer-trilingual-zh-cantonese-en`（转换自 ModelScope
SeACo-Paraformer 检查点）加载后调用 `create_stream(hotwords="Kubernetes/梓骞")`，直接抛错：

```
offline-recognizer-impl.h:CreateStream:38 Only transducer models support contextual biasing.
```

**根因**：sherpa-onnx 的热词（Aho-Corasick 上下文偏置）**只在 transducer 模型上实现**，且要求
`modified_beam_search` 解码。SeACo-Paraformer 的 SeACo 偏置模块在导出 ONNX 时未被包含/接入
sherpa-onnx 运行时——"模型叫 SeACo"和"运行时支持 SeACo 偏置"是两回事。

**解决**：路线一回归提示词 B 原文指定的 FunASR 生态——`funasr.AutoModel` 加载
`iic/speech_seaco_paraformer_large_asr_nat-zh-cn-16k-common-vocab8404-pytorch`，
`generate(hotword="词1 词2")` 原生支持热词。sherpa-onnx trilingual 包保留在 models/ 作为该
结论的实证对照（其无热词识别仍可用）。

**教训**：选运行时要查"目标特性"的实现矩阵，不能只看模型名字；官方文档的
Hotwords (Contextual biasing) 页面其实写明了 "Only transducer models support hotwords"，
初读时误以为 SeACo 有独立通道。

## 2. GitHub Releases 国内下载速度不稳定

**现象**：SenseVoice 包（163MB）GitHub 直连约 1MB/s（3 分钟）可完成；SeACo 包（~840MB）
下载速度掉到 10KB/s 级，几乎不可用。

**解决**：hf-mirror.com 逐文件下载实测 **16.3MB/s**（快百倍），且只需下载 int8 权重
（234MB）而非整个 tar（含 fp32 共 1.1GB）。下载脚本已实现三级源回退：
ModelScope snapshot → hf-mirror 文件级 → GitHub Releases tar。

## 3. jiwer 3.x API 细节

- `jiwer.cer()` 的自定义 transform 必须能"reduce 到 list of list of chars"——空 `Compose([])`
  会抛 `ValueError`。正确写法：`jiwer.Compose([jiwer.ReduceToListOfListOfChars()])`。
- `Compose` 在 `jiwer` 顶层命名空间，不在 `jiwer.transformations`。
- 为了口径统一，我们先做自己的归一化（NFKC 全角折叠 + 去标点 + lower）再喂 jiwer，
  自实现编辑距离与 jiWER 结果完全一致（交叉验证通过）。

## 4. sherpa-onnx Python API 的 pybind 构造细节

- 直接 `OfflineRecognizer(config)` 报 `takes no arguments`——公开入口是包装层的工厂方法
  （`from_sense_voice` / `from_paraformer` / `from_qwen3_asr` / ...），pybind 类是私有的。
- `OfflineRecognizerConfig(model=...)` 报 incompatible constructor——pybind 关键字名是
  `model_config`。优先用工厂方法可以完全绕开这些细节。

## 5. Qwen3-ASR 热词只能走构造级注入

per-stream `create_stream(hotwords=...)` 对 Qwen3-ASR 同样抛 "Only transducer models"
（同坑 1）。Qwen3-ASR 的热词形态是**构造级** `from_qwen3_asr(hotwords="词1,词2")`
（逗号分隔，写进 context prompt）。产品形态上等价：重建轻量 recognizer 配置即可切换
热词集，权重常驻内存不重载。

## 6. edge-tts 偶发 "No audio was received"

29 条音频生成 1 条失败（服务端瞬时故障），重跑生成脚本即可（幂等跳过已有文件）。

## 7. TTS 测试音频的口径边界（非坑，是边界声明）

edge-tts 音频为标准发音、无背景噪声，等价"安静环境朗读"条件。真实嘈杂/口音场景的
绝对 CER 会更高，但**热词双跑对比**（同音频基线 vs 偏置）的相对降幅结论不受影响。
"耳语基线测试"（M0 另一项生死判据）必须用真实耳语录音，不能用 TTS 替代。

## 8. 汉字数字 vs 阿拉伯数字的 ITN 差异计入 CER（从严口径）

SenseVoice 的 ITN 会把"下午三点"转成"下午3点"。归一化未实现中文数字等价转换，
该差异按从严口径计入 CER（daily_03 因此 3.1%）。结论不受影响（总体 CER 2.45% < 5%），
报告口径说明已如实标注。
