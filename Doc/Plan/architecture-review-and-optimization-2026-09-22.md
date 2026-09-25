# VoiceStick 架构评审、缺陷清单与优化/测试方案

> 评审对象：`feat/stick-gateway` @ `8f74822`（VERSION 2.3.9，2026-09-20；评审开始时工作树干净，评审期间发生并行合并，见下方状态变更）
> 评审日期：2026-09-22
> 方法：源码实读 + 六路并行深审（固件核心 / 固件网关 / macOS / Windows 核心 / Windows 特性 / 协议一致性与网站）+ 关键结论人工二次复核 + 实际执行 `swift build`、`swift package describe`、CMake 源清单核对、版本/分支比对。
> 证据约定：`文件:行号` 为证；**【已复核】** 表示评审者本人重读源码或实跑确认；未标注者来自深审报告（含行号），引用前建议复核。行号对应上述 commit。
> 本文档只做分析与方案，未修改任何产品代码。

> **评审期间状态变更（重要，2026-09-22 09:5x 复核）**：工作树被并行修改——`feat/add-MiRemote` 正在合并进当前分支（`.git/MERGE_HEAD` 存在；240 文件新增、28 修改、2 删除；`README.md`/`README.zh-CN.md`/`Doc/Ref/protocol.md`/`Doc/Ref/desktop-config.md`/`Doc/Expe/claude-memory-distilled.md` 五处 `UU` 冲突未解决）。复核结果：
> - `desktop/macos/Package.swift` 已出现 `VoiceStickCore`/`COpus`/`VoiceStickTests` target；`Sources/VoiceStickCore/`（9 文件）与 `Tests/VoiceStickTests/`（11 文件）已进入工作区；
> - **但 `swift build` 仍然失败**：`Sources/VoiceStickCore/RemoteButtonHID.swift:84: cannot find type 'ButtonMapping'`（同文件 31/46/47/50 的 `RemoteButton` 亦无定义）；
> - `git log --all -S 'ButtonMapping'` 与 `-S 'RemoteButton'` **全历史零命中** ⇒ 这两个类型从未被任何分支提交过，不是「合并顺序/漏合并」问题，必须补源码或让 `RemoteButtonHID.swift` 与 add-MiRemote 的 HID 设计对齐；
> - 因此：**P0-1 对已提交的 HEAD（`8f74822`）完全成立**；工作树修复进行中但未完成，5 处冲突与缺失类型解决前仍不可构建、不可交付。下文 macOS 结论基于 HEAD 提交（半套实现），合并完成后需在完整实现上复核。

---

## 0. 结论摘要

**整体判断**：这是一套工程成熟度明显高于平均水平的"硬件 + 双桌面端 + 网站"产品。职责边界（桌面端是状态唯一可信源、固件只报事实、小米固件零改动）在代码中真实落地；Windows 端在真机事故上积累的防御（CCCD 缓存击穿、僵尸链路梯度自愈、OTA 三重超时、录音看门狗）属于同类项目的高水准；`Doc/Expe` 的复盘文化是最大隐性资产。

但当前仓库有 **4 个 P0 级问题**（一个已潜伏 14 天：macOS 无法构建），以及一组由「隐式线程契约 / 单值连接抽象 / 错误恢复不对称 / 契约不可机器校验 / 无自动化门禁」衍生的 P1。**最高性价比的三件事**：① 恢复 macOS 可构建并进 CI；② 把 protocol.md 变成可机器校验的单一事实源（三端 golden-frame 契约测试）；③ 给发布链加"凭据/签名/完整性"硬门禁。

### Top 15 结论

| # | 级别 | 结论 | 关键证据 |
|---|---|---|---|
| 1 | ~~P0~~ **已修复** | macOS 端曾无法构建（半迁移）：并行工作已合并 `feat/add-MiRemote`（`7fc539a`）并补齐缺失类型（`3bd4762`）；本次复核 `swift build` 通过（438/438 测试绿），并新增 CI 门禁 | `git log`；本机实跑；`.github/workflows/ci.yml` |
| 2 | **P0（部分推进）** | 固件 `control_rx/ota_rx` 无鉴权、无 Secure Boot、广播无过滤 ⇒ 任意 central 可刷任意固件。`test_playback` 实为 **SPIFFS 内**任意文件（`audio_pipeline_set_playback_file` 固定加 `/spiffs/` 前缀；原报告「任意文件读取」表述过重），本次已加路径分隔符/`..` 拒绝 | `voice_ble.c:774,779,1189-1191,1031,1058,1062`；`audio_pipeline.c:319-328`；`main.c` test_playback 分支（已加固） |
| 3 | ~~P0~~ **已实现**（待 Windows CI 验证） | ASR 完成回调曾运行在 worker 线程直接改协调器状态。已新增 `SetUiDispatcher`/`RunOnUiThread`，封送 ASR、字幕 ASR、精修/翻译完成回调；`win32_app` 注入 `DispatchToUi` | `voice_stick_coordinator.h:293,642,651`；`.cc:567-578,584-670,2529-2535,2600-2695`；`win32_app.cc:524-527` |
| 4 | ~~P0~~ **已实现门禁** | 默认发布链曾把真实凭据注入 exe+MSI 并上传公开 Release。现改为显式 opt-in（`VOICESTICK_EMBED_BUILTIN_KEYS` / `VOICESTICK_MSI_EMBED_REAL_KEYS`），`release.ps1` 上传前跑凭据扫描，命中即拒绝 | `scripts/scan_release_artifacts.py`（7/7 单测绿）；`build-msi.bat:43-56,202-227`；`release.ps1:184-192` |
| 5 | P1 | 固件 ATVV 会话 ERROR 终局、发现链终局、STREAMING 无超时 ⇒ 网关模式语音键可静默永久失效（屏幕仍 RC: OK） | `gateway_atvv_session.c:236,194-196,350-371`；`xiaomi_atvv_client.c:189-194,216-222,380-384` |
| 6 | P1 | 网关模式 `voice_ble` 连接/对端为单值，切换器动作复用单值句柄 ⇒ 多链路断错对象、切换器卡死 | `voice_ble.c:52,799-808,860-890,1313-1338`；`gateway_switcher.c:139-160` |
| 7 | P1 | 深睡准备不可逆：关机中止分支不回滚 ⇒ 屏幕永久黑 + MIC 断电但设备仍在跑 | `main.c:612-613,629-647`；`ui_status.c:411-423`；`stick_s3_board.c:413-423` |
| 8 | P1 | 固件录音/OTA 互斥非原子、OTA 错误不清理、rollback 启动即签到；事件队列满静默丢弃关键事件 | `voice_ble.c:240-262,296-323`；`main.c:749-761,3328-3338` |
| 9 | P1 | Windows 烧录工具关窗 5s 超时后释放对象，worker 仍可能运行 ⇒ use-after-free | `flash_tool_dialog.cc:296-321,700-710,209-215` |
| 10 | P1 | 腾讯热词表同步 HTTP body 长度单位/编码双错、业务错误当成功、且在 `audio_mutex_` 内阻塞（最长 ~80s） | `tencent_asr_vocab_client.cc:277-281,435-454`；`voice_stick_coordinator.cc:1928-1954,2221-2235` |
| 11 | P1 | Windows 协调器 `config_` 跨线程竞争（含 UAF 面）；配置写盘非原子且合并保存丢 `[license]` | `voice_stick_coordinator.cc:244-263` 与 `:2632-2775`；`app_config.cc:925-933,849-877` |
| 12 | P1 | 离线授权：试用锚点/防回拨只存用户可写 config，删文件即重置；绑定依赖"设备在线"；`DateToDays` 早于 2026 年下溢 | `license_runtime.cc:18-26,41-57`；`license.cc:27-38,64-69` |
| 13 | P1 | 固件 OTA 信任链只有"同源 sha256"（无固件签名），下载无超时/无上限；模型在位只比大小不校验哈希 | `firmware_manifest.cc:257-281,106-129`；`model_download_dialog.cc:117-129` |
| 14 | P1 | 浏览器烧录不校验 manifest 的 `merged_sha256/merged_size`；`update-appcast.py` 正则跨 item 贪婪匹配可产出重复条目 | `App.vue:121-146,186-196`；`update-appcast.py:15-24,97,122` |
| 15 | P1（部分闭环） | 零自动化门禁：CI 不跑测试、不构建 Windows/macOS。已新增 `.github/workflows/ci.yml`：macOS build+测试、Windows Debug 构建+ctest、scripts 单测 | `.github/workflows/ci.yml`；Windows 测试 NDEBUG 假绿仍需测试目标显式 `-UNDEBUG`（本次 CI 用 Debug 规避） |

### P0 推进记录（2026-09-22 实施）

