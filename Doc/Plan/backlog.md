# 遗留项 Backlog（9-22 架构评审 + 10-07 跟进评审）

> 上游：`Doc/Plan/architecture-review-and-optimization-2026-09-22.md`（缺陷编号 P0/A/B/C/D/E 沿用其 §3 清单）、`Doc/Plan/architecture-review-followup-2026-10-07.md`（N 系列与阶段划分）。
> 建立：2026-10-07（跟进评审 N4 立项动作 0.4）。**状态变更直接改本表并注明日期/commit**；评审文档只读快照，进度以本文件为准。
> 状态：`open` 未开始 · `partial` 部分完成 · `blocked` 需产品决策/真机/跨环境 · `closed` 已关闭（见关闭记录）。
> 9-22 三批推进已关闭项不在此登记（P0-1/P0-3/P0-4、A5、A18、B4/B5/B6/B10、D2/D4/D5/D8、E1/E2/E5/E6/E11 等，见上游推进记录）。
> 标注「9-22 未复核」的条目沿用评审结论，认领前先复核；标「10-07 抽查」的有本次实测行号。

## 0. 阶段 0 在办（跟进评审 §5.1）

| ID | 事项 | 状态 | 备注 |
|---|---|---|---|
| 0.1 | 跨端契约 fixtures（golden-frame 多端对拍，进 CI） | **closed（Win+macOS）** | `tests/contract/` 48 黄金样本（state 14/power_mgmt 2/ota_state 5/binary 5/control 18/ota_control 4），两端测试随 CI 运行；**固件端 reader 余项**：待 E8 host 构建（`run_tests.py` 跨平台化）后消费同一 manifest |
| 0.2 | release-guard 脚本 + CI job | **closed** | `scripts/release_guard.py`（7 项检查）+ `test_release_guard.py`（13 用例）+ `ci.yml` release-guard job |
| 0.3 | 网站 `npm run build` 进 CI | **closed** | `ci.yml` website job（npm ci + build，lock 缓存） |
| 0.4 | 本 backlog 建立 | **closed** | 本文件 |

## 1. P0

| 编号 | 事项 | 状态 | 验收 / 备注 |
|---|---|---|---|
| P0-2 | 固件 BLE 鉴权：`control_rx`/`ota_rx` 加 `WRITE_ENC`+MITM、Secure Boot v2 + flash encryption、`test_playback` 收进调试构建 | **blocked**（产品决策 + 真机 + 一次性 eFuse） | 10-07 抽查仍开放：`voice_ble.c:774` WRITE_NO_RSP、`:1191` sm_mitm=0、sdkconfig 无启用项；最低限度先做「README 威胁模型 + 发布阻断项」 |

## 2. P1 — 固件（A 类，9-22 编号）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| A1 | ATVV ERROR 终局：冷却重试 + 状态上报 | open | 9-22 未复核 |
| A2 | ATVV 发现链终局：CHRS 退避重试 + 「ATVV 不可用」上报 | open | 9-22 未复核 |
| A3 | STREAMING 无音频看门狗（合成 PRESS_UP 收尾） | open | 9-22 未复核 |
| A4 | `voice_ble` 多连接表（按 conn_handle 维护 peer，替换单值） | open | **10-07 抽查**：`voice_ble.c:52` 仍单值 `s_conn_handle`；Hub「显示层落表兜底」绕行仍在 |
| A6 | OTA 错误路径统一 `esp_ota_abort` + `ota_clear_state`；rollback 签到延后 | open | 9-22 未复核 |
| A7 | app_event 关键事件处理 | **partial** | 10-07 抽查：`main.c:764` 关键标记 + 20ms 等待 + 日志已在；重试/计数上报未见 |
| A8 | audio_task 错误分支热循环 → Task WDT（失败退避 + 主动收尾） | open | 9-22 未复核 |
| A9 | esp_timer 任务当工作队列：I2C 轮询与硬件初始化移出 timer | open | 9-22 未复核 |
| A10 | 编码器降级不可恢复 + I2C 总线泄漏 | open | 9-22 未复核 |
| A11 | BMI270 加载失败仍报 present | open | 9-22 未复核 |
| A12 | `tap_enabled` 默认值与 protocol.md 相反（实现/文档取一） | open | 9-22 未复核 |
| A13 | `tap_sensitivity` 无范围校验 + 缺字段静默落盘 | open | 9-22 未复核 |
| A14 | `ui_state.text` 超 MTU 预算：发送端零校验、固件硬截断 | open | 9-22 未复核；与 state_tx MTU 红线相关 |
| A15 | `gateway_keymap` 回执 400B 缓冲发不出 | open | 9-22 未复核 |
| A16 | 双击收尾不清 owner / click_to_talk 忽略 remote up | open | 9-22 未复核 |
| A17 | 主机无响应看门狗是死代码（timer 从不 start） | open | 9-22 未复核 |
| A19 | PMIC IRQ 先 enable 后清源 + ISR 内队列失败永久 disable | open | 9-22 未复核 |
| A20 | 错误路径吞掉（ATVV TX rc / HOGP rc / power_log IO） | open | 9-22 未复核 |

