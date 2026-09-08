# 本机麦克风模式（Local Mic Mode）——P1 核心闭环并入 VoiceStick.exe 设计

状态：迭代一/二/三均已交付（ceb2550a / 820bb43c / 0897cbdc），迭代三 UX 补全（录入式热键 + 模型目录浏览/有效性回显）a3deb3fc 已交付，后续候选见文末
决策：2026-09-07 用户确认「并入 VoiceStick.exe、核心闭环优先」。

## 目标

P1（p1/，Python MVP 验证工程）验证的**无设备语音输入**闭环并入 C++ 产品：
本机麦克风采集 + 键盘热键按住说话 + SenseVoice 本地离线识别 + 注入与剪贴板恢复。
一个托盘、一套设置、统一状态机（符合「桌面端是状态唯一可信源」红线）。
P1 原型归档保留（与 m0 同级，不进产品链路）。

## 与 P1 的能力对照（首期范围）

| P1 能力 | 并入方式 | 迭代 |
|---|---|---|
| SenseVoice 离线识别 | LocalAsrClient（sherpa-onnx C API，OfflineRecognizer） | 一 |
| 本机麦克风采集 | WASAPI capture（新增 mic_capture_win） | 二 |
| 按住说话热键 | LL 键盘钩子（复用 xiaomi_keymap_hook 模式，新增 mic_mode_hotkey） | 二 |
| 注入 + 剪贴板完整格式恢复 | InputInjectorWin::Paste 内挂 clipboard_vault 逻辑 | 三 |
| 热键方案/热词飞轮/AI 改写 | **不在首期**（shortcut_capture 已有键位 UI；热词走现有云端热词） | 后续 |

## 架构设计

### 输入源第三极

现有输入源：StickS3（BLE audio_tx）/ 小米遥控器（ATVV→Opus 归一化）。
新增「本机麦克风」：不占 BLE device_id，协调器内以 `kLocalMicDeviceId` 常量
（如 `"local-mic"`）标识，会话流转走既有 button_down/up 语义——LL 热键 keydown
映射为 HandleButtonDown、keyup 映射 HandleButtonUp（device_id=local-mic），
状态机/悬浮条/超时兜底全部复用，零新状态。

### 音频路径（复用 Opus 管线）

```
WASAPI mic PCM(16k) → audio_opus_encoder → Ogg Opus chunk
    → AsrClient::SendOggOpusChunk（与 BLE 路径完全一致）
    → LocalAsrClient 攒 chunk → is_last 时 Opus 解码回 PCM → SenseVoice 推理
    → on_final 全文 → 既有 Paste 流程
```

取舍：本地识别多一次 Opus 编解码往返（16kHz 单声道，往返损失可接受；小米遥控器
路径已实证 Opus 归一化可行）。换来：LocalAsrClient 是纯 AsrClient 实现，协调器
与音频管线零改动。

### LocalAsrClient（迭代一核心）

- sherpa-onnx **C API**（`sherpa-onnx/csrc/c-api.h`）+ **OfflineRecognizer**
  （SenseVoice 非自回归，非流式）。
- 依赖引入：官方 windows-x64 预编译包（含 onnxruntime，静态链接选项），CMake
  FetchContent 外部下载或 third_party 解压集成（对齐 WinSparkle 预编译包配方的
  本地缓存回退模式）；源码编译不做（onnxruntime 编译过重）。
- 模型：SenseVoice-Small int8（~900MB），复用 m0/models 已有权重（配置
  `local_asr.models_dir`，默认指向程序目录旁 models/）；MSI 打包分发策略后续迭代。
- Start() 校验模型文件存在（model.int8.onnx + tokens.txt），失败 LastStartError
  如实报错（不静默降级——与 P1 纪律一致）。

### mic_mode_hotkey（迭代二，已交付）

复用 xiaomi_keymap_hook 的成熟模式：LL 钩子 + message-only 窗口 + 主线程消息泵；
默认右 Ctrl 按住说话（`[local_asr].push_to_talk_key`，键名解析 `push_to_talk_key.h`）。
**只观察不拦截**（键仍投递前台应用，原组合键行为不变）；注入的合成键一律过滤
（LLKHF_INJECTED），避免与产品自身注入互相触发。