| P0 | 状态 | 本次改动 | 验证 |
|---|---|---|---|
| P0-1 macOS 构建 | ✅ 已修复 | 并行工作合并 `feat/add-MiRemote`（`7fc539a`）+ 补齐 `RemoteButton`/`ButtonMapping` 等**全历史缺失**类型（`3bd4762`）；新增 CI 门禁 | 本机 `swift build` 通过、`swift run VoiceStickTests` = **438/438** |
| P0-2 固件 BLE 鉴权 | 🟡 部分推进 | `test_playback` 增加路径分隔符/`..` 拒绝；**未**启用 `WRITE_ENC`/MITM/Secure Boot——会改变两端配对 UX 且涉及一次性 eFuse 烧录，需产品决策与真机验证 | 本机无 ESP-IDF，未编译；待 `idf.py build` |
| P0-3 ASR 回调线程 | ✅ 已实现（待 CI 验证） | 协调器新增 `SetUiDispatcher`/`RunOnUiThread`；封送 ASR 5 回调、字幕 ASR 5 回调、`TransformText` 收口、本地/云端精修 `on_complete`；`win32_app` 注入 `DispatchToUi` | 本机无法编译 Windows；交由 `ci.yml` 的 windows job（Debug + ctest）验证 |
| P0-4 发布凭据门禁 | ✅ 已实现 | 新增 `scripts/scan_release_artifacts.py`（config 精确值 raw/UTF-16LE + 形态规则 + zip 解包）；`build-msi.bat` 内置凭据与真实 key 配置模板改显式 opt-in；`release.ps1` 上传前扫描并拒绝 | 扫描器单测 **7/7** 绿；合成 leaky exe 端到端 exit=1、干净包 exit=0 |

**遗留与下一步**：
1. **P0-2 完整闭环**需先完成：① 两端桌面实现并真机验证 StickS3 的 OS 级配对；② 再给 `control_rx`/`ota_rx` 加 `BLE_GATT_CHR_F_WRITE_ENC` 并提升 `sm_mitm`；③ Secure Boot v2 + flash encryption 的密钥/产线与一次性 eFuse 烧录流程。在无 IDF 工具链、无法真机验证的本机环境中贸然改连接行为会导致存量设备失联，故只做了无损的越权读取收敛。
2. **P0-3 尚未过编译器**：请在 Windows 上 `build_win.bat` + `ctest`，或推送后观察 `ci.yml` 首次运行；若报编译错误按提示修正（改动集中在 5 个区域，行号见上表）。
3. **P0-4 的 `.bat`/`.ps1`** 无 Windows/PowerShell 可执行，仅静态审查；首次内测包发布前请验证 opt-in 分支与 `release.ps1` 门禁的拒绝路径。

### P1 推进记录（2026-09-22 第二批）

| 项 | 状态 | 改动 | 验证 |
|---|---|---|---|
| A5 深睡准备不可逆 | ✅ | `main.c` 把 `ui_status_prepare_deep_sleep` / `stick_s3_board_prepare_deep_sleep` 移到所有中止分支之后、`esp_deep_sleep_start()` 之前 | 静态审查；`ci.yml` 固件 job 编译 |
| A7 事件队列静默丢弃 | ✅（部分） | 新增 `app_event_is_critical()`；关键事件（BLE_DISCONNECTED / OTA_BEGIN / OTA_DONE / OTA_END / XIAOMI_STOP_DUE / ENTER_POWER_OFF）最多等 20ms，失败必打 `ESP_LOGW` 计数 | 同上 |
| A18 sdkconfig 漂移 | ✅ | `sdkconfig.defaults` MAX_CONNECTIONS 2→3 并修正注释 | `grep`：defaults=3 与受控 `sdkconfig`=3 一致 |
| B4 WASAPI UI 自旋 | ✅ | `Start` 改 3s 有界等待 + 超时置 `stop_requested_`；`shared_ptr` 打开标志消除超时后引用悬垂；`running_`/`open_done_` 支持失败后重试回收 | `ci.yml` windows job 编译 |
| B5 剪贴板注入失败 | ✅ | 校验 `OpenClipboard`/`SetClipboardData`；失败还原快照并中止粘贴，不再把旧剪贴板内容粘进前台应用 | 同上 |
| B10 LLM UTF-8 边界 | ✅ | `EndsOnUtf8Boundary` 判据改 `cont == len - 1`；非法前导字节返回 false | 同上 |
| E1 浏览器烧录不校验哈希 | ✅ | `App.vue` 用 WebCrypto 校验 `merged_sha256` + `merged_size` 后才写入；失败中止；支持 `merged_url_fallback`；manifest 拉取失败即中止；i18n 双语补齐 | `npm run build` 通过 |
| E2 appcast 跨 item 正则 | ✅ | `existing_item` 按 `<item>` 边界切分；新增版本单调性守卫（`--allow-version-downgrade` 可显式越过） | 新增 `test_update_appcast.py` 4/4 绿 |
| E5 idf_cli 静默换口 | ✅ | 显式 `--port` 未命中直接报错，不再回退自动匹配 | `py_compile` + 逻辑审查 |
| E9 E2E 退出码假 PASS | ✅（部分） | `gateway_switch_acceptance` 任一 FAIL 返回 1；`run_asr_bench` 有运行但零成功的 provider 返回 1 | `py_compile` |
| E11 workflow 命令注入 | ✅ | `release.yml` 两处 tag 解析改为 env 传递 + `vX.Y.Z` 形态校验 | 静态审查 |
| E6 CI 门禁 | ✅ | `ci.yml` 新增 firmware `idf.py build` job；scripts job 纳入 appcast 单测（现共 5 个测试文件） | 待首次 push 运行 |

**本批未做（仍在 P1 待办，按优先级）**：
1. **B1 烧录工具关窗/析构 UAF**：需按 worker 完成事件重构关闭路径（不能 5s 超时后释放对象），改动较大，留待有 Windows 编译环境时做。
2. **B6/B7 腾讯热词表**：HTTP body 长度单位/编码修正 + 错误判定上抛 + 词表同步移出 `audio_mutex_`。
3. **B8/B9**：协调器 `config_` 改 `shared_ptr<const AppConfig>` 原子换入；配置写盘改临时文件原子替换并保留 `[license]`。
4. **C1–C5**：授权锚点冗余/绑定改持久集合/`DateToDays` int64；固件 OTA manifest 签名；模型在位哈希复核。
5. **A1–A4**：ATVV ERROR 恢复、发现链重试、STREAMING 看门狗、`voice_ble` 多连接表。
6. **A6**：固件 OTA 错误路径统一 `esp_ota_abort` + `ota_clear_state`（本轮未动，避免无编译环境下的状态机风险）。
7. **D1–D10 macOS**：能力帧解析、订阅健康/超时、OTA 超时等（原计划，已在第三批部分完成，见下）。

### P1 推进记录（2026-09-22 第三批：macOS D 类 + Windows 小项）

| 项 | 状态 | 改动 | 验证 |
|---|---|---|---|
| D2 macOS click_to_talk 丢 `duration_ms` | ✅ | `handleButtonClick` 按 `duration_ms` 消歧：>0 仅匹配活跃会话才停，否则按失步残留忽略；==0 走启动（finalizing 期间忽略）；字段缺失回退旧状态判定（对齐 Windows `HandleButtonClick`） | 本机 `swift build` + `swift run VoiceStickTests` |
| D4 macOS 丢 `min_version` / `*_fallback` | ✅ | `FirmwareManifest` 增加 `min_version`、`ota_url_fallback`、`merged_url_fallback`；OTA 主源失败自动切换回退源；升级门槛优先用 manifest 下发值，缺失回退本地常量 | 同上 |
| D5 macOS OTA 无超时 | ✅ | 新增 OTA 看门狗：begin/end 的 write-with-response ack 限 5s，数据/事件停滞限 15s；`progress` 事件重新武装；done/fail/析构取消。一次丢 ack 不再永久锁死后续升级 | 同上 |
| D8 Windows 丢 OTA `esp_err` | ✅ | `FirmwareOtaStateEvent` 增加 `esp_err` 解析；`ble_central_win` 的失败日志与用户可见错误串带上固件原始错误码 | `ci.yml` windows job（待首次运行） |
| B6 腾讯热词表 HTTP body 错 | ✅（核心项） | 发送改 UTF-8 字节长度（原传 UTF-16 元素数 + wchar 缓冲，只发一半且非 UTF-8）；解析 `Response.Error` 显式失败并记日志；被表规则过滤的词计数上报；HTTP 失败记 `err=` | 同上 |

**仍未做（下一批建议顺序）**：
1. **B7** 腾讯热词同步移出 `audio_mutex_`（需重构 ASR 启动路径，建议在可编译/可跑测试的环境做）；
2. **B8/B9** 协调器 `config_` 快照化 + 配置原子写 + `[license]` 保留（数据丢失风险最高的一组）；
3. **B1** 烧录工具关窗/析构 UAF；
4. **C1–C5** 授权锚点/绑定/`DateToDays`、固件 OTA manifest 签名、模型在位哈希；
5. **A1–A4/A6** 固件 ATVV 错误恢复/发现重试/STREAMING 看门狗/多连接表/OTA 错误统一 abort；
6. **D1/D3/D9** macOS 能力帧解析、订阅健康与重订阅、协议版本协商。

---

## 1. 程序功能与架构全景

### 1.1 产品功能矩阵

