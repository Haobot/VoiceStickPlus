"""ASR 离线评测基准库。

复用 scripts/e2e_test/ 下既有回放脚本的协议实现（不伪造结果，直连真实 ASR）：
- volcengine.py：火山引擎 BidirectionalASR（bigmodel_async 可复用连接协议）
- tencent.py：腾讯实时语音识别（asr/v2 WebSocket，opus 封装与桌面端一致）
- wsproto.py：纯 stdlib 的 WebSocket 客户端（raw socket + ssl）
- metrics.py：CER（字准）、延迟分位数、跨轮抖动统计
- report.py：JSON + Markdown 报告生成

凭据一律从 %APPDATA%/VoiceStick/config.toml 读取，不打印不输出。
"""