实现要点（与设计的差异/沉淀）：

- **ASR 路由钉住制**：会话建立（HandlePrimaryButtonDown）时一次性钉住
  `session_asr_` 指针（local-mic→LocalAsrClient，其余→云端 asr_）。不能在发送时
  按会话身份现算——`SendFinalOggChunkIfNeeded` 发最终块**前**已 `active_session_id_.reset()`，
  现算会把 final 块误路由到云端客户端（真机测试抓出，测试断言云端零触碰拦下）。
- **采集停止归属热键**：`WasapiMicCapture::Stop()` 只在热键释放/Shutdown 调用
  （Stop 须 join 采集线程，而 EnterReady 等会话收尾路径持有 audio_mutex_，
  join 会与喂帧死锁）。会话异常终止时后续帧被会话校验丢弃，松开即彻底停止。
- **静音包补零**：WASAPI SILENT 包喂零而非跳过，保持帧时间线与 StickS3 设备
  路径对齐——全程无声的按住会话由 ASR 返回空文本干净收尾，不触发
  "No audio frames" 错误。
- **尾帧补零冲刷**：释放时 OpusFrameSlicer 余量补零到 640 采样（40ms）编码，
  再发空 END 帧复用主会话 audio_end 收尾路径（短按丢弃/最终块/finalizing 全复用）。
- 采集线程喂帧走 `HandleAudioFrame`（内部自锁+会话校验），无锁早退门控用原子
  session id；slicer/encoder/seq 仅采集线程与 join 后的释放线程访问。
- 修改 [local_asr] 经设置界面保存即热更（SyncLocalMicRuntime，见下节）；手改
  config.toml 仍需重启（boot 期 ApplyUpdatedConfig 之外无文件监听）。

### 运行时热更（迭代三，已交付）

`Win32App::SyncLocalMicRuntime()`（幂等，对齐 SyncF5Suppressor 模式）在启动与
`ApplyUpdatedConfig`（设置保存回调）统一调用：

- enabled 且模型目录（空 = exe/models，相对路径锚 exe 目录）与已生效值不同 →
  重建运行件注入协调器（`SetLocalMicRuntime`）；disabled 且有已生效值 → 拆运行件
  与热键。
- 热键：`MicModeHotkey::Start` 幂等（已运行只换键）；键名解析失败（enabled 时）
  日志告警并卸载热键。
- **SetLocalMicRuntime 热替换死锁预防**：旧 `WasapiMicCapture` 析构调 `Stop()`
  join 采集线程，采集回调 `FeedLocalMicPcm` 要抢 `audio_mutex_`——必须先在锁内
  清会话状态（`local_mic_active_session_id_` 置 0、`session_asr_` 防悬挂）再在
  锁外替换 unique_ptr，否则热替换瞬间与采集线程互等死锁。

### 剪贴板恢复（迭代三，已交付）

`clipboard_vault.h/.cc`（独立组件，P1 clipboard_vault.py 的 Win32 移植）挂在
`InputInjectorWin::Paste`：快照→写识别文本→Ctrl+V→延时 150ms→恢复；快照拿不到
（剪贴板被占用打不开）只记日志跳过恢复，绝不误清用户剪贴板。

实现要点（真机/单测沉淀）：

- **Save 打不开必抛错**：空快照只代表真空剪贴板，混入「打不开」会让 restore
  误清用户剪贴板。重试 8 次 × 25ms，OpenClipboard 带 message-only owner 窗口
  （进程级单例；P1 实证 OpenClipboard(NULL) 写入存在释放异常）。
- **句柄类格式跳过**（CF_BITMAP/CF_METAFILEPICT/CF_PALETTE/CF_ENHMETAFILE），
  但系统枚举位图时会同时列出可合成的 CF_DIB——vault 快照 CF_DIB 字节，恢复后
  系统从 DIB 再合成位图，**图像内容实际保留**（优于 P1 预期的直接丢弃）。