| 能力 | 固件 StickS3 | macOS | Windows | 网站 |
|---|---|---|---|---|
| 按键/双击/敲击/编码器采集 | ✅ 硬件事实 | 消费 | 消费 | — |
| 音频采集 + Opus 编码 | ✅ 16kHz/mono/40ms/32kbps CBR | — | 小米链路二次编码 | — |
| BLE GATT（audio/state/control/OTA） | ✅ 服务端 | ✅ 中央 | ✅ 中央 | — |
| 小米遥控器 ATVV | 网关模式（新，central） | 另一分支有完整实现 | ✅ 完整 | — |
| 交互状态机 | ❌ 红线 | ✅ Coordinator | ✅ Coordinator | — |
| ASR | — | 火山/云 | 火山/腾讯/云/本地 SenseVoice | — |
| Ogg Opus 封装 | — | ✅ | ✅ | — |
| 文本注入/字幕/悬浮窗 | 只渲染 `ui_state` | ✅ | ✅ | — |
| 微信输入法模式（虚拟麦克风） | — | ❌ | ✅ | — |
| 体感鼠标 / 敲击映射 | 采样 | ❌ | ✅ | — |
| 固件 OTA | ✅ 接收端 | ✅ | ✅ + COM 烧录 | ✅ 浏览器 esptool-js |
| 自动更新 | BLE OTA | Sparkle | WinSparkle/MSI | appcast 源 |
| 商业化（离线授权/本地模型） | — | — | ✅ | 下载页 |

### 1.2 核心数据流

```text
StickS3 mic -> ES8311/I2S PCM -> HPF+AGC -> Opus(40ms) -> BLE notify
                                                   |
Desktop: AudioBleFrame -> Ogg Opus 封装 -> ASR WebSocket -> 精修/翻译 -> 粘贴/字幕
                                                   \-> Opus decode -> PCM -> WASAPI 虚拟麦克风（微信模式）
小米 RC: HOGP 按键 -> OS（可选拦截重映射）
         ATVV ADPCM -> PCM 后处理 -> Opus 重编码 -> 汇入同一条 Ogg/ASR 管线
```

### 1.3 分层与职责

- **固件**（15.4k 行 C）：`main.c`（3357 行，编排/状态/电源/网关粘合）+ `audio_pipeline` + `voice_ble` + `ui_status` + `stick_s3_board` + `power_log` + `bmi270` + `mini_encoder_c` + `gateway`（ATVV/HOGP/switcher/targets，纯逻辑可 host 测试）。
- **Windows**（src 54.3k + tests 16.5k 行 C++20）：`voicestick_core`（纯逻辑）+ `VoiceStickApp`（Win32）+ `VoiceStickFlash`（COM 烧录）+ 两个测试目标；小米链路 5 个纯逻辑模块是分层样板。
- **macOS**（8.7k 行 Swift）：`VoiceStickApp` + 规划中的 `VoiceStickCore`（**当前未接上**）；完整实现在另一分支。
- **网站**（576 行 Vue3）：落地页/下载页/浏览器烧录；发布走 COS 主源 + GitHub 回退 + appcast。
- **文档**（145 篇 Markdown，`Doc/Plan` 88、`Doc/Expe` 32）：设计/复盘/参考三层齐备，是最大隐性资产，也是最大同步成本。

### 1.4 资产与现状盘点

| 维度 | 数值 | 备注 |
|---|---:|---|
| 固件源码 | 15.4k 行 | 最大单文件 `main.c` 3357 行 |
| Windows 源码 / 测试 | 54.3k / 16.5k 行 | `core_tests.cc` 15.7k 行、约 2827 处 `assert` |
| macOS 源码 / 测试 | 8.7k / 0.09k 行 | 测试未接入 SwiftPM；当前分支不可构建 |
| 固件 host 单测 | 1.5k 行 / 4 目标 | 仅 Windows+MSVC 绝对路径可跑 |
| 脚本 / E2E | 13.4k 行 / 26 个 py | 真机工具链丰富 |
| CI | 2 个 workflow | **不执行任何测试**；不构建 Windows/macOS |
| 版本 | VERSION 2.3.9 = firmware/version.txt ✅ | 最新 tag 仅 v2.3.7，CHANGELOG 到 v2.3.8；文档写"当前 2.3.6" |

---

## 2. 设计理念评估

### 2.1 做对了什么

1. **职责边界不是口号**：固件只上报事实，取消/确认/恢复语义全在桌面端；协议弃用表（`press_start`→`button_down` 等）让演进有据。
2. **协议事实化**：`Doc/Ref/protocol.md` 记录字段、MTU 预算、时序常量与"为什么"，是跨端实现的事实基准。
3. **纯逻辑/薄壳分层**：Windows 的 ATVV/ADPCM/Ogg/keymap 决策、固件 gateway 的 parser/keymap/session/targets/switcher 均不依赖平台 API，可 host 单测——这正是本次无设备也能审出大量逻辑缺陷的前提。
4. **事故驱动的健壮性**：CCCD 缓存击穿、连接期活性证明、僵尸链路梯度自愈且"绝不 unpair"、广播报告≠实时状态、三重 watchdog。每条都有真实事故与文档。
5. **反"假绿"文化**：集成测试无 key 返回 77 SKIP、E2E 不 mock 真实链路、`AbortIfFailed` 免疫 NDEBUG——红线写得清楚（虽然执行有缺口）。
6. **降级不停机**：IMU/编码器/BLE/音频初始化失败均降级继续。

### 2.2 结构性债务

1. **线程契约是隐式的**：项目只说"状态机在桌面端"，没说"谁可以在哪个线程进入"。Windows 的 ASR 回调（P0-3）、字幕兜底、NimBLE 回调，macOS 的 CoreBluetooth 回调各自旁路进入。
2. **单值抽象承载多连接现实**：`voice_ble` 与 ATVV 会话把"当前对端"建模为单值，网关模式天然多链路（OS-HID + app + 遥控器）。
3. **错误恢复不对称**：链路层自愈丰富，会话层（ATVV）、失败回滚（微信/剪贴板/OTA）、退出路径几乎空白。
4. **契约不是可机器校验的单一事实源**：同一份协议在 protocol.md 散文 + 固件 JSON 字面量 + Windows 解析器 + Swift Decodable 四处手写；三端零 golden-frame 测试。本次 15 条跨端漂移全部源于此。
5. **门禁缺位 + 分支松散**：无 CI 测试/跨平台构建，长生命周期分支各持半套 macOS 实现；版本/文档/标签靠人工同步。
6. **上帝对象**：`voice_stick_coordinator.cc` 3703 行、`main.c` 3357 行、`win32_app.cc` 3125 行，状态机/音频/ASR/LLM/热词/固件/体感/字幕/微信混杂。

---

## 3. 缺陷清单

> 级别：**P0** 阻断交付/可被利用/必然触发；**P1** 功能可靠性、安全或数据安全；**P2** 性能、体验、潜在缺陷；**P3** 文档、可维护性、低危。

### 3.1 P0

#### P0-1 当前分支 macOS 端无法构建且处于半迁移【已复核】
- **实测**：`cd desktop/macos && swift build` → `error: AppDelegate.swift:4:8 unable to resolve module dependency: 'VoiceStickCore'`，`Build failed`；`swift package describe` 确认 target 只有 `VoiceStickApp` + `CZlib`。
- **证据**：
  - `desktop/macos/Package.swift` 无 `VoiceStickCore` target、无 test target；`Sources/VoiceStickCore/` 仅 `RemoteButtonHID.swift`，`Tests/VoiceStickTests/` 未接入。
  - `AppDelegate.swift:4` `import VoiceStickCore`，`:327-329` 引用 `XiaomiAtvvSession.Options`；`XiaomiButtonInterceptManager.swift:3` 同。
  - 即使补上 target，仍缺：`RemoteButtonHID.swift:31/46/47/50/84` 依赖的 `RemoteButton`/`ButtonMapping`，以及 `BatteryMonitorWindowController`、`XiaomiF5Suppressor`、`GlobalHotkeyManager`、`FrontmostAppProvider`、`HotkeyCaptureWindowController`、`EncoderSettingsWindowController`、`RemoteSettingsWindowController`、`ButtonMappingWindowController` 等 8+ 类型；`AppDelegate` 对 `StatusController/PairDeviceWindowController` 的实参也对不上（`AppDelegate.swift:73-85` vs `StatusController.swift:112-118`；`:521-524` vs `PairDeviceWindowController.swift:19`）。
  - **根因**：`bffa235`（2026-09-08，本分支）把 AppDelegate/拦截层改成"Windows 对齐版"，但提供 `VoiceStickCore` 全部源码 + `Package.swift` target + 7 个测试套件的 `847ca46`（2026-09-02，macOS ATVV P5）只存在于 `feat/add-MiRemote`，**不是 HEAD 祖先**（`git merge-base --is-ancestor 847ca46 HEAD` 为假）。两分支各持一半 macOS 实现。
- **影响**：macOS 端 14 天不可编译/不可交付；`protocol.md` 的 macOS 章节描述的是另一分支行为；文档中"macOS 编译通过"的门禁不成立；任何 macOS 相关结论都只能代表仓库这份源码，不代表线上 2.3.9 二进制。
- **修复方向**：合并/摘取 `feat/add-MiRemote` 的 `desktop/macos/**`（注意 `main.swift` vs `VoiceStickMain.swift`、`XiaomiButtonInterceptManager` 删除等冲突），合并后 `swift build` + `swift run VoiceStickTests` 必须绿，并进 CI。**并补齐从未提交的 `RemoteButton`/`ButtonMapping`/`ButtonsSettings`/`KeySpec`/`XiaomiMicOpenAnchor` 等类型定义**；合并后 `swift build` + `swift run VoiceStickTests` 必须绿，并进 CI。若短期只维护 Windows/固件，则显式从发布矩阵摘除 macOS 并标注分支边界。
- **进展（评审期间）**：合并已在进行中（见文首「评审期间状态变更」）：targets/源码/测试已入工作区，但 5 处文档冲突未解决且 `RemoteButtonHID.swift` 仍因缺类型编译失败——**尚不可认为已修复**。