## 3. P1 — Windows（B 类）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| B1 | 烧录工具关窗/析构 UAF（硬同步 + 取消令牌） | open | **10-07 抽查**：`flash_tool_dialog.cc:209-215` 仍 Cancel+5s 有界等待即释放 |
| B2 | usage tap 管道缺 OVERLAPPED → 退出挂死 / Mutex 不释放 | open | **10-07 抽查**：manager 文件内仍无 `FILE_FLAG_OVERLAPPED` |
| B3 | 微信模式启动失败不回滚默认录音设备 | open | 9-22 未复核 |
| B7 | 腾讯热词同步移出 `audio_mutex_` | open | 9-22 未复核（B6 已修） |
| B8 | 协调器 `config_` 跨线程竞争 → `shared_ptr<const AppConfig>` 原子换入 | open | **10-07 抽查**：`voice_stick_coordinator.h:644` 仍普通成员；阶段 1 首位 |
| B9 | 配置写盘非原子 + 合并保存丢 `[license]`（临时文件 + 原子替换） | open | 与 B8 同批；数据丢失面最大 |
| B11 | `SetLocalRefiner` 持 `audio_mutex_` 析构旧 client | open | 9-22 未复核 |
| B12 | 精修线程对象只增不减 | open | 9-22 未复核 |
| B13 | 热词候选文件双写者 + 陈旧快照覆盖「忽略」 | open | 9-22 未复核 |
| B14 | 热词合法性四套口径 → 统一 `IsValidHotword` | open | 9-22 未复核 |
| B15 | F5 抑制器低级钩子内 Sleep 忙等 + 文件日志 | open | 9-22 未复核 |
| B16 | 自愈路径调 `TryUnpairAsync` 与「绝不 unpair」红线冲突 | **待核查** | 10-07：红线注释已在（`:1845`），但 `TryUnpairAsync` 调用点仍在（`:595/:1827`）——需确认调用路径均非自愈 |
| B17 | 后台线程直接 `ShowNotification`（统一 DispatchToUi） | open | 9-22 未复核 |
| B18 | 云端精修/翻译取消不回调 + 断流当成功 | open | 9-22 未复核 |
| B19 | OTA 流控记账被 progress 回调存在性门控 | open | 9-22 未复核 |
| B20 | VS 控制写未拷贝局部句柄 / 扫描状态跨线程无锁 | open | 9-22 未复核 |

## 4. P1 — 商业化/授权（C 类）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| C1 | 试用锚点/防回拨冗余到注册表/DPAPI（删 config 不重置） | open | 9-22 未复核 |
| C2 | 授权绑定改「已配对 ∪ 已连接」持久集合（设备离线不断供） | open | 9-22 未复核 |
| C3 | `DateToDays` uint32 下溢 → int64 | open | 9-22 未复核 |
| C4 | 固件 OTA manifest detached 签名 + 下载超时/上限/https-only | open | 9-22 未复核 |
| C5 | 模型在位 sha256 复核（非只比大小） | open | 9-22 未复核 |
| C6 | 固件版本比较 fail-open + 预发布后缀字典序 | open | 9-22 未复核 |
| C7 | 云试用凭据下发无设备证明 + 允许 ws://→http:// | open | 9-22 未复核 |
| C8 | 日志轮转/热词与签名 URL 明文、精修 prompt 长度上限等分组治理 | open | 9-22 未复核 |

## 5. P1 — 跨端/ macOS（D 类）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| D1 | macOS 消费 device_info 以外能力帧（battery/encoder/gateway/power…） | open | 9-22 未复核 |
| D3 | macOS 订阅错误处理/活性兜底/重订阅 | open | 9-22 未复核；「显示已连接但语音静默失效」在 macOS 不可观测 |
| D6 | OTA 分块下限 `max(20,…)` 突破帧头预算（MTU 20 必 bad_offset） | open | 9-22 未复核 |
| D7 | 固件 `ota_abort` transfer_id 不匹配时清状态且不 abort | open | 9-22 未复核 |
| D9 | 三端 `version`=1 硬编码，无协议版本协商 | open | 9-22 未复核 |
| D10 | `gateway_keymap` 回执 Windows 无解析 / macOS 无事件 | open | 9-22 未复核 |

## 6. P1 — 发布与测试设施（E 类）

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| E3 | macOS 更新链 3 处静默降级（占位公钥/sign 失败/未公证）→ fail-hard | open | 9-22 未复核 |
| E4 | `prepare_flash_payload.ps1` 供应链哈希锚点（require-hashes） | open | 9-22 未复核 |
| E7 | Windows 测试目标显式 `-UNDEBUG` 或统一 CHECK 宏 | **partial** | CI 用 Debug 构建 + ctest 已实跑全绿（2026-10-07，断言真实生效）；本地 RelWithDebInfo 假绿面仍在 |
| E8 | 固件 gateway host 测试改跨平台 CTest 并进 CI | open | **10-07 抽查**：`run_tests.py:17` 仍硬编码 MSVC vcvars 路径 |
| E9 | E2E 脚本退出码反映判定（不假 PASS） | **partial** | 9-22 已修两项（gateway_switch/run_asr_bench），其余未复核 |
| E10 | appcast 单调性（✅已随 E2 修）+ COS `firmware/latest/manifest.json` 指向国内域名 | **partial** | manifest 指向未复核 |
| E12 | `test_playback` 收进调试构建 + 补 protocol.md 文档 | open | 与 P0-2 同批 |

