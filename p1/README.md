# VoiceStick P1 — 桌面语音输入 MVP（Windows 先行）

按住说话 → 松手出字 的本地优先语音输入客户端。路线图 v1.3 §四 P1 第一迭代交付物，
技术方案见 [docs/design-p1-mvp.md](docs/design-p1-mvp.md)。

## 功能

- **全局热键**：按住 `右 Ctrl` 说话，松手触发识别；`Esc` 取消本句（丢弃音频）。
- **悬浮条**：屏幕顶部中央状态条（录音中=粉 / 识别中=蓝 / 结果=绿 / 错误=橙），结果停留 4 秒。
- **本地识别**：SenseVoice-Small int8（sherpa-onnx），完全离线。
- **热词飞轮**：SQLite + AES-GCM 字段加密热词库；识别后处理纠正（拉丁精确/模糊 + 中文拼音滑窗），
  命中回写权重（+0.05，上限 2.0），下一句立即生效。
- **AI 改写**：默认规则版（去口癖/叠字折叠/标点规整）；可选腾讯混元（配置凭据后自动启用）。
- **文本注入**：剪贴板 + Ctrl+V 到当前焦点窗口。

## 快速开始

依赖 m0/ 的虚拟环境与模型权重（不重复下载 ~3GB）：

```bash
# 1. 准备（一次性）
cd ../m0 && .venv/Scripts/pip install -r ../p1/requirements.txt
.venv/Scripts/python.exe scripts/download_models.py --only sense_voice   # 若模型未就位

# 2. 启动
cd ../p1 && ../m0/.venv/Scripts/python.exe main.py
```

按住 `右 Ctrl` 对麦克风说话，松手后文本自动粘贴到焦点窗口。`Ctrl+C` 退出。

## 配置

`p1/config.toml`（gitignored，缺省全部用默认值），字段见 [config.example.toml](config.example.toml)：

| 字段 | 默认 | 说明 |
|---|---|---|
| `hotkey.push_to_talk` | `right ctrl` | 按住说话键（keyboard 库键名） |
| `hotkey.cancel` | `esc` | 取消本句键 |
| `engine.adapter` | `sense_voice` | 识别引擎（P1 仅此一种） |
| `engine.models_dir` | `../m0/models` | 模型权重目录（与 M0 共享） |
| `rewrite.provider` | `rules` | `rules` 或 `hunyuan`（后者需凭据） |
| `hotword.db_path` | `%APPDATA%/VoiceStickP1/hotwords.db` | 热词库位置 |

混元凭据走环境变量 `HUNYUAN_SECRET_ID` / `HUNYUAN_SECRET_KEY`（与 m0 一致，不进仓库）。

## 目录结构

```text
src/p1/
├── interaction/    # 热键监听、悬浮条 UI、文本注入（OS 边界）
├── orchestration/  # 录音会话、识别管线编排
├── flywheel/       # 热词库（加密存储）、后处理纠正器
├── rewrite/        # 规则改写器、混元改写器
├── engines/        # 引擎适配器（capabilities 自描述）
└── controller.py   # 热键→会话→管线→UI 粘合状态机
```

## 测试与验收

```bash
# 单元/集成测试（65 个，无凭据/模型时自动 SKIP 相关项）
../m0/.venv/Scripts/python.exe -m pytest tests/ -q

# 全真链路自检（真实 wav → 识别 → 纠正 → 改写 → 剪贴板，不粘贴）
../m0/.venv/Scripts/python.exe scripts/e2e_selftest.py

# 悬浮条视觉冒烟（截图三态到 docs/shots/）
../m0/.venv/Scripts/python.exe scripts/overlay_smoke.py
../m0/.venv/Scripts/python.exe scripts/verify_overlay_shots.py

# 真机验收辅助：注入完整/取消会话（需主程序运行中；键位需临时改为 f8）
../m0/.venv/Scripts/python.exe scripts/inject_ptt.py hold|cancel
```

## 已知限制

- MCP/SendKeys 等合成键盘注入不经低级键盘钩子，自动化验收需用 keyboard 库注入
  （`scripts/inject_ptt.py`）或物理按键。
- 高 DPI（150%/200%）下 ImageGrab 截图是虚拟化分辨率，悬浮条像素核验需乘缩放系数。
- 热词库密钥（`hw.key`）丢失即不可恢复——隐私优先的取舍，见设计文档 §热词库。