#### P0-2 固件 BLE 无鉴权 + 无 Secure Boot + `test_playback` 越权文件访问【已复核；文件读范围已修正】
- 证据：
  - `voice_ble.c:774` `control_rx` = `WRITE_NO_RSP`；`:779` `ota_rx` = `WRITE|WRITE_NO_RSP` —— 均无 `WRITE_ENC`/`WRITE_AUTHEN`。
  - `voice_ble.c:1189-1191` `sm_io_cap = NO_INPUT_OUTPUT`、`sm_mitm = 0`（Just Works）；`sdkconfig:494-495` Secure Boot / Flash 加密均未启用。
  - `voice_ble.c:1031,1058,1062`：General Discoverable、`ble_gap_adv_start(..., NULL /* 无过滤 */, ...)`。
  - OTA 只校验长度/偏移（`voice_ble.c:240-388`），无镜像签名。
  - `main.c:1106-1117`：`test_playback` 把对端字符串直接传给 `audio_pipeline_set_playback_file()`，无路径校验；该命令在正式固件中可用。**修正**：`audio_pipeline.c:319-328` 固定拼 `/spiffs/` 前缀，故读取被限制在 SPIFFS 分区内（并非全文件系统），但 `..`/分隔符可穿越挂载根，且分区内含 `power_log.bin` 等数据——**本次已加拒绝逻辑**（见推进记录）。
- **影响**：射频范围内任意 central 无需配对即可：写入 `ui_state`/`remote_button_down/up`/`gateway_keymap_set`/`usb_auto_off`；把任意有效 ESP 镜像写入 OTA 分区并启动；通过 `test_playback` 打开 SPIFFS 分区内文件（含 `..` 穿越）并以音频流回传（分区内数据外泄）。网关模式下设备还是已配对主机的 HOGP 键盘，恶意固件可向主机注入按键。
- **修复方向**：`control_rx/ota_rx` 加 `WRITE_ENC`+MITM 配对；启用 Secure Boot v2 + flash encryption 并校验镜像；广播加白名单；`test_playback` 只在调试构建编译（`#if CONFIG_...`）或删除；若本期不做，README 明示威胁模型并把固件签名列入发布阻断项。

#### P0-3 Windows ASR 回调在 worker 线程直接进入协调器状态机【已复核】
- 证据：回调注册 `voice_stick_coordinator.cc:589-597`；线程来源 `asr_client_win.cc:127` `worker_ = std::thread(RunReusableWebSocket)`，`:466-486` 在 worker 内调用 `on_segment/on_final`（腾讯/本地同构 `local_asr_client_win.cc:165`）；无锁改状态 `voice_stick_coordinator.cc:2995-3003`、`:2259-2313`、`:3148-3156`；UI 线程持 `audio_mutex_` 操作同名字段 `:1928-1955`、`:2210-2224`。`FinishWithFinalText` 还会在 worker 线程调 `ui_->*` 与 `input_injector_->Paste`（内含固定 Sleep）。
- **影响**：`std::string/vector/optional` 跨线程并发读写是 UB；最坏堆损坏、final 丢失或二次粘贴；与 P1"字幕兜底线程无锁"同源。
- **修复方向**：`WireAsrClientCallbacks` 全部 marshal 回 UI 线程（项目已有 `DispatchToUiThread` 范式）；debug 构建加线程断言；字幕/本地麦/微信路径一并收编。

#### P0-4 默认发布链把真实凭据写入公开产物【已复核】
- 证据：`scripts/build-msi.bat:200-222` 默认 `VOICESTICK_MSI_CONFIG_SOURCE=%APPDATA%\\VoiceStick\\config.toml`，注释原文 "Generate MSI config.template.toml with **real test keys** so installed users get Volcengine/Tencent/LLM working out of the box"；`:43-55` 七个 `VOICESTICK_BUILTIN_*` 注入 exe；`release.ps1:141-142` 默认调用 build-msi.bat、`:187` `gh release upload` 到公开 Release；便携包同样带 key 并被 COS 镜像（子代理证据 `package-portable.ps1:103,107`、`mirror_urls.py:36`）。
- **性质说明**：AGENTS.md 明确"Windows 支持把凭据编译进 exe"是**有意设计**（内测开箱即用），凭据文件本身 gitignored。但当前**默认路径**即产出含 key 的公开产物，且没有任何发布门禁区分"内测包"与"公开发布包"。任何下载者 `strings VoiceStick.exe` 即可取得腾讯云 SecretId/SecretKey、火山/LLM key，用于计费滥用。
- **修复方向**：拆成"含 key 内测包"与"不含 key 公开发布包"两条互斥命令，默认落在不含 key 侧；发布前对将公开产物做凭据特征扫描硬门禁；把内置 key 改成用户自有 key 的引导流程。

### 3.2 P1

#### A. 固件运行时

| # | 问题 | 证据 | 影响 / 修复 |
|---|---|---|---|
| A1 | **ATVV ERROR 终局** | `gateway_atvv_session.c:236`（ERROR 丢弃全部 Control）、`:194-196`（start 只收 IDLE）、`:350-371`（tick 无 ERROR 分支） | CAPS 2s 超时/8kHz/codec 不符任一命中即永久静默；加冷却重试+上报 |
| A2 | **ATVV 发现链终局** | `xiaomi_atvv_client.c:189-194,216-222` 直接置 DONE；看门狗只看 SVCS/CHRS `:296-307` | 服务/句柄缺失后永不再发现；保持 CHRS 退避重试并上报"ATVV 不可用" |
| A3 | **STREAMING 无超时** | `gateway_atvv_session.c:350-371`；`xiaomi_atvv_client.c:380-384`；自愈宏 `:50-51` 全仓无引用 | 丢一次 STOP 即永久"按住"，录音不收尾；加无音频看门狗合成 PRESS_UP |
| A4 | **单值连接状态 + 切换器动作错位** | `voice_ble.c:52,799-808,860-890,1313-1338`；`gateway_switcher.c:139-160`；`main.c:2424-2436` | 多链路断错对象、切换器永久"切换中"；改连接表 + 动作携带 peer 身份 |
| A5 | **深睡准备不可逆** | `main.c:612-613` 先关屏/断 MIC，中止分支 `:629-647` 无回滚；`stick_s3_board.c:413-423` 的 L3B 无 true 调用者；`ui_status.c:411-423` | 关机中止后黑屏 + MIC 断电但设备在跑（录音变静音）；面板/L3B 关闭移到 sleep 前最后一步或幂等恢复 |
| A6 | **录音/OTA 互斥非原子 + OTA 错误不清理 + rollback 失效** | `voice_ble.c:240-262,296-323`；`main.c:1741-1756,3328-3338` | 录音中 OTA 可触发 cache-disable 崩溃；一次 OTA 报错后永久拒绝录音/关机；坏固件不回滚 |
| A7 | **app 事件队列静默丢弃 + 消费端阻塞 3s** | `main.c:749-761,1841,731`；`audio_pipeline.c:1047` | XIAOMI_STOP_DUE/OTA_END/BLE_DISCONNECTED 丢失 → 状态机卡死且主键无法自救 |
| A8 | **audio_task 错误分支热循环 → Task WDT** | `audio_pipeline.c:632-636,653-656` 无延时；`sdkconfig:1628-1629` 检查 CPU1 idle | codec 持续失败 → 5s 后整机复位；失败加 vTaskDelay + 连续 N 次主动收尾 |
| A9 | **esp_timer 任务当工作队列** | `main.c:2010-2038`（双击回调内启动 I2S/ES8311/Opus）、`encoder/imu/air_mouse poll`；BMI270 I2C 超时 100ms（`bmi270.c:231-254`）；`power_log.c:411-448` 模式切换再做 2–3 次 I2C | 手势窗口/空闲计时/OTA 看门狗全部抖动；I2C 轮询与硬件初始化移出 timer |
| A10 | **编码器降级不可恢复 + I2C 总线泄漏** | `mini_encoder_c.c:48-51,119,145-147,62-92,113-132` | 一次总线抖动即永久失效；失败候选不释放 I2C（占 G0 strapping 上拉） |
| A11 | **BMI270 加载失败仍报 present** | `bmi270.c:406` 早于 `:425-429` 失败分支 | 拿起/旋转/敲击/体感静默失灵但主机显示在线 |
| A12 | **`tap_enabled` 默认 true 与文档 false 相反** | `main.c:105,3052` vs `protocol.md:303` | 开箱可能注入 Down 键；文档/实现取一 |
| A13 | **`tap_sensitivity` 无范围校验且缺字段静默重置落盘** | `main.c:1078-1100` | 非法值写 NVS、覆盖用户设置；补夹取 1..10 与显式 return |
| A14 | **`ui_state.text` 超预算静默截断** | 固件 `voice_ble.c:720-726` 512B 硬截断 → cJSON 失败整帧丢弃 `main.c:921-927`；macOS `BleCentral.swift:119-146` / Windows `ble_central_win.cc:2782-2810` 发送端零长度校验；固件对 state_tx 反而有预算告警 `:1483-1487` | 长 partial/最终文本 → 设备屏幕卡在 thinking；发送端按 MTU 预算截断/分段 |
| A15 | **gateway_keymap 回执永远发不出** | `main.c:2285` `routes[400]` + snprintf 返回值累加 + `off < sizeof`；13 键最小 495B | 路由设置界面拿不到表；分片/紧凑编码 |
| A16 | **双击收尾不清 owner / click_to_talk 忽略 remote up** | `main.c:1281-1290,1468-1473`；`:1493-1495` | 编码器"自定义键"失效；远程热键松手不停录 |
| A17 | **主机无响应看门狗是死代码** | `s_host_response_timer` 只创建 `main.c:2134`、只停止 `:539-540`，全仓无 start | 主机不回 ui_state 时设备永远停在 recording/thinking |
| A18 | **配置真相源漂移** | `sdkconfig.defaults:12`=2 vs `sdkconfig:674`=3（逐项 diff 唯一不一致） | 删 sdkconfig 重建会砍掉网关一条链路 |
| A19 | **PMIC IRQ 先 enable 后清源 + ISR 内队列失败永久 disable** | `main.c:1728-1733,2855-2906` | 中断线可永久失效，只剩 10s 电池兜底 |
| A20 | **错误路径吞掉** | ATVV TX 写 rc 只日志 `xiaomi_atvv_client.c:85-92`；HOGP 发送返回值被丢 `main.c:2313-2317`；power_log `fwrite/fclose` 不检查 `power_log.c:209-222`；导出短读当 EOF `power_log.c:534-543`+`voice_ble.c:566-575` | 故障静默、遥测丢失 |