- **CF_UNICODETEXT 终止符口径**：剪贴板按 NUL 结尾规范化文本，测试布置与读回
  的字节口径必须一致（都含终止符），否则字节级断言差 2 字节必挂。
- Restore：EmptyClipboard 后逐格式 `GlobalAlloc(GMEM_MOVEABLE)`+memmove+
  SetClipboardData（单项失败 GlobalFree 继续下一项）；GetClipboardData/
  GlobalSize/GlobalLock 均须在打开态。
- Paste 恢复时序沿用 P1 验证值：注入后 Sleep(150) 再恢复（目标应用异步读剪贴板，
  立即恢复会粘贴出旧内容）；press_enter 分支额外多等。

## 配置（app_config / config.toml）

```toml
[local_asr]
enabled = false            # 本机麦克风模式开关（默认关，不改变现有行为）
models_dir = ""            # 空 = 默认位置
push_to_talk_key = "right ctrl"
```

## 测试策略

- 单测（voicestick_core）：LocalAsrClient 用真模型跑推理冒烟（模型缺失时 SKIP，
  不 mock 推理结果——不伪造红线）；Opus 攒包→解码往返；热键状态映射；
  剪贴板快照恢复（CF_DIB 字节级，复用 P1 测试用例设计）。
- 真机验收：右Ctrl 按住说话→悬浮条→松手出字→原剪贴板（图片）保留；
  无 BLE 设备环境独立可用。

## 风险与边界

- sherpa-onnx 预编译包与 MSVC 2022 x64 的 ABI/链接模式（静态 vs DLL）需在
  迭代一冒烟时实证；WinSparkle 的本地缓存回退配方可参考。
- 麦克风权限：Win10 1903+ 桌面应用 WASAPI 采集通常无系统级弹窗（非 UWP），
  真机验证。
- 模型体积对安装包/更新通道的影响（WinSparkle 增量）——分发策略单列后续迭代。

### 设置 UI 补全（迭代三收尾，a3deb3fc 已交付）

- **录入式热键**：「按键说话键」行增「录入」按钮，复用 ShortcutCapture
  （`require_modifier=false` + 新增 `allow_modifier_as_key=true`，单键模式下
  修饰键直接作主键，右 Ctrl 一键即录）；按键决策抽出纯函数 `ClassifyKey`
  （kAccumulateModifier/kCapture/kCancel/kRejectNoModifier 四态）；捕获成功经
  `FormatPushToTalkKey`（键表扩为 name/display/vk 三列，display 列输出规范
  键名如 "right ctrl"，同义名 esc/escape 取首见主名）回写编辑框，不受支持
  的键（方向键/Win 键等）弹提示不改值；Esc 取消；3 秒无键盘事件复用热键
  设置的 UIPI 超时引导（kPttCaptureHintTimerId）。
- **模型目录浏览与有效性回显**：「模型目录」行增「浏览…」按钮
  （IFileDialog FOS_PICKFOLDERS 泛化 `ChooseFolderInto`，SetFolder 定位当前
  值）；目录编辑失焦/浏览返回/打开设置时即时校验，`✓ 模型就绪` /
  `✗ 缺少 model.int8.onnx…` 状态行回显。
- **口径统一**：`ValidateSenseVoiceModelsDir`（model.int8.onnx + tokens.txt
  存在性）与 `ResolveLocalMicModelsDir`（空→exe/models、相对→锚 exe）提为
  local_asr_client_win 公共函数，`Start` 与设置界面、`SyncLocalMicRuntime`
  三处同一解析/校验口径；`shortcut_capture.cc` 移入 voicestick_core 供
  core_tests 链接（ClassifyKey 纯函数单测 + Format 与 Parse 23 键往返一致）。

## 后续候选（首期不做）

- 模型 MSI 分发策略（~900MB 模型进安装包 or 首启下载/用户自备目录）。
- 热词飞轮 / AI 改写 / 热键方案预设等 P1 剩余能力移植（shortcut_capture 已有
  键位 UI 可复用做热键方案预设）。