## 7. N 系列（10-07 跟进评审）与 P2/P3 摘要

| 编号 | 事项 | 状态 | 备注 |
|---|---|---|---|
| N1 | macOS App target 16.8k 行零测试（状态机/配置/Ogg 下沉 Core） | open | 阶段 2 首位；测试 runner 自带 NOTE 印证缺口 |
| N2 | 跨端契约零 fixtures（golden-frame 对拍） | open | 即 0.1，下一批 |
| N3 | CI 缺 website/release-guard | **closed（本批）** | 两 job 已入 `ci.yml`；契约 fixtures 归 0.1 |
| N4 | 遗留项无跟踪载体 | **closed（本文件）** | 状态变更规则见头部 |
| N5 | 三份 Hub 漂移（CLAUDE/CODEBUDDY 合并行 + 多余空行） | **closed（本批）** | 已修复；`release_guard` hub 检查守护 |
| N6 | 仓库卫生（根 9 个 .bat / m0·p1 跟踪矛盾 / skills 双份 / superpowers） | open | 阶段 3 |
| N7 | 上帝对象与接口膨胀（67 virtual / 最大单文件） | open | 阶段 2 |
| N8 | `core_tests.cc` 单文件 + main() 手工注册 | open | 阶段 2 |
| N9 | 本地化三套实现无奇偶守护（网站 zh/en 已由 guard 覆盖） | open | 桌面键清单后补 |
| N10 | Doc/Plan 89 篇状态标注 + 可观测性计数 | open | 阶段 3 |
| 既往 P2 | Ogg granule 1.5× 偏差 | **closed（本批）** | 两端改由帧采样常量推导（`kFrameSamples`/`frameSamples`），guard `frame-ms` 守护；Windows 侧由 CI ctest 复核 |
| 既往 P3 | protocol.md 帧时长 60ms 写错 | **closed（本批）** | 改 40ms；guard `frame-ms` 检查文档与固件 `AUDIO_FRAME_MS` 一致 |
| 既往 P2 其余 | PCM 单缓冲、HOGP 多键、mid-press 路由、NimBLE 任务内 NVS、音频路径不看 MTU、本地 ASR O(n²)、UI 同步 sleep、字幕兜底无锁、日志轮转 | open | 9-22 未复核 |
| 既往 P3 其余 | 可观测性计数无读取、低危安全（管道 DACL/开发公钥/明文 URL）、固件死代码、电池刻度 800 vs 850 | open | 9-22 未复核 |

## 8. 关闭记录

| 日期 | 项 | 验证 |
|---|---|---|
| 2026-10-07 | 0.2 release-guard（13 单测 + 真实仓库 7/7 PASS）、0.3 网站 CI、0.4 backlog、N3、N4、N5、granule、protocol 60ms | 本地：`test_release_guard.py` 13/13、`release_guard.py` 全绿、`npm run build` ✅、macOS `swift build` + 552/552 ✅；CI 已复核见下行 |
| 2026-10-07 | **0.1 契约 fixtures 两端落地**：`tests/contract/`（生成器独立手搓字节 + manifest.json 48 样本）+ Windows `TestContractFixtures`（ctest）+ macOS `runContractFixtureTests`（swift run）；键序不构成契约、expect 只取公共字段（单端缺口清单见 `tests/contract/README.md`） | 本地 macOS `swift run VoiceStickTests` **644/644**（契约 48 样本对拍行输出）；Windows 侧由 CI ctest 复核 |
| 2026-10-07 | **CI 六 job 首次全绿**（此前连续 15+ 次恒红，E6 门禁从「形同虚设」变真闸）：① macOS `BatteryMonitorWindowController.swift` `/` 跨工具链二义显式化（CI Xcode16 实锤、本地新 SDK 恰好可解）；② WinSparkle 下载源 vslavnik→vslavik（旧 fork 已 404）+ SHA256 钉死 + zip 顶层目录布局自适应；③ `settings_dialog`/`win32_app` 补 `VOICESTICK_LOCAL_ASR/REFINE_ENABLED` 守卫（ASR-OFF/REFINE-OFF 瘦构建从未被编译过，链接即失败）；④ CI 注入 sherpa-onnx 官方包（缓存 + SHA256），本地 ASR 保持默认 ON、LocalAsrClient 测试全量覆盖；⑤ **WasapiMicCapture COM 释放顺序修复**（ComPtr 在 `CoUninitialize` 之后析构属 UB——无麦克风失败路径 CI 首次执行即 ctest SEGFAULT，经进度标记 + SEH 探针二分定位） | CI run `37634784237`：macOS/Windows(build+ctest)/firmware/website/release-guard/script-tests 六 job 全部 ✓ |