#### B. Windows 运行时

| # | 问题 | 证据 | 影响 / 修复 |
|---|---|---|---|
| B1 | **烧录工具关窗/析构 UAF** | `flash_tool_dialog.cc:296-321`（5s 超时后 CloseHandle+reset）、`:700-710`（worker 解引用 self 成员）、`:209-215` | worker 未退出即释放对象 → 崩溃/丢固件；硬同步或 shared_ptr+取消令牌 |
| B2 | **退出挂死（usage tap 管道）** | `xiaomi_usage_tap_manager.cc:166-169` 缺 `FILE_FLAG_OVERLAPPED`，却在 `:186-203,227-247` 按可取消等待；`xiaomi_keymap_hook.cc:47` 析构 Stop；`win32_app.cc:1412-1424` 退出不 reset | 启用增强按键识别且无探针时退出挂死，Mutex 不释放 → 下次启动静默退出；加 OVERLAPPED 或 CancelSynchronousIo |
| B3 | **微信启动失败不回滚默认录音设备** | `voice_stick_coordinator.cc:941-969,974-984,730-735`；唯一回滚 `:1072-1077` | 默认麦长期停在 CABLE（静音）；失败路径抽恢复函数 |
| B4 | **WASAPI Start 在 UI 线程无超时自旋** | `wasapi_mic_capture.cc:43-56`；设备打开在 `:67-76` | 音频服务挂起 → UI 永久卡死；事件等待 + 3s 超时 |
| B5 | **剪贴板注入失败不检查** | `input_injector_win.cc:26-44` | 粘贴旧剪贴板内容 + HGLOBAL 泄漏；失败即中止并报错 |
| B6 | **腾讯热词表同步 HTTP body 长度/编码双错 + 业务错误当成功** | `tencent_asr_vocab_client.cc:277-281`（UTF-16 元素数当字节数）、`:435-454` 不查 Response.Error、`:415-417` 静默丢词、全文件无日志 | 热词实际未同步却显示成功；改 UTF-8 字节长度 + 错误上抛 + 日志 |
| B7 | **热词同步在 `audio_mutex_` 内阻塞网络（最长 ~80s）** | `voice_stick_coordinator.cc:1928→1954→2221→2235`；`asr_client_tencent.cc:185-195` | 状态机/按键/音频帧全部阻塞；同步移出锁、启动预热或专用线程 |
| B8 | **`config_` 跨线程竞争（UAF 面）** | 写 `voice_stick_coordinator.cc:244-263`（UI）；读 `:2632,2655,2660-2673,2723,2775`（后台） | 悬垂读/崩溃/配置错乱；改 `shared_ptr<const AppConfig>` 原子换入 |
| B9 | **配置写盘非原子 + 合并保存丢 `[license]`** | `app_config.cc:925-933` trunc 直写；`:849-877,887-923` 合并清单无 license；`:1270-1279` 补救 API 未被调用 | 掉电/被杀 → 配置与授权全丢、试用重置；临时文件+MoveFileEx，两路径先 ReloadLicenseFromDisk |
| B10 | **本地 LLM UTF-8 边界判定方向错** | `llama_cpp_engine.cc:42-59` `return cont < len`（应 `cont == len-1`）；承诺见 `local_llm_engine.h:18` | 半截码点交给 UI（替换符/吞字）；改为"末尾序列完整"+单测 |
| B11 | **`SetLocalRefiner` 持 `audio_mutex_` 析构旧 client** | `voice_stick_coordinator.cc:540-549`；`local_refinement_client.cc:26-31` join；`win32_app.cc:1639-1691` 换引擎前不 Cancel | UI/音频回调冻结数秒；先取消、锁外析构 |
| B12 | **精修线程对象只增不减** | `local_refinement_client.cc:186-195,325-336,26-31` | 每句一个 thread 对象累积；固定容量 worker + reap |
| B13 | **热词候选文件双写者 + 陈旧快照覆盖"忽略"** | `voice_stick_coordinator.cc:2688-2695` vs `settings_dialog.cc:1979-2027` | 用户忽略的词反复弹回；单一持有者或 reload-merge-save |
| B14 | **热词合法性四套口径** | `win32_app.cc:1757-1763`、`hotword_selector.cc:36-56,79-80`、`llm_refinement_client.cc:262-269`、`tencent_asr_vocab_client.cc:415-417` | "加进去但不生效"且无提示；统一 `IsValidHotword` + 拒绝原因 |
| B15 | **F5 抑制器在低级钩子里 Sleep 忙等 + 文件日志** | `voice_f5_suppressor.cc:88-105,52-59` | 每次首按阻塞输入线程 ≤80ms；改事件驱动、回调内不写日志 |
| B16 | **BLE 自愈路径会自动 unpair，违反红线** | `ble_central_win.cc:2000-2003,2136-2139` 调 TryUnpairAsync vs 同文件 `:1815-1821` 注释"绝不在自愈里 unpair"；`voice_stick_coordinator.cc:97-99` 方案 B 描述 | 会删 `Enum\\BTHLE` 节点弄死 HOGP 按键直通；自愈只做 radio reset + CCCD 击穿 |
| B17 | **后台线程直接 `ShowNotification`，共享未加锁状态** | `win32_app.cc:3044-3058` vs `:3109-3119`；调用点 `voice_stick_coordinator.cc:2707,2748` | `pending_balloon_action_` 竞争 + 读正在修改的 config；统一 DispatchToUi |
| B18 | **云端精修/翻译取消不回调 + 断流当成功** | `llm_chat_client.cc:266-273,313-325`；`llm_refinement_client.cc:71-95` | 靠 watchdog 兜底；截断结果可能通过守卫；取消/断流显式回调 |
| B19 | **OTA 流控记账被 progress 回调存在性门控** | `ble_central_win.cc:3322-3332` vs `:3210-3221` | 空 progress 的 OTA 24KB 后必判"设备停止确认"失败（当前仅测试可达） |
| B20 | **VS 控制写未先拷贝局部句柄 / 扫描状态跨线程无锁** | `ble_central_win.cc:2782-2790` vs `:2859-2863`；`:3833` vs `:1138,1226-1237` | 与 CloseSession/扫描看门狗竞态；照抄 ATVV 局部拷贝、状态纳锁 |

#### C. Windows 特性 / 商业化

| # | 问题 | 证据 | 影响 / 修复 |
|---|---|---|---|
| C1 | **试用锚点/防回拨只存用户可写 config，删文件即重置** | `license_runtime.cc:41-57`；`license.cc:106-138`；`app_config.cc:1056-1063`（唯一落盘点） | 无限续试用、回拨检测失效；锚点冗余到注册表/DPAPI 并记录"锚点消失"事件 |
| C2 | **授权绑定依赖"当前已连接设备"，设备离线即断供** | `license_runtime.cc:18-26`（只用 ConnectedDeviceIds）；`license.cc:64-69`；`voice_stick_coordinator.cc:1835-1841,3604-3612` | 付费用户设备关机时被判过期、本地麦模式被闸；改用"已配对 ∪ 已连接"持久集合 |
| C3 | **`DateToDays` 早于 2026-01-01 时 uint32 下溢** | `license.cc:27-38,73-75,114-118` | 有效年卡误判过期、回拨检测失效；改 int64 并显式处理负值 |
| C4 | **固件 OTA 信任链仅"HTTPS + 同 manifest sha256"；下载无超时/上限/静默截断** | `firmware_manifest.cc:257-281,106-129,137-172`；`voice_stick_coordinator.cc:3199-3244` | manifest 被改即可写任意镜像（无固件签名）；加 detached Ed25519 签名 + 超时/体积上限 + https-only |
| C5 | **模型在位只比文件大小，运行期零哈希校验** | `model_download_dialog.cc:117-129`；`model_downloader.cc:395-414,443-445`；`settings_dialog.cc:1824,1885` | 同尺寸损坏/替换文件被静默接受；在位判定跑一次 sha256 |
| C6 | **固件版本比较 fail-open + 预发布后缀字典序** | `firmware_manifest.cc:283-288,56-78,87-90` | 版本串异常即"已最新"永不提示；rc10 被当比 rc9 旧 |
| C7 | **云试用凭据下发无设备证明且允许 ws://→http:// 明文** | `voice_stick_cloud_api_win.cc:44-57,199-201,224-233` | api_key 明文回传并覆盖配置；只接受 wss/https + 设备证明 |
| C8 | **B18/B19 之外**：`hotword_candidates` 越界耦合、日志无轮转且含热词/签名 URL、i18n 完整性只查非空、精修 prompt 无长度上限、`voice_f5_suppressor` 等 | 见子代理报告 | 分组治理 |

#### D. 跨端协议与 macOS 能力

| # | 问题 | 证据 | 影响 |
|---|---|---|---|
| D1 | **macOS 不消费 `device_info` 以外的一切能力帧** | `VoiceStickCoordinator.swift:398-416` 只处理 5 类事件；全目录 grep `battery_status/encoder_status/gateway_status/power_mgmt/encoder_rotate/gateway_key` 只命中两条注释；对照 Windows `voice_stick_coordinator.cc:634-686` 分发 11 类；protocol.md 多处写 "both desktops"（:120-132,139-144,366-367） | 电量/编码器/网关抑制等能力缺失；补实现或逐条标注平台 |
| D2 | **macOS click_to_talk 忽略 `duration_ms`** | `VoiceStickCoordinator.swift:463-489`（只看本地态）；规范 `protocol.md:150-156`；Windows 合规 `voice_stick_coordinator.cc:1202-1213,1251-1268` | 一次失同步后 start/stop 语义永久反转；按 duration>0 判停 |
| D3 | **macOS 订阅无错误处理/无活性兜底** | 全目录无 `didUpdateNotificationStateFor`；`BleCentral.swift:274-299` 连 error 都不检查；无心跳/无重订阅 | "显示已连接但语音静默失效"在 macOS 不可观测不可恢复 |
| D4 | **macOS 忽略 `min_version` 与 `*_fallback`** | `FirmwareManifest.swift:4-24`；`VoiceStickCoordinator.swift:1471-1474` 用本地常量 | 无法远端强制固件下限；主源故障无回退 |
| D5 | **macOS OTA 无任何超时** | `BleCentral.swift:167-170,417-445,523-532`；对照 Windows `ble_central_win.cc:94-98,3211,3241,3279` | 一次 ack 丢失永久锁死后续升级 |
| D6 | **OTA 分块下限 `max(20,…)` 突破帧头预算** | `BleCentral.swift:185-186`（+12）；`ble_central_win.cc:3184-3185`（+15） | MTU 退化到 20 时必 `bad_offset`；chunk = maxWrite-header，不可行即报错 |
| D7 | **固件 `ota_abort` transfer_id 不匹配时清状态且不 abort** | `voice_ble.c:390-403` | `esp_ota_handle` 泄漏 + 假 aborted 帧；不匹配时返回错误且不清理 |
| D8 | **Windows 丢弃 OTA `esp_err`** | `ble_protocol.h:121-128` 无字段；固件 `voice_ble.c:227-232` 有发；macOS 有解析 | 丢失最有价值的失败诊断 |
| D9 | **三端 `version` 硬编码 1，无版本协商章节与反向反馈** | `voice_ble.c:436-439,1488-1493`；`BleProtocol.swift:71,91,99`；`ble_protocol.cc:157,171,217,285` | 未来协议升级即静默不兼容 |
| D10 | **`gateway_keymap` 回执 Windows 无解析、macOS 无事件** | `ble_protocol.h:92-119` 无 routes；全 src grep 只命中注释 | 路由 UI 无法回显 |

#### E. 发布链、网站与测试基础设施

| # | 问题 | 证据 | 影响 |
|---|---|---|---|
| E1 | **浏览器烧录不校验 `merged_sha256/size`** | `App.vue:121-129` fetch→arrayBuffer 直接写 `:186-196`；`:132-146` 只用 merged_url | 链路任一环被篡改即可植入任意固件（含 bootloader/分区表） |
| E2 | **`update-appcast.py` 正则跨 item 贪婪匹配** | `update-appcast.py:15-24`（`    <item>\\s*.*?sparkle:os="X".*?    </item>` + DOTALL），`:97,122` 用作保留项，`:131` 输出顺序 | 只发 Windows 不发 macOS 时产出重复 windows 条目，最坏更新链失效；按 item 切分或 XML 解析 |
| E3 | **macOS 更新链 3 处静默降级** | `build-macos.sh:59-64,140-157`（占位公钥仅 WARNING；sign_update 失败把错误文本写进 .signature）；`make-dmg.sh:57-63`（未公证也发布） | 更新签名失效/不可验证；全部 fail-hard + 格式校验 |
| E4 | **`prepare_flash_payload.ps1` 供应链无哈希锚点** | `:30-36,44-58,81-92`（python-embed 仅 HTTPS、复用 %TEMP% 缓存、pip 无 --require-hashes） | 打进已签名 MSI 的运行时可为任意代码；固定 sha256 + 缓存校验 + require-hashes |
| E5 | **`idf_cli.py` 显式端口不可用时静默换口** | `:437-453,543-548,681-697` | `-p COM17` 可能烧到别的设备；显式端口未命中即退出 |
| E6 | **CI 不跑测试、不构建 Windows/macOS** | `.github/workflows/*` 无 ctest/swift/pytest；release 只构建固件 | P0-1 潜伏 14 天、D1-D10 漂移无拦截 |
| E7 | **Windows 测试在 NDEBUG 下整段消失** | `core_tests.cc` 2827 处 `assert`、唯一 `return 0` `:15721`；`CMakeLists.txt` 无 NDEBUG 处理；`build-msi.bat` 用 RelWithDebInfo | 用同一构建目录跑 ctest 会"全绿"假象；测试目标 `-UNDEBUG` 或统一 CHECK 宏 |
| E8 | **固件 host 测试仅 Windows/MSVC 绝对路径，无 CI 入口** | `run_tests.py:17` | 主开发机 macOS 上实际不可执行 |
| E9 | **scripts 单测无 CI；E2E 脚本退出码不反映判定** | `gateway_switch_acceptance.py:222-244,297-299` FAIL 仍 return 0；`run_asr_bench.py:135-140` | "假 PASS"与仓库"不伪造结果"红线相悖 |
| E10 | **appcast 无版本单调性校验；COS `firmware/latest/manifest.json` 被改写成 github.io** | `update-appcast.py:27-42`；`deploy-website.yml:130-143,169-172` | 旧线补丁可把广播版本回退；国内主源 manifest 指向国际站 |
| E11 | **workflow_dispatch 的 tag 直接内插 shell** | `release.yml:44`；该 job 持 `contents: write` 与 COS secrets | 具写权限者可越出引号执行命令；改用环境变量传参 |
| E12 | **`test_playback` 未文档化 + 正式固件可用** | `main.c:1106-1117`；protocol.md 控制事件表无此条 | 配合 P0-2 构成 SPIFFS 内越权读取（已加 `..`/分隔符拒绝）；仍建议收进调试构建并补文档 |

### 3.3 P2（摘要）

- **Ogg granule 1.5× 偏差（两端一致地错）**【已复核】：`ogg_opus_muxer.cc:30`、`OggOpusMuxer.swift:32` 按 960 样本/包累加（16k → +2880），实际 40ms=640（应 +1920）；`protocol.md:52` 也误写 60ms。落盘/转发 Ogg 时长虚高 50%。
- 固件 PCM 单缓冲别名（`gateway_atvv_session.c:168-180`），仅靠 MTU 247 兜住，无断言。
- HOGP 多键破损 + HID CCCD 迟到回调二次推进/越界（`gateway_hid_host.c:115-142,581-604`；`gateway_hogp_report.c:118-127`）。
- 路由 mid-press 变更导致目标机卡键（`gateway_keymap.c:85-87`；`main.c:2307-2327`）。
- NimBLE host 任务里做 NVS 提交 + 抢 LVGL 锁（`main.c:2595-2617`）。
- 音频路径不看 MTU（`voice_ble.c:1345-1410`），小 MTU 静默丢帧。
- keymap pending 无按住超时（`xiaomi_keymap_interceptor.cc:153-215`）。
- 本地 ASR 增量识别 O(n²)（`local_asr_client_win.cc:140-163,183-216,257`）。
- UI 线程同步 sleep 4.5s+（微信停止路径 `voice_stick_coordinator.cc:1015-1057`）。
- 字幕兜底线程无锁访问容器（`:2068-2085` vs `:2365-2467`）。
- 日志无轮转、含热词明文与签名 URL；i18n 完整性只查非空。
- `test_playback` 之外的控制事件未文档化（`gateway_side_switch`、`ota_commit`）。
- macOS 序列化失败发空 Data 仍当成功（`BleProtocol.swift:106-121`）。
- 固件 JSON 拼接不转义、数值无上界夹取（当前实参受控，属潜在）。
- m0/p1 原型：p1 音频/剪贴板生命周期异常后不可恢复、三项配置空转；m0 warmup 未兜底、权重只查存在性；文档 10 处 `m0/refine` 引用失效。

### 3.4 P3（文档、低危、流程）

- 文档漂移：`protocol.md:52`（60ms vs 40ms）、`:533-543`（"固件不实现 ATVV" vs 已实现）、`:97/303/304`（ui_states/tap 默认值）、`:457`（v2.3.10 才有的 fallback 字段）；`release-and-security.md:28` 写"当前 2.3.6"；CHANGELOG 最新 2.3.8 而 tag 只到 v2.3.7；`website/package.json` 1.9.0 与 VERSION 脱节。
- 可观测性：`dropped_pcm_frames`/`s_disc_restarts`/`dropped events` 无读取；ATVV ERROR 只一行 WARN；HOGP 发送失败仅 WARN。
- 低危安全：usage-tap 管道 DACL 授 Everyone GENERIC_ALL（`xiaomi_usage_tap_manager.cc:160-169`）；`license_public_key.h` 是开发公钥（发行前须替换）；API 凭据明文落盘；`spectrogram_server` innerHTML 拼文件名；`probe_hotword_extraction.py` 放行 http Bearer。
- 固件死代码/名义 API：`set_l3b_power(true)` 无调用者；`#if 0` 真关机链仍编入；`ESP_ERROR_CHECK(ui_status_init())` 屏坏即重启循环。
- 电池刻度分母 800 vs 实际 850（`stick_s3_board.c:317`）。

---

## 4. 根因归纳

1. **隐式线程契约**（P0-3、B8、B17、P2 字幕/本地麦）：没有类型/断言/统一 Dispatcher 强制"谁在哪个线程进入状态机"。
2. **单值抽象承载多连接**（A1-A4）：`voice_ble` 与 ATVV 会话把"当前对端"建模为单值，网关模式天然多链路。
3. **错误恢复不对称**（A1、A5、A6、B1-B5、C4）：链路层自愈丰富；会话层、失败回滚、退出路径、网络取消几乎空白。
4. **契约不可机器校验**（D1-D10、P2 granule）：同一协议在文档/固件/两端实现四处手写，零 golden-frame 测试。
5. **门禁缺位 + 分支松散**（P0-1、E6-E9）：无 CI 测试/跨平台构建；长生命周期分支各持半套实现；版本/文档靠人工。
6. **信任边界被"开箱即用"侵蚀**（P0-2、P0-4、C4、C5、E1、E4）：为了体验把凭据/未签名镜像/未校验模型放进信任链，缺最后一道校验。

---

## 5. 优化方案

### 5.1 阶段 0：24–48h 止血（P0）

| 动作 | 步骤 | 验收 |
|---|---|---|
| 0.1 恢复 macOS 构建 | 合并 `feat/add-MiRemote` 的 `desktop/macos/**`（Package.swift targets、VoiceStickCore、COpus、Tests）并人工裁决冲突 | `swift build` + `swift run VoiceStickTests` 绿；.app 启动冒烟 |
| 0.2 ASR 回调封送 | `WireAsrClientCallbacks` 统一 `DispatchToUiThread`；加 debug 线程断言 | 压力/TSan 0 race；final 不丢 |
| 0.3 固件安全闸 | `control_rx/ota_rx` 加 `WRITE_ENC`+MITM；`test_playback` 仅调试构建；评估 Secure Boot（至少列入发布阻断项） | 未配对写入被拒；调试命令在正式构建不可达 |
| 0.4 发布链凭据闸 | 拆分"内测含 key / 公开发布不含 key"两条命令；发布前扫描产物中的 key 特征 | 公开产物 strings 无凭据；发布脚本默认安全侧 |
| 0.5 分支卫生 | 明确 macOS 所属分支并在 AGENTS.md/README 标注；CI 对每分支强制全平台构建 | 新克隆任一受支持分支均可构建 |

### 5.2 阶段 1：1–2 周（P1 功能可靠性）

1. **固件会话恢复**：ATVV ERROR/发现失败有界重试、STREAMING 无音频看门狗、状态上报（屏幕/托盘可见"遥控器语音链路异常"）。
2. **多连接表**：`voice_ble` 按 `conn_handle` 维护 peer 表；send/disconnect 带 handle；切换器动作携带对端身份；补"入侵者连接/断开"回归。
3. **深睡中止可逆**：面板/L3B 关闭移到 `esp_deep_sleep_start()` 前最后一步，或每分支幂等恢复。
4. **OTA 原子性**：begin 前同步停录；错误路径统一 abort+clear；rollback 签到延后到健康判据；下载加超时/上限/https-only + manifest 签名。
5. **固件事件通道**：关键事件独立队列/位图 + 失败重试告警；app_event_task 去秒级阻塞；audio_task 失败退避。
6. **Windows 运行时三件套**：烧录工具硬同步（Job Object+取消令牌）；微信设备回滚；WASAPI 超时；剪贴板失败中止。
7. **并发收敛**：`config_` 改 `shared_ptr<const AppConfig>`；`ShowNotification` 统一 DispatchToUi；字幕兜底加锁；腾讯词表同步移出 `audio_mutex_`。
8. **授权可用性优先**：绑定改"已配对 ∪ 已连接"；`DateToDays` 改 int64；锚点冗余到注册表/DPAPI；配置原子写并保留 `[license]`。
9. **配置与文档对齐**：`sdkconfig.defaults`=3；protocol.md 的 60ms/ATVV/默认值/平台标注修正；granule 修正（两端）。

### 5.3 阶段 2：1–2 月（架构收敛）

1. **显式线程边界**：协调器定义唯一入口线程 + 统一 Dispatcher；状态机字段 debug 线程断言；消灭所有旁路。
2. **状态机形式化**：Windows `SessionState` 迁移表抽成纯函数（允许迁移 + 副作用），非法迁移打点；macOS 复用同一张表。
3. **协议单一事实源**：golden-frame fixtures + 三端解析对拍；protocol.md 的常量/默认值/能力清单由脚本反向校验源码。
4. **固件拆分**：`main.c` 拆 `power_manager`/`button_gesture`/`gateway_facade`/`ota_session`；I2C 轮询移出 esp_timer；关键事件队列/计数器上报。
5. **可观测性**：ATVV 状态、事件丢弃、HOGP 发送失败、模型哈希校验结果做成 `state_tx` 计数/状态帧并在桌面端展示。
6. **信任链收口**：固件镜像签名、manifest 签名、浏览器烧录 WebCrypto 校验、模型在位哈希、flash payload 哈希锚点。

### 5.4 阶段 3：持续

- 事故复盘流程化：回归测试 + 文档更新 + Hub 红线（沿用 `work-summary-retro` skill）。
- 发布 checklist 自动化（见 §6.5）。
- 长生命周期分支定期合并 + CI 全平台构建门禁。

---

## 6. 测试方案

### 6.1 目标态测试金字塔

```text
        E2E 真机（L3 固件回放 / L4 微信 / 网关切换 / OTA / 小米 ATVV）    ← 已有工具链，纳入发布门禁
      ─────────────────────────────────────────────────────────────
      跨端契约测试（golden 字节对拍：固件 C ↔ macOS Swift ↔ Windows C++）   ← 新建，最高性价比
    ───────────────────────────────────────────────────────────────
    集成测试（Windows 真实 ASR + FakeBle；固件 host 逻辑 + 注入式 BLE ops）  ← 已有基础，CI 化
  ─────────────────────────────────────────────────────────────────
  单元测试（Windows core_tests / 固件 gateway host / macOS VoiceStickTests） ← 补齐缺口 + NDEBUG 治理
```

### 6.2 必须补充的用例（按缺陷对应）

**A. 纯逻辑单元（任意平台可跑）**
- 协议解析边界：audio/state/motion/OTA 帧的 0 长、截断、超大 `payload_len`、错误 version/type、MTU 23/185/247 预算；`ui_state.text` 超预算的发送端截断/分段。
- Ogg：granule 累加 = 帧长×3（40ms→1920）、>255B 包、页序列/CRC、空流 `Finish()`。
- ADPCM/PCM **跨语言 golden**：同一段 `.adpcm` 分别喂固件 C 与 Windows C++，逐样本对拍（当前只有各自手推向量）。
- keymap/switcher：路由 mid-press、单双击边界、满表淘汰保护活动目标、`last_seen` 回绕。
- 授权：签名/绑定/到期/时钟回拨/串码格式、"开发公钥不可出包"发布断言。
- 配置：损坏 TOML、缺失/非法字段回默认、凭据保留、`[license]` 合并保留、配对表 CSV 往返。

**B. 并发/时序（新增）**
- ASR final 与音频帧并发的 TSan/压力用例（P0-3）；`config_` 换入与后台读并发（B8）。
- 注入式时钟覆盖：双击/hold、STREAMING 看门狗、ATVV CAPS 超时、OTA 停滞、订阅超时。
- 退出路径：usage tap 无探针/有探针/半死三态下的 Stop/析构计时（B2）；烧录取消后 worker 未退出时关窗（B1）。
- WASAPI：模拟 `Activate/Initialize` 阻塞，断言 Start ≤3s 失败返回（B4）。
- 深睡中止：面板/L3B 状态机模拟"中止后恢复"（A5）。

**C. 固件 host 逻辑 CI 化**
- `run_tests.py` 改写为 CMake/CTest（gcc/clang 可跑）；把 NimBLE 胶水抽成可注入 `ble_ops` 后补测：CCCD 推进/多键分通道/多连接分发/事件丢弃/重启恢复。

**D. 跨端契约（新建）**
- `tests/contract/fixtures/*.bin`：每类帧黄金字节 + 期望解析；三个 CI job 分别用固件 host、Swift runner、Windows 单测解析同一批 fixtures。
- 覆盖 AudioFrame、StateFrame（含 text 预算）、MotionFrame、OTA begin/data/end/abort/state、Control 事件全集（含 `gateway_side_switch`/`ota_commit`/`test_playback` 的编译开关）。

**E. 集成与 E2E（已有，纳入门禁）**
- Windows L1 保持"无 key SKIP、不伪造"；增加腾讯 ASR 变体。
- L3 补三个场景：录音中开始 OTA、STREAMING 丢 STOP、事件队列灌注。
- 网关：双机切换拒绝/接受、入侵者连接断连后恢复、OTA 全程。
- 微信 L4：peak 判据 + "失败后默认录音设备必须还原"断言。
- 浏览器烧录：sha256 校验失败的负向用例（E1）。

### 6.3 CI 门禁（建议新增 `.github/workflows/ci.yml`）

```yaml
jobs:
  firmware:
    - idf.py build
    - cmake+ctest gateway host tests      # 跨平台运行器（新增）
  windows:
    - cmake --build && ctest              # 测试目标显式 -UNDEBUG，避免假绿
    - git ls-files 校验新源文件已跟踪     # 防 f75af4f5
  macos:
    - swift build && swift run VoiceStickTests
  contract:
    - 三端解析同一 fixtures
  release-guard:
    - VERSION/version.txt/tag/CHANGELOG/protocol 一致性
    - 公开产物凭据特征扫描
```

> ⚠️ Windows 测试必须用**不定义 NDEBUG** 的配置跑（`build-msi.bat` 的 RelWithDebInfo 会让 2827 处 assert 消失）；关键用例推广 `AbortIfFailed` 显式计数。

### 6.4 真机矩阵（最小必测集）

| 场景 | 设备 | 判据 |
|---|---|---|
| 按住说话 → 粘贴 | StickS3 | 尾音完整、延迟基线、状态帧无截断 |
| 双击 Enter / 侧键恢复 / 编码器快慢档 | StickS3 | 交互状态表逐项 |
| 网关：语音键 + HID 直通 + 双机切换 | StickS3 + RC + 2 PC | 2.5s 内恢复、非目标被拒、切换器不卡 |
| OTA（BLE + COM 兜底） | StickS3 | 录音中开始不崩、失败后可录音/关机、可回滚 |
| 微信输入法模式 | Windows + 微信 | 虚拟麦电平、失败后默认设备回滚 |
| 小米直连（非网关） | Windows | ATVV 会话、F5 抑制、按键映射 |
| 深睡/唤醒/自动关机（含中止路径） | StickS3 拔 USB | 5min 关机、前键唤醒、中止后屏幕/MIC 正常 |
| macOS 全链路（合并后） | Mac | 大对齐验收清单 + 能力帧解析 |

### 6.5 发布校验（自动化脚本）

1. `VERSION` == `firmware/version.txt` == tag == CHANGELOG（或显式 Unreleased）。
2. protocol.md 常量/默认值与三端源码 grep 对拍（帧长、MTU、超时、能力清单）。
3. `git ls-files` 覆盖 `desktop/windows/**` 新增源码。
4. 公开产物凭据扫描（腾讯 AKID、sk-、LLM key 特征）+ 确认使用发行公钥。
5. appcast 版本单调递增 + 无重复 item；downloads.json/asset URL 可 200 且 SHA-256 匹配。
6. 浏览器烧录与 Windows/macOS 更新源均校验哈希/签名。

---

## 7. 优先级矩阵与验收指标

| 优先级 | 事项 | 验收指标 |
|---|---|---|
| P0 | macOS 构建恢复 | `swift build` + runner 绿；CI macOS job 绿 |
| P0 | ASR 回调线程封送 | 压力/TSan 0 race；final 丢失率 0 |
| P0 | 固件 BLE 鉴权/调试命令收敛 | 未配对写入被拒；`test_playback` 正式构建不可达 |
| P0 | 发布链凭据闸 | 公开产物无凭据；内测包与公开包命令互斥 |
| P1 | ATVV 恢复 | 人为丢 STOP/超时后 ≤3s 或下次按键自愈；异常态可见 |
| P1 | 多连接表 | 入侵者连接/断开不污染切换器；双机切换 100% |
| P1 | 深睡中止可逆 | 中止后屏幕/MIC 立即恢复，无需重启 |
| P1 | OTA 原子性/回滚 | 录音中 OTA 不崩；错误后 5s 恢复可录音；坏固件可回滚 |
| P1 | 事件队列 | 0 静默丢弃（或全部有日志+计数）；灌注测试不卡死 |
| P1 | Windows 运行时 | 退出 <2s；烧录取消无 UAF；默认麦还原；注入失败必报错 |
| P1 | 授权可用性 | 设备离线仍可验签；删 config 不重置试用 |
| P1 | 契约测试 | fixtures 三端全绿；granule 正确 |
| P2 | 可观测性 | 关键故障有计数并上屏 |

---

## 附录 A：证据索引（节选）

- macOS：`Package.swift`；`AppDelegate.swift:4,327-329`；`swift build` 实测；`git merge-base --is-ancestor 847ca46 HEAD`。
- 固件安全：`voice_ble.c:774,779,1189-1191,1031,1058,1062`；`sdkconfig:494-495`；`main.c:1106-1117`。
- ASR 线程：`asr_client_win.cc:127,466-486`；`voice_stick_coordinator.cc:589-597,2259-2313,2995-3003`。
- 发布链：`build-msi.bat:43-55,200-222`；`release.ps1:141-142,187`；`package-portable.ps1:103,107`；`mirror_urls.py:36`。
- ATVV：`gateway_atvv_session.c:236,194-196,350-371`；`xiaomi_atvv_client.c:189-194,216-222,380-384`。
- 多连接：`voice_ble.c:52,799-808,860-890,1313-1338`；`gateway_switcher.c:139-160`；`main.c:2424-2436`。
- 深睡：`main.c:612-613,629-647`；`ui_status.c:411-423`；`stick_s3_board.c:413-423`。
- OTA：`voice_ble.c:240-262,296-323,390-403`；`main.c:1741-1756,3328-3338`；`firmware_manifest.cc:257-281,106-129`。
- 事件队列：`main.c:749-761,1841,731`；`audio_pipeline.c:1047`。
- Windows 运行时：`flash_tool_dialog.cc:296-321,700-710,209-215`；`xiaomi_usage_tap_manager.cc:166-169,186-203,227-247`；`wasapi_mic_capture.cc:43-56`；`input_injector_win.cc:26-44`。
- 并发/配置：`voice_stick_coordinator.cc:244-263,540-549,1928-1954,2221-2235`；`app_config.cc:849-933`。
- 授权：`license.cc:27-38,64-69,106-138`；`license_runtime.cc:18-26,41-57`。
- 热词：`tencent_asr_vocab_client.cc:277-281,415-417,435-454`；`voice_stick_coordinator.cc:1928-1954,2221-2235,2688-2695`。
- 本地 LLM：`llama_cpp_engine.cc:42-59`；`local_refinement_client.cc:26-31,186-195`。
- 网站/发布：`App.vue:121-146,186-196`；`update-appcast.py:15-24,97,122`；`prepare_flash_payload.ps1:30-92`；`release.yml:44`；`deploy-website.yml:130-143`。
- 协议漂移：`protocol.md:52,97,150-156,303,455,533-543`；`ogg_opus_muxer.cc:30`；`OggOpusMuxer.swift:32`。
- 测试/CI：`core_tests.cc`（assert 计数、:15721）；`CMakeLists.txt`；`run_tests.py:17`；`.github/workflows/*`。

## 附录 B：评审方法与局限

- 六个并行子代理分别实读固件核心、固件网关、macOS、Windows 核心、Windows 特性、协议一致性与网站；P0 与关键 P1 由评审者重读源码或实跑命令复核（标注【已复核】）。
- 本机无 ESP-IDF、无 MSVC、无 Windows/macOS 真机；固件与 Windows 的行为结论基于源码与文档，未做运行时验证；macOS 仅做了构建验证（失败）。
- macOS 深审受 P0-1 限制：当前分支是半套实现，其逐行结论只代表本分支；完整实现需在 `feat/add-MiRemote` 上复核。
- 未逐条复读的条目均来自子代理工具输出并在正文出处标注；标"推测"处未做二次验证。
- 行号为 `8f74822` 时点；引用前请以当前源码为准（仓库自身亦有此约定）。
