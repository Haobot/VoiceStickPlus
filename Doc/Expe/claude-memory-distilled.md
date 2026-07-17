# Claude Code 记忆蒸馏：Voice Stick 长期参考

- 来源：Claude Code 在本仓库（`C--Dev-FFE-George-voicestick`）约 80 条项目记忆的蒸馏，时间跨度 2026-06 至 2026-07-17。
- 蒸馏日期：2026-07-17。
- 用法：按需查阅，不必全读。排查问题先按章节定位相关条目；条目中的寄存器值、阈值、毫秒数、文件:行号、命令行均为当时的实测结论，改动前先核对代码是否已漂移。
- 约定：仓库内文档引用均为相对路径（如 `Doc/Plan/xxx.md`）；桌面端日志指 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`（**不是** Roaming 的 `%APPDATA%`）。

---

## 1. 固件 / 音频链路

### 1.1 ES8311 增益与 ALC（当前生效配置）

近场（<15cm）ASR 变差的根因是 **PGA 过高致 ADC 硬削波**（非 SNR 问题——固定增益同倍放大信号与噪声，不改变 SNR）。解法：降 PGA + 启用 ES8311 硬件 ADC ALC。嘴距 5cm vs 15cm 声压差约 +19dB。

**位域权威源是 Linux 主线 `sound/soc/codecs/es8311.h`**（raw.githubusercontent 拉取；cgit 返回 418），不能信 `managed_components` 里 `es8311_reg.h` 的用途注释——曾按注释臆测位域写反（写 `0x23`，bit7=0），致 ALC 两年未生效而编译日志全绿。`esp_codec_dev_write_reg` 写任意值都返回 OK，此类 bug 只有行为（电平不自适应）能暴露。

正确位域与当前生效值（`audio_pipeline.c` init_codec，2026-07-10 后）：

- PGA = 24dB（36→24→18→24 演进；18 是 ALC 未生效期降的，ALC 生效后回升；PGA 是 3dB 离散步进，`set_in_gain` 传小数会被截到最近档）。
- REG18(0x18) = `0x83`：bit[7]=ALC_EN=1，bit[6]=AUTOMUTE_EN=0，bit[3:0]=winsize=3（短响应）。
- REG19(0x19) = `0xC0`：bit[7:4]=maxlevel=12（≈-7.8dBFS，目标峰值≈13600；原 8≈-11dBFS 实测微信模式解码峰值仅几百~5000 故上调），bit[3:0]=minlevel=0（-30dBFS）。
- REG1A(0x1A) = 0x00（automute off，避免误判停顿为静音）。
- REG1B/1C **不写**：保留 es8311_open 默认（0x1B=0x0A、0x1C=0x6A），其中 HPF 在 REG1C(ADC8) bit[5]（`ES8311_ADC8_HPF_SHIFT=5`），不写即不破坏。

调参方向：呼吸感(pumping)→调大 winsize 或降 maxlevel；底噪放大→调高 minlevel；响度不足→调高 maxlevel（越接近 0dBFS 越易削波）。内核 `es8311_level_tlv` 等 TLV 声明是 dB 映射的权威依据。写寄存器用公开 API `esp_codec_dev_write_reg/read_reg`（esp_codec_dev.h 第 65/75 行），在 open+set_in_gain 之后追加即可。详见 `Doc/Plan/es8311-alc-bitfield-fix.md`、`Doc/Plan/audio-gain-tuning.md`、`Doc/Plan/wechat-audio-level-fix.md`。

### 1.2 I2S 通道生命周期（esp_codec_dev 所有权）

录音启停报 `i2s_channel_disable: the channel has not been enabled yet` 的根因（修复 `be1bce2`）：`esp_codec_dev_open/close` 已全权管理 rx/tx 通道 enable/disable（open 时 reconfig 流程 disable→init_std→enable，close 时 disable），应用层再手动管必有一处重复。两个触发点相互制约，单改一边只挪报错位置：

- `init_i2s` **必须保留** `i2s_channel_enable`（否则 codec open 的 reconfig disable 未使能通道，报错挪到开始时）。
- `deinit_i2s` **只 del 不 disable**（close 已 disable；close 后通道 READY 可直接 `i2s_del_channel`）。

该 ERROR 由驱动层 `ESP_GOTO_ON_FALSE` 打出，应用层返回码压不住。验证靠串口日志 `grep -c "not been enabled"`。

### 1.3 软件高通去爆破音（v1.9.x）

`audio_pipeline.c` 在 PCM 抽单声道后、Opus 编码前加二阶 Butterworth HPF（transposed direct form II），抑制 20–100Hz 呼吸气流爆破音；硬件 ES8311 HPF 截止太低（去 DC 级）对爆破音无效。去掉 100Hz 以下气流噪声后 ASR 字错率也明显改善。

- 参数：fc≈90Hz @16kHz，Q=0.707；b0=0.975318 b1=-1.950637 b2=0.975318 a1=-1.950028 a2=0.951246。
- 频响：90Hz -3dB / 50Hz -10.6dB / 30Hz -19dB / 150Hz -0.5dB / 300Hz+ ~0dB。
- 调参：男声偏闷调到 100–120Hz；爆破残留调到 70–80Hz。状态每会话 start 清零；**audio_task 与 drain 路径都要过滤**；`hpf_process` 用 lround 取整+软限幅防漂移。

### 1.4 尾音丢失：button_up 抢跑 drain（已根治）

"说完即松"丢最后 1–2 字，根因两层缺一不可：

1. `audio_task` 的 `while (atomic_load(&s_running))` 在 `s_running=false` 后立即退出，不读 I2S DMA 缓冲（4 描述符×120 帧 ≈ 60ms）里残留的尾音 PCM。
2. `audio_pipeline_stop` 发 sentinel 后立即返回，`stop_recording` 随即发 `button_up`（走 state_tx），drain 帧/audio_end 走 audio_tx，**BLE 不保证跨特征顺序**，button_up 先到桌面端结束会话，后到的 drain 帧被丢弃。

修复：audio_task 退出 while 后加 drain 段（读 2 帧 ≈80ms 覆盖 DMA 残留）；`audio_pipeline_stop` 同步 `wait_for_tasks_to_exit` 等 audio_task+tx_task 退出再返回（超时兜底 `TASK_EXIT_WAIT_MS=800ms`），保证 button_up 在所有 audio notify 之后提交。tx_task 遇 sentinel 用 `goto drain` 排空队列再发 audio_end，不能直接 break。

通用准则：任何"按住开始/松开结束"的音频会话，`*_stop` 必须同步等 drain 完成再返回。排查尾音问题分清两层：日志看到 drain 帧入队只证明固件侧 (1)，体感才能确认桌面端 (2)——**drain 帧入队 ≠ 桌面端收到**。方案：`Doc/Plan/audio-trailing-syllable-drain.md`。

### 1.5 BLE 音频拥堵根治（2026-06-30 验收）

`voice_ble: tx seq=N mbuf alloc failed` 刷屏致识别断续的根治组合（**勿回退**）：

- **禁 Wi-Fi**：内部 RAM 从 ~4KB 涨到 ~112KB，消除 2.4GHz 共存干扰（v1.8.0 起 voice_net 已物理删除，见 §2.1）。
- **fast conn interval 固定 7.5ms**：`voice_ble_request_fast_interval` 改 `itvl_min=itvl_max=6, min_ce_len=max_ce_len=8`；`start_recording` 开头（button_down 后）立即请求，与 audio 初始化并行。
- **MSYS1 mbuf 池 100→200 块**（`CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT`，sdkconfig+sdkconfig.defaults，多占约 25KB 内部 RAM）：100 块长录音瞬时耗尽会复现 alloc failed，**池大小是关键因素**（曾误判"与池大小无关"，当天下午长录音复现推翻）。

配套：tx 重试 `TX_RETRY_DELAY_MS=10`/`TX_MAX_RETRIES=30`（audio_pipeline.c）；alloc failed 告警节流（`s_mbuf_fail_streak` 每 10 次一条）。2M PHY 请求保留但 Windows central 拒绝回退 1M（IDF v5.5.1 NimBLE 只有 `ble_gap_set_prefered_le_phy`）。OTA rollback 由 `app_main` 直接 `esp_ota_mark_app_valid_cancel_rollback()`。

### 1.6 PSRAM 栈与 flash 操作

- 大栈任务（audio_task 32KB 等）用 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` + `vTaskDeleteWithCaps(NULL)`，需 `#include "freertos/idf_additions.h"` 与 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`。内部 RAM 不足时普通 xTaskCreate 报 `ESP_ERR_NO_MEM`（曾表现为按录音键屏幕报 `Audio audio_task: ESP_ERR_NO_MEM`）。audio_task 栈 24KB 在加 LVGL 后会溢出，须 32768。
- **PSRAM 栈任务里不能做 flash 操作**（L3 回放钩子坑）：audio_task 里 fread SPIFFS 触发 `assert failed: esp_task_stack_is_sane_cache_disabled`（cache 禁用期 PSRAM 栈不可访问）。解法：在内部 RAM 栈任务（main）预读整个文件到 PSRAM buffer（`heap_caps_malloc MALLOC_CAP_SPIRAM`），PSRAM 栈任务仅 memcpy。串口抓到 assert+Rebooting 是铁证。
- 诊断内存：`heap_caps_get_free_size/largest_free_block(MALLOC_CAP_INTERNAL)`、`uxTaskGetStackHighWaterMark`。

### 1.7 深睡 / 电源管理

- **USB 供电守护已正确，勿误改**：三条关机路径都有 `is_external_powered()`（`s_battery_charging || s_usb_powered`）检查——`restart_poweroff_timer`（main.c:347）、`enter_power_off`（main.c:400，缓存+fresh PMIC 读双重检查）、断连关机 `start_disc_poweroff_timer`/`disc_poweroff_timer_cb`（main.c:1538/1518）。USB 检测基于 `stick_s3_board_usb_powered` → `vbus_voltage_mv > 4500`。实测 USB 插着 6+ 分钟无 poweroff check 触发。再遇"USB 插着却关机"，先查 `power source changed` 日志是否真 usb=1，多半实为 app 端 BLE 问题。
- **省电参数**（commit c812d57）：`DISPLAY_ACTIVE_BRIGHTNESS` 32→20、`DISPLAY_DIM_BRIGHTNESS` 8→4、`DISPLAY_DIM_TIMEOUT_MS` 30s→10s、`DISPLAY_OFF_TIMEOUT_MS` 60s→20s、`POWEROFF_TIMEOUT_MS` 10min→5min；`DISC_POWEROFF_TIMEOUT_MS` 10min 不变。S2 熄屏态"拿起亮屏"已验证可用（S1→S2 只关背光不停拿起轮询，`APP_EVENT_PICKUP` 在 `s_display_dimmed` 时唤醒）。设计：`Doc/Plan/display-brightness-battery-optimization.md`、低功耗见 `Doc/Plan/固件待机省电策略.md`。

### 1.8 FreeRTOS 回调栈陷阱（通用）

Wi-Fi 链路 API（`esp_wifi_set_config/connect/disconnect`）单次调用需 3–4KB 局部栈，`Tmr Svc`（`CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048`）和 `sys_evt`（`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304`）都不够，在 timer/event 回调里直接调会栈溢出 panic（backtrace 常 CORRUPTED，**看任务名比 backtrace 重要**）。正确做法：专用 worker task（栈 ≥6KB）+ timer 回调只 `xQueueSend` 投递命令。同理，timer/event 上下文里调 esp_http_client/mbedtls 等大栈 API 前先问"这个上下文栈够吗"。（Wi-Fi 代码虽已删除，此教训对任何大栈 API 仍适用。）

---

## 2. 固件烧录与串口日志

### 2.1 固件升级路径（v1.8.0 起）

Wi-Fi STA 配网与 LAN HTTP OTA 已**物理删除**（voice_net 组件、Windows 端 `wifi_settings_dialog`/`wifi_credentials_win`/`ota_command`/`voice_stick_ctl` 与 `VoiceStickCtl` 目标全删）。不要再引用 `voice_net`、`wifi_set`/`ota_pull` 帧、`VoiceStickCtl`、`probe_wifi_provisioning.py`。当前四条路：

1. **BLE OTA 远程**：托盘菜单"更新固件"（OSS manifest 下载推送）。
2. **BLE OTA 本地文件 GUI**（v1.9.x，`ce2fd6c`）：托盘设备子菜单"从本地文件更新固件..."，`coordinator.UpdateFirmwareFromFile`。
3. **BLE OTA 本地文件命令行**（v1.9.x，`8ac7258`+`064b49c`）：`VoiceStick.exe --ota <bin> [--device <id>]`。已运行实例用 `FindWindowW("VoiceStickWindow")`+`WM_COPYDATA` 转发；设备已连接时一行命令全程无人工介入（自动化首选，真机已验证）。前提：app 运行且日志出现 `stage=ready`。
4. **USB COM 烧录**：`python scripts/idf_cli.py -u`。

本地 bin 在 `firmware/build/voice_stick.bin`；验证靠设备串口 `OTA complete, rebooting`。`ota_commit` BLE 命令保留（boot 已自动签到，双槽分区表与 `CONFIG_APP_ROLLBACK_ENABLE=y` 不变）。烧录 skill（`.agents/skills/sticks3-flash-ota/`，另有 `.claude/skills/`、`skills/` 两处同步副本，改时三处同步）2026-07-09 已重写为上述流程，旧 VoiceStickCtl/HTTP OTA 描述已清除。

### 2.2 Stick S3 Boot 按键时序

前面板按钮：短按=重启、双击=关机、长按=进下载模式。

- **进 Boot（烧录前）可自动**：设备正常运行且 USB JTAG 可达时，esptool `default_reset` 软复位直接进下载模式，**不需长按**；深睡/卡死/首次/状态不明时才需手动长按。
- **出 Boot（烧录后）必须手动**：`--after hard_reset` 在本板无效（reset 线被按钮电路接管），烧完必须短按重启，否则留在 `waiting for download`（串口能开但芯片不跑）。不要再用 esptool `run` 子命令尝试自动复位。"烧录成功但串口无响应"先检查是否短按重启。详见 `.agents/skills/usb-jtag-flash-log/SKILL.md`。

### 2.3 USB JTAG 运行时日志坑

ESP32-S3 USB JTAG 控制台（VID 303A:1001）的运行时日志抓取不可靠：

- **烧录报成功 ≠ app 在跑**（见 §2.2）。
- **DTR 软复位抓 boot 日志**：`s.dtr=False; sleep(0.1); s.dtr=True; sleep(0.3); s.dtr=False`，稳定抓 ROM bootloader+app_main；但会触发二次复位覆盖后续日志。
- **运行时事件日志常读 0 字节**：Core1/PSRAM 栈任务（audio_task）的 ESP_LOG 尤其不稳定，Core0 主循环日志相对好抓。诊断优先让日志在 Core0 打，或加心跳日志验证通道存活。`idf_cli.py monitor` 同样不稳（ClearCommError 报错），pyserial 直读+DTR 复位更可靠。
- **关键数据绕行 USB 日志**：事件发生时写 NVS，下次连接后通过 BLE state_tx 上报（whisper_pen 用此法抓断连 reason=8：`ble_diag`/`dc_reason` → `{"event":"last_disconnect","reason":N}`），比反复重试 USB 日志可靠。
- **区分采样日志 vs 全量日志**：Windows 日志 `audio frame slow` 只在处理 >1000us 时打印（voice_stick_coordinator.cc:1165），seq 跳跃是快帧没打日志**非丢帧**——whisper_pen 曾因此误判"70% 帧丢失"。判断丢帧前先确认日志是否采样。
- 独立工程必须显式配 USB 控制台：`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` + `UART_NUM=-1`（不能用 `ESP_CONSOLE_UART_NONE`，与 USB_SERIAL_JTAG 互斥致 NONE 生效），否则 USB 串口读不到任何日志，易误判设备没启动。

### 2.4 pyserial 采集坑

- Python `open('/tmp/x.log')` 在 Windows 写到 `C:\tmp\x.log`（git bash 的 /tmp 映射只对 shell 生效），显式用 `C:/tmp/x.log`。
- 烧录/重启瞬间 USB 重新枚举，pyserial 旧句柄 read 静默失效采到 0 行——**必须等设备重启完成再开采集**；采集任务 exit 0 不代表采到数据，要 `wc -l`/`grep -c` 核实，勿凭"用户说正常+Monitor 没报"下结论。
- SPIFFS 测试镜像：`build_spiffs_image.py` 调 spiffsgen.py 打包，esptool write_flash 到 storage 分区（0x610000, 0x1f0000）。

---

## 3. Windows 桌面端

### 3.1 构建假成功与管理员进程残留（最高频坑）

- `build_win.bat` 在 `cmake --build` 经 `vs_link_exe` 包装时，链接失败（如 LNK1104 文件占用）的退出码可能丢失，脚本仍输出 `Build SUCCEEDED`。曾因轻信字样让用户拿旧 exe 白测一轮。**每次构建后必须**：①核对 `desktop/windows/build-x64/VoiceStick.exe` 的 LastWriteTime 是本次构建时间；②读 `build_compile.log` 末尾确认无 FAILED 行；③不信 SUCCEEDED 字样。
- VoiceStick.exe 清单曾要求 requireAdministrator，**管理员进程普通 taskkill 杀不掉**（Access denied 静默继续），build-x64 删不干净，ninja 复用旧 obj 链出旧 exe。处理：提权 `Start-Process taskkill.exe -ArgumentList '/F','/IM','VoiceStick.exe' -Verb RunAs -Wait` → `rm -rf desktop/windows/build-x64` → 重建，并核对 exe 时间戳/体积变化。
- 本地开发授权约定：构建前可自动关闭运行中的本地 `VoiceStick.exe`，构建+测试成功后自动启动新 exe 供手动验证（仅限本地 build-x64 路径）。Windows 改动测试通过后默认打包 MSI 并中文 Conventional Commits 提交，失败状态不提交。

### 3.2 git add -f 与签名 MSI

- `.gitignore` 整体忽略 `desktop/windows/`：已追踪文件照常 modified，但**新建 .h/.cc 被静默忽略**，必须 `git add -f <新文件>`，否则提交后他人构建缺文件。
- 本机 `Cert:\CurrentUser\My` 有自签证书 `CN=VoiceStick Dev`（Thumbprint `5416B0067FAEF56F52EB615503A484350D04D4FC`，有效期至 2027/6/30），`scripts\build-msi.bat` 自动选中它走通「构建+签名 exe/dll+WiX+签名 MSI」，**无需签名机**（旧记录"本机无证书"已过时）。

### 3.3 WinSparkle file URL 坑

`build_win.bat` 删 build-x64 触发 FetchContent 重新下载 WinSparkle 时，CMake 3.31（VS2022 自带）对 `file:///C:/...`（三斜杠）解析成 `/C:/...` 报 File not found；**必须用两斜杠** `file://C:/Softwares/CMake-Tool/WinSparkle-0.9.2.zip`（裸路径也可）。进程级 `$env:VOICESTICK_WINSPARKLE_URL` 可能缓存旧值，调 build_win.bat 前在当前进程显式重设。

### 3.4 便携包打包

- `package-portable.bat` 的中文 `(echo ...)` 多行重定向块在 cmd GBK 代码页下解析错位（echo 内容被当命令执行）；已改 PowerShell 复刻 `scripts/package-portable.ps1`（WriteAllText 生成 config.toml/README + Compress-Archive）。**含中文的 .ps1 必须存 UTF-8 with BOM**，否则 PowerShell 5.1 按 GBK 读破坏 here-string；Write 工具默认无 BOM，需 `[System.IO.File]::WriteAllText($path, $content, [Text.UTF8Encoding]::new($true))` 转码，`[Parser]::ParseFile` 预检语法。
- 删文件被护栏误拦时改用 `[System.IO.File]::Delete()`。PowerShell 命令里 `Remove-Item` 与 `"C:\Program Files"` 字面量同现会被路径护栏误判拦截——用 `Join-Path $env:ProgramFiles` 构造或拆命令。
- 便携包只含 `VoiceStick.exe`+`WinSparkle.dll`（VoiceStickCtl 已删）；模板 `asr_provider` 用占位符，**绝不写真实 key**；产物 `dist/VoiceStick_Portable_v<ver>.zip`+目录版。便携模式：exe 同级有 config.toml 即激活，数据存程序目录，禁自启和自动更新。
- 打包前 exe 须最新构建：ninja 增量可能不重链接，须删 build-x64 全量重建并核对时间戳（同 §3.1）。

### 3.5 MSI config 模板首启复制（v1.9.0）

MSI 装 `config.template.toml` 到 `Program Files\VoiceStick\`，首启 `AppConfig::Load()` 检测 `%APPDATA%\VoiceStick\config.toml` 不存在时从 exe 同级复制（`SeedConfigFromTemplate`，app_config.cc，`13b2720`）。三个陷阱：

1. **模板不能叫 `config.toml`**——`AppConfig::IsPortableMode()` 检测 exe 同级 config.toml 激活便携模式，asInvoker 进程无 Program Files 写权限致配置存不下来。
2. **不能直接装到 `%APPDATA%`**——perMachine MSI 的 AppDataFolder 只解析到安装者账户。
3. **升级不覆盖**——目标存在即跳过；强制刷新让测试同事删 `%APPDATA%` config 重启。

分发：`set VOICESTICK_CONFIG_TEMPLATE=<真实config路径>` 后跑 build-msi.bat（密钥本机注入不进 git；含密钥 MSI 切勿上传公开 Release）。方案 `Doc/Plan/windows-msi-config-template-seed.md`。

**此类改动必须真机端到端验证**：单测只覆盖 `SeedConfigFromTemplate` 纯函数，WiX 落地/首启触发/升级不覆盖都测不到。流程：备份真实 config（记 SHA256）→装 MSI→核对模板落地+exe 时间戳→删 config 启动→**SHA256 核对与模板字节一致**→改配置重启→SHA256 核对不覆盖→还原备份→卸载清理。

### 3.6 Win32 对话框声明式布局范式

改手动 `CreateWindowExW` 定位的对话框（参考 `settings_dialog.cc`），**一开始就用声明式布局表**，勿用"手动累加 y+空 label 占位+ShowWindow 隐藏占位"（条件行隐藏留白，支持动态高度/滚动需推倒重来）：

- 模型：`std::vector<LayoutEntry> layout_`，每行注册 `{advance, parts[], visible}`（visible 为可见性谓词）；`BuildControls` 只创建+注册（位置传占位 0,0），定位全交 `Relayout()`——可见项 SetWindowPos 并推进 y，不可见项 SW_HIDE 不推进。
- 坑：`SS_ETCHEDHORZ` 分隔线不进 `label_controls_`（WM_CTLCOLORSTATIC 透明背景会弄坏它）；行内条件按钮用 `defer_visibility=true` 交给专门 Apply 函数；滚动用全量 Relayout 而非 ScrollWindowEx（STATIC 透明背景易残留）；按钮钉底公式 `btn_y = client_h - btn_h - margin` 统一两种情形；`SIF_DISABLENOSCROLL` 让滚动条禁用态仍占位防客户区宽度跳变；窗口高上限 `min(内容+按钮区, 工作区高度-Dp(40))`，`scroll_pos_` 在 Relayout 内 clamp。
- 动态高度改造后必预想"全展开+高 DPI 超高"，提前加 WS_VSCROLL。加粗字体用 `dpi_util.h` 的 `CreateUiFontBold(dpi)`。

### 3.7 Win32 杂项坑

- **trackbar 类名**：`WC_TRACKBARW` 宏可能报 C2065 未声明（TBS_*/TBM_* 数字宏不受影响），直接用字符串 `L"msctls_trackbar32"`（air_mouse_tuning_window.cc 用此法）。
- **wWinMain 命令行**：其 `command_line` 参数**不含程序名**，`CommandLineToArgvW` 后 argv[0]=第一个参数，按 argv[1] 起扫的解析全部错位（`--ota` 曾因此失效，`064b49c` 修复）。一律用 `GetCommandLineW()`+`CommandLineToArgvW`。
- **提权重启单例竞态**：`ShellExecuteW(runas, "--relaunch")`+旧实例立即退出时，新实例 `CreateMutexW` 拿到 ERROR_ALREADY_EXISTS 被单例（`Local\TenClass.VoiceStick.SingleInstance`）赶走，**新旧实例都没了**。修复：`--relaunch` 实例重试等 Mutex 释放（5 秒×100ms）。ShellExecuteW 返回 >32 只表示请求已提交。
- **UIPI 与微信高权限窗口**：Medium IL 的 VoiceStick 的 SendInput 被静默丢弃，无法注入 High IL 的微信 4.0 窗口（OpenProcess(QUERY_INFORMATION) 都 ACCESS_DENIED）。**认知盲点纠正**：微信输入法模式的 `SendDown`（注入 Ctrl+Win）同样是合成输入，同样受 UIPI 限制——manifest/文档旧称"该模式不受影响"是错的。已实现自动检测高权限前台+气泡提醒+托盘"以管理员身份重启"（`Doc/Plan/windows-elevation-hint.md`）；manifest 维持 asInvoker，HKCU\Run 自启拉起的仍是 Medium 实例。清单嵌入坑：vs_link_exe 拦截 `/MANIFEST*` 标志，须用 POST_BUILD `mt.exe -manifest xxx -outputresource:exe;#1`，.rc 不要再声明 RT_MANIFEST。
- **app 日志位置**：`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`（**非 Roaming**），`LogBleLine`/`LogApp` 写入，含 `scan started`→`connect stage`→`connected`→`stage=ready` 全链路。曾因查 Roaming 找不到日志误判"app 不工作"绕进深睡排查，实际 app 连接完全正常（真问题是 --ota 解析 bug）。

### 3.8 流式精修渲染（2026-06-30）

精修开启后悬浮窗卡死闪动的根因与修复（`2c7a85b`，均在 overlay_window.cc）：

- `OnTimer` 在 kListening 每 16ms（kAnimationStepMs）无条件 `InvalidateStaticLayer`，每 16ms 全量重建 D2D CreateTextLayout 致 UI 线程过载 → 卡死。修复：静态文本不重建 static layer，仅重绘动态指示器（音浪条）。
- 流式 token 每 ~60ms 到达重置 140ms（kTextTransitionMs）滚动动画 → 闪动。修复：`AppendPartial` 跳过文字滚动过渡（`Show` 加 `skip_text_transition`）。

**精修耗时 2.5~12.8s 随文本线性增长，LLM 固有延迟不可压缩，勿再探索压缩总时间**（definite 分段并行：腾讯云无 definite、长句无停顿退化、final_text≠utterances 拼接不同源；倒计时并行：装不进 1.2s 窗口）。优化重心是流式逐字显示。RFC：`Doc/Plan/overlay-render-streaming-refine.md`。流式精修仅 Windows 端实施，macOS 端待推进（macOS 目前连非流式精修都没有）。

### 3.9 ASR 协议与配置坑

- **腾讯云 slice_type=2 是单句稳态（VAD 切句），不是整段结束**；整段结束看顶层 `final=1`（服务端随后断连）。曾误用 slice_type=2 立即 on_final 致说话停顿提前截断（`ba22232` 修复：单句累积到 `accumulated_final_text_`，final=1 才 `EmitFinalText()`，close frame 兜底补发）。`needvad` 代码强制 1 保留分句。腾讯 VAD 的另一面：长句停顿切句后不续识别（切火山 ASR 同固件长句正常，证明非固件问题）。协议见 `Doc/Guide/实时语音识别（WebSocket）.md`。
- **腾讯 4002 "密钥不存在"勿轻信表面提示**：曾实为设置对话框 provider 切换把 `tencent_secret_id` 错映射到 `volcengine_api_key` 字段。先定位数据流（代码读哪个字段、值是什么，日志 `TencentAsr::Start called secret_id=...`），再定位数据内容；provider 切换必须显式 switch/case 映射，不能 `idx==0?A:B`；加载时加安全迁移（`volcengine_api_key` 形似 `AKID...` 且 tencent 字段空则回迁落盘）。

---

## 4. 微信输入法模式（wechat_input_method）

### 4.1 链路闭环与热键

- 微信输入法 Windows 版（2.1.0+）/微信电脑版 4.1.8+：长按 **Ctrl+Win** 按住说话松开发送；Ctrl+Win+Shift 持续模式。VoiceStick 默认热键 `ctrl+win` 对应 hold_to_talk（SendDown/SendUp）。
- 闭环：微信输入法监听**系统默认录音设备**；VoiceStick 用 WASAPI 渲染到虚拟麦**播放端 CABLE Input**；用户须把系统默认录音设备设为 **CABLE Output**。CABLE Input=eRender 不能设为默认播放设备，CABLE Output=eCapture 必须设为默认录音设备。

### 4.2 SendInput 必须带 scan code

注入第三方输入法/热键工具时，`INPUT.ki.wScan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)` 必须填，仅 wVk 时 SendInput 返回成功但目标不响应（部分输入法用 WH_KEYBOARD_LL 靠 `KBDLLHOOKSTRUCT.scanCode` 识别，scan code 为 0 不认）。右 ALT（VK_RMENU）/右 Ctrl 等扩展键还要 `KEYEVENTF_EXTENDEDKEY`。诊断"返回 OK 但无反应"：物理按键能触发、合成不能 → scan code 或 LLKHF_INJECTED。实现在 `wechat_input_method_hotkey.cc` 的 `BuildKeyboardInput`。

### 4.3 按下到弹框延迟优化（hold_to_talk_instant）

延迟分布：固件段 380~780ms（300ms hold 阈值最大 + audio init + slow interval 传输）+ 桌面段几十 ms。四项优化（2026-07-06）：

1. `handle_primary_down` 按下即 `voice_ble_request_fast_interval()`（不等 300ms 后），conn update 在阈值等待期并行完成。
2. 固件新增 `interaction_mode="hold_to_talk_instant"`：按下即 `start_recording`+`button_down`，跳过 300ms 阈值；button_up 短按仍进双击窗口（双击 Enter 保留）。**桌面端用派生枚举 `kHoldToTalkInstant`（不暴露配置/UI）**，`InteractionModeToSend()` 在 wechat+hold_to_talk 时返回 instant，`config_.interaction_mode` 不变；旧固件不识别则安全退化 300ms。
3. `StartWechatInputMethodSession` 先 SendDown 再 renderer.Start（renderer.Start 失败补 SendUp）。
4. 全删提示音（固件+Windows 双侧，macOS 无此功能）。

坑：`voice_ble.c` device_info 的 `char json[280]` 加字段后超 280 触发 -Werror snprintf 截断，扩到 320；kconfgen 在中文 Windows 用 GBK 读 sdkconfig.defaults，构建设 `PYTHONUTF8=1`+`PYTHONIOENCODING=utf-8`（既有环境问题）。方案 `Doc/Plan/wechat-press-to-popup-latency-optimization.md`。

### 4.4 首帧就绪再 SendDown 与首字延迟根因

- **SendDown 推迟到首帧 PCM 入 ring_buffer 后**（commit 9fae99b）：原 SendDown 在 renderer.Start 前，微信弹框即取音但 ring 空读到静音致首字卡顿。现 renderer.Start 提前到 SendDown 前，SendDown 在 `HandleWechatInputMethodAudioFrame` 首帧解码成功后持锁发出；新增 `wechat_hotkey_sent_down_`，Stop 仅在 sent_down_ 时配对 SendUp。代价：弹框延迟约 100~200ms 但弹框即有有效音频。
- **首字延迟根因已量化**：桌面端 button_down→SendDown 总计仅 74~296ms（auto_switch 12-18 / renderer.Start 22-37 / 首帧 3ms 热 192-249ms 冷 / SendDown 34-49，grep `wechat latency`），**1~2 秒主因在下游 VB-CABLE 缓冲+微信 ASR 黑盒**（不可控，管道滞留上限 ~562ms 不足解释）。固件 init_codec 32ms 热/60ms 冷，决策不改。桌面端 buffer_duration_ms 50→20 省 30ms（e6bd975，若抖动丢字可回退 30/50）。桌面端优化空间已尽，勿再投入。测试设施：OggOpusDemuxer、TimedFakeSink（`Doc/Plan/wechat-latency-test-framework.md`）、`Doc/Guide/vbcable-latency-measurement.md`。

### 4.5 点按式（click_to_talk）语义与 session_id 竞态

- 长按式（微信输入法）：启动 SendDown、停止 SendUp；点按式（Typeless 等）：启动/停止都发 **SendClick**（完整 down+up，仅按下不松开不弹框）。首帧 Opus 解码成功后才发热键。
- **停止竞态**：固件 click_to_talk 停止时 audio_end（audio_tx）先于 button_click（state_tx）到达，end_of_stream 分支先停会话，随后到达的"停止"click 被误判为启动新会话（又弹一次）。修复：记 `last_stopped_wechat_session_id_`+时间戳，`HandleButtonClick` 用 `IsStaleWechatStopClick`（session_id 匹配+2 秒窗口）忽略迟到的停止 click。固件启动与停止带相同 session_id。
- **残留 active 自愈**（2026-07-16）：`HandleButtonClick:824` 停止判定加 session_id 匹配——`event.session_id` ≠ `active_session_id_` 时走启动分支（先 Stop 旧发 SendClick OFF 再 Start 新），避免停止 click+audio_end 都丢时新会话被错位吞掉。`button_double_click` 不带 session_id 视作匹配走停止+Enter。
- **双击回车**：固件 click_to_talk 物理首击延迟 300ms（`CLICK_TO_TALK_START_DELAY_MS`）等双击窗口：窗口内第二击发 `button_double_click`（不启动录音，桌面端 SendEnter）；超时确认启动。代价：单击启动延迟 300ms；远程热键不延迟。
- 方案：`Doc/Plan/wechat-click-to-talk-hotkey-fix.md`、`wechat-click-to-talk-session-id-reconciliation.md`、`wechat-click-to-talk-double-click-enter.md`。

### 4.6 断连清理与卡 listening 兜底

- **新增输出模式必须检查四处清理分支**：`CancelRecognitionInProgress`、`CancelActiveCycleIfDeviceDisconnected`（BLE 断连）、`Shutdown`、`HandleWechatInputMethodAudioFrame` 的 IsEnd/audio_end 分支。wechat 模式落地时漏了断连分支，`wechat_input_method_active_` 残留 true 致"松开还在 Recording"。
- **button_up 走 BLE notify 无 ACK，闪断会丢**：丢失后残留 active，下次 button_down 被 `HandleWechatInputMethodPrimaryButtonDown:392` 的 `if (IsWechatInputMethodActive()) return` 吞掉 → 长按完全无反应。自愈：(1) audio_end 帧到达即完整结束会话；(2) button_down 时若 active 残留先 Stop 再 Start（安全前提：hold_to_talk 录音中再按主键固件不发新 button_down）。**注意锁**：audio_end 路径须释放 `audio_mutex_` 后才调 `StopWechatInputMethodSession`（它内部也取该锁，持锁调死锁），范式"锁内标记+锁外 Stop"。
- **录音硬超时兜底**（commit 2da4c27）：button_down 进 recording 时 `ScheduleRecordingHardTimeout`（默认 120s，测试注入 300ms），超时回调锁内校验 generation 后按模式分发停止路径（wechat 调 StopWechat+EnterReady，focused_app 调 CancelShortRecording），EnterReady/EnterFinalizing/StopWechat 开头 Cancel。新增任何 recording 路径都要挂这个兜底。`debug_audio_recorder_.Finish()` 重复调用幂等。
- **共享事件处理器提前 return 吞通用动作**：`HandleButtonDoubleClick` 开头曾为 wechat 加提前 return，吞掉后面的 `SendEnter()`（wechat 模式双击永不发 Enter）。修复：并入通用路径（活跃录音先 StopWechat 让文字进输入框，再统一 SendEnter）。**新增模式时 grep 所有按 `config_.default_output_profile.target` 分发的处理器逐个确认**，每个模式都要有"双击发 Enter"测试。

### 4.7 WASAPI 坑

- **shared mode 格式**：渲染 16kHz PCM 到 48kHz 设备（VB-CABLE mix format）必须加 `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`，否则 `IAudioClient::Initialize` 拒（AUDCLNT_E_UNSUPPORTED_FORMAT）。错误消息 `"Virtual microphone not found"`（voice_stick_coordinator.cc:467）不区分 OpenDevice 与 InitializeStream 两种失败，曾误导诊断——`Get-PnpDevice -Class AudioEndpoint` 确认设备 OK 后才定位到格式。教训：底层模块加 HRESULT 日志，"not found" 类消息要区分失败点。
- **renderer 复用不变量**：`wechat_renderer_` 在 session 间复用同一对象，**`Stop` 只 `sink_->Stop()`，绝不 `sink_.reset()`**——曾 reset 致第二次 Start 解引用 nullptr 崩溃（WER APPCRASH，0xc0000005）。`OpenAndInitialize` 开头已初始化先 `Cleanup()` 再重建。测试必须覆盖 Start→Stop→Start（`TestWasapiRendererRestartsAfterStop`）。

### 4.8 低电平问题（采集端根因）

"CABLE Input 电平仅 1-5%、微信无识别，但 ASR 模式正常"的根因**在固件采集端不在播放端**：peak 诊断日志显示 Opus 解码器输出 PCM 峰值本身就低（几百~5000，正常应上万），ring/WASAPI 传输零衰减。**ASR 正常 ≠ PCM 幅度正常**——腾讯/火山服务端 AGC 能拉起低电平，CABLE 直通链路无 AGC 原样暴露。修复见 §1.1（PGA 18→24、maxlevel 8→12）。诊断方法：从解码器输出端开始逐层加 int16 peak 日志往 WASAPI 比对；落盘 ogg 用 Python 解析 OpusHead gain+packet 字节统计是交叉验证金标准；临时诊断日志定位后必须清理。

### 4.9 自动切换默认录音设备（v1.9.x 已完成）

`auto_switch_default_recording_device`：录音期把默认录音设备切到 `virtual_mic_capture_name`（CABLE Output），Stop 在 renderer.Stop(drain) 后切回。**角色分离恒只切 eConsole，eCommunications 保持真实麦不动**（Teams 零干扰；核心假设"微信从 eConsole 取音"已实证成立，无反证勿退全角色切换）。实现：`IDefaultAudioDeviceController`+Fake 单测+真实 IPolicyConfig COM；状态持久化 `default_device_switch_state.json` 供崩溃自愈。方案 `Doc/Plan/auto-audio-device-switching.md`。

- **IPolicyConfig 未公开 COM 查证**：WebSearch/raw 不通，jsdelivr CDN（`cdn.jsdelivr.net/gh/{owner}/{repo}@{branch}/{path}`）与 GitHub API（PowerShell Invoke-WebRequest）可达。可靠源 SoundSwitch **dev 分支**（默认分支是 dev）。确认值：CLSID=`870AF99C-171D-4F9E-AF0D-E63DF40C2BC9`，IID=`F8679F50-850A-41CF-9C72-430F290290C8`；vtable 12 方法 SetDefaultEndpoint 第 11，C++ 声明须严格对应顺序。**勿凭记忆写 GUID**（曾"确信"的 CLSID 与实际完全不同）。GUID 已落在 `default_audio_device_controller.cc` 注释。
- 默认关闭须用户勾选；平时默认录音设备须是真实麦否则切回无效。

### 4.10 短录音过滤

wechat 路径曾缺最短时长过滤，落盘出极小 ogg（128 字节=Ogg 头 101+EOS 27 零帧；289 字节=头+1 个 40ms 帧；可反推：101+N*188=N 个 40ms 帧）。instant 模式任何点按都启动会话是温床，但**不能靠加阈值治**（instant 是为降弹框延迟的独立优化），应在落盘层过滤：已加 `ShouldDiscardWechatRecording()`（零帧或 <0.5s，对齐 `kMinimumRecordingDurationSeconds`）覆盖 4 个落盘点。新增录音路径时检查所有落盘点是否对齐过滤。

### 4.11 VB-CABLE 随包方案（内测限定）

Qt 迁移暂缓；优先"随包带 VB-CABLE 原始 zip+首启自动装驱动+自动切设备"（方案 `Doc/Plan/windows-vbcable-as-is-bundling.md`，AS IS SHA256 校验贯穿）。**VB-CABLE 是 VB-Audio donationware，随包携带属再分发，条款 3.4 一手原文未取得：公开发布前必须改走 B2-下载（首启从官方源拉取）或取得商业授权，禁止直接沿用携带模式公开发布。**

---

## 5. 交互 / 体感

### 5.1 主键 300ms 阈值与双击（设计非 bug）

`hold_to_talk` 下**不是按下即录音**：`handle_primary_down` 物理按下只设 `s_hold_threshold_pending` 并启动 `DOUBLE_CLICK_MAX_PRESS_MS=300ms` 定时器，到期且 GPIO11 仍低才 `start_recording()`+发 `button_down`；短按 <300ms 走 `handle_primary_up` 进 500ms 双击窗口，超时发 `button_click`（桌面端忽略），窗口内再按发 `button_double_click`（桌面端注入 Enter）。这是 commit `c5b0014` 的有意设计（`Doc/Plan/primary-button-double-click.md` 决策#2：避免第一击无效启停音频管线）。

**用户已拍板保留 300ms 阈值**（按下即录音体感突兀，阈值提供意图确认），勿默认下发 hold_to_talk_instant（wechat 的 instant 是独立优化）。"按下没反应"的根治靠兜底/自愈（§4.6 硬超时），排查真因多为：轻点 <300ms 被双击窗口吃掉、或 Windows 客户端没启动。串口关键日志：`hold threshold reached, starting recording` / `button front up during hold threshold`。

### 5.2 敲击检测（tap）

全链路：固件软件算法（ACC 脉冲+GYR 平静确认，non-legacy 不支持硬件 tap feature）→BLE `voice_ble_send_tap("double")`→Windows `HandleTapEvent`→VK_DOWN。10ms 轮询定时器；NVS 键 `tap_en`/`tap_lvl2`；配置 `tap_to_arrow`。

- **BMI270 PWR_CTRL(0x7D) 位**：bit0=AUX_EN、bit1=GYR_EN(0x02)、bit2=ACC_EN(0x04)（Bosch `bmi2_defs.h`）。曾误写 0x01（AUX_EN）致陀螺仪恒零。启用 GYR 写 0x06。
- **阈值单位**：`bmi270_read_acc_g()` 返回 **g**，阈值必须在 g 尺度（曾填 LSB 致需 ≥6144g 物理不可能）；`read_acc_raw()` 才是 LSB（±2g 下 1g≈4096 LSB）。
- **灵敏度 1~10 档桌面端滑块实时下发**（免烧录，`2b3c532`+`9822dd4`）：协议 `tap_sensitivity` 从字符串改整数 level:1..10（固件兼容 legacy low→2/medium→5/high→9）；阈值表整体下移过一次（新 1 档=0.50g/50dps，新 10 档=0.15g/90dps，档 5=0.34g/67.8dps）。方向键注入 500ms 节流（`last_tap_inject_at_`）。
- **基线漏检/误触治理**（`a3bc065`）：①按键抑制 600ms（`s_tap_suppress_until_us`，覆盖 hold 300ms+codec 初始化窗口）；②主轴集中度判据 `delta_dom/delta_sum >= 0.6`（敲击集中单轴，扰动三轴均匀）；③基线冲击期极慢 EMA(0.02) 跟随不冻结、平静期快 EMA(0.2)——曾因冲击期冻结致姿态变化后举起设备持续触发 VK_DOWN；④上升沿判据 `delta_dom >= last + acc_thr_g*0.5`（敲档 9=0.095g）。实测敲击 conc=0.61~0.92、扰动 0.38；档 9 仍偏灵敏，日常建议 5~6 档或集中度 0.65~0.7。
- 否决方向（勿再探索）：麦克风做触发（与 tap 时间互斥+功耗隐私）、GYR 判方向（测角速度不测平移）。RFC：`Doc/Plan/tap-false-trigger-mitigation.md`、`Doc/Plan/imu-tap-detection.md`。

### 5.3 体感鼠标（air mouse）

陀螺仪→鼠标移动，仅 Windows 端。协议：state_tx motion 帧 `type=0x11`（6 字节 version+type+int16 dx+int16 dy），control_rx `air_mouse_enabled`。方案 `Doc/Plan/imu-air-mouse.md`。

**真机联调定标结论（勿再走弯路）**：

- **轴映射**：设备竖握时 `dx=gx`（左右转腕）、`dy=gy`（俯仰点头），gz 不用（曾误设 dx=gz 方向全错）。
- **体感态必须 `voice_ble_request_fast_interval()`（7.5ms）**，否则 50Hz motion 帧在 slow interval(100~400ms) 限流成几帧/秒。
- **状态分裂**：固件不知体感态会照常启动硬件录音（设备录音+卡 Recording，桌面无视）——固件 `handle_primary_down/up` 在 `s_air_mouse_enabled` 时不录音只上报 button_click。**任何影响固件行为的"桌面态"必须同步告知固件**。
- **设备 Ready 但按下无反应**：先查是否体感激活态（主键被映射为鼠标左键，UI 曾不提示；2026-07-08 已补 `SendUiState("air_mouse")`+固件 AIR_MOUSE 屏显）。诊断：app 日志搜 `air mouse enabled on VS-XXXX`；长按发 `button_click`+`air mouse primary click ... left button` 即体感态，按侧键退出。

**控制模型演进**（幂律 gamma 与角度控制 θ 均已废弃）：当前为**速度控制+三段线性增益曲线**（`Doc/Plan/air-mouse-gain-curve.md`）——`v_target=omega×gain×factor(|omega|,curve)`，微调段 |omega|<15→0.15、中段 15..50 插值 0.15→4.0、甩动段 ≥50→4.0；base gain=sensitivity×16；曲线参数运行期化（4 配置项 `air_mouse_curve_*` 经 clamp）。固件死区 3dps；零偏校准 NVS 持久化（进入即响应）。标定教训：初版微调段 5/0.3 太窄致慢转冲过头，拓宽至 15/0.15 修复。

- **静止判据滞回（O4，`270941e`，真机待验证）**：jerk 判静止的根因缺陷——jerk 小只代表角速度稳定不代表没动，匀速慢转 jerk≈0 被误判 still 吃帧。修复：EMA 收敛零偏仍用 jerk，位移门控改用去偏幅值带滞回的 STILL/ACTIVE 状态机（`AIR_MOUSE_OMEGA_ENTER_DPS=4`/`EXIT=2`，bmi270.c）；死区 3dps 保留作 ACTIVE 帧内硬截断。零偏采集须等连续静止（手未稳采到 -20dps 污染）。
- **热调参面板（O9）**：非模态 `air_mouse_tuning_window`（托盘"体感鼠标调参"），滑块走 `UpdateAirMouseParams` 轻量路径（只更 live 参数不存盘），保存才写 config；7 滑块+invert_y+保存/重置。
- AirMouseTick 用固定 dt（WM_TIMER 周期；原 now-last_tick 在测试无间隔时 dt≈0 不累积，已记坑）。60Hz SetTimer 驱动，UI 线程无锁。

### 5.4 侧键单/双击冲突（已定案）

用户拍板（2026-07-03）：侧键**单击**=进/退体感鼠标，**双击**=恢复上次输入确认；有活跃录音/识别/待粘贴时单击仍走取消语义。固件层 `s_side_double_click_timer` 区分：单击延迟 500ms 双击窗口超时后发 `button_click secondary`，窗口内第二击发 `button_double_click secondary`；桌面端 `HandleButtonDoubleClick` secondary 分支调 `RestoreLastInputConfirmation`，体感态下双击忽略。代价：侧键单击响应延迟 500ms（可接受）。

---

## 6. 测试方法论（E2E L0–L4）

2026-07-15 完成 L0/L1/L3/L4 闭环（方案 `Doc/Plan/windows-e2e-test-plan.md`，后续 `Doc/Plan/windows-e2e-next-steps.md`）。资产：12 条语料（pcm/ogg/txt）+ `scripts/e2e_test/` 脚本 + `voicestick_integration_tests` CMake 目标。

### 6.1 L1 集成测试四个坑（`desktop/windows/tests/integration_tests.cc`）

真实火山 AsrClientWin+FakeBleCentral 注入语料 ogg→ASR→Paste→CER 断言，12 条全通过：

1. **断言点是 pasted_text 不是 final_countdowns**：focused_app+hold_to_talk+auto_paste 下 on_final→`EnterPendingConfirmation`(行 2162)→`CompletePendingPaste`→`Paste`(行 1848)，**不走 ShowFinalCountdown**；曾误等 final_countdowns 空转 40s。
2. **帧间必须 40ms 节流**：一次性灌入全部 Opus 帧致 ASR WebSocket 突发拥堵，partial 不完整且 final 不返回。FakeAsrClient 不涉及时序，单测发现不了——**真实 ASR 测试必须模拟实时节奏**。
3. **button_down 到 button_up ≥ 0.5s**（`kMinimumRecordingDurationSeconds`），否则 `HandlePrimaryButtonUp`(行 1114) 触发 CancelShortRecording 不启动 ASR。
4. **AsrClientWin 不在 voicestick_core**（外壳层），integration_tests 需单独编入 `src/asr_client_win.cc`（链接 voicestick_core+winhttp+bcrypt）。

CER：UTF-8 按字符拆分+编辑距离 DP；数字/中英混合语料 CER 不稳改用关键实体包含判定（case-insensitive）。无 key 时 return 77（CTest SKIP）。增量编译：`vcvars64.bat + cmake --build --target voicestick_integration_tests`（<10s，避免 build_win.bat 6 分钟全量）。

### 6.2 L3 固件回放两个坑

固件 `test_playback` 钩子（2026-07-15）：①PSRAM 栈 fread 崩溃（见 §1.6，预读 PSRAM buffer+memcpy）；②**回放必须实时节流**——memcpy 立即返回不像 esp_codec_dev_read 阻塞，全速循环 5 秒发 475 帧（应 ~125 帧）把 PCM 压成 4 倍速音频 ASR 无法识别，加 `vTaskDelay(pdMS_TO_TICKS(AUDIO_FRAME_MS))` 后 95 帧正常。脚本配套：run_l3_firmware.py 的 AUDIO_HEADER_FMT seq 是 `I`(u32) 不是 `H`（协议 header 16 字节 fmt `<BBHIIBBH`）。BLE 单连接：L3 需 app 断开（bleak 独占）。

### 6.3 L4 微信真机

`loopback_capture.py`（WASAPI 抓 CABLE Output，dev=43 48kHz 2ch）+`run_l4_wechat.py` 半自动编排。验证通过：微信识别正常+CABLE peak=25202。坑：

- **断言阈值**：微信模式 CABLE 电平 peak 高（25202）但平均能量低（~46，nonzero_ratio 仅 0.025），nonzero_ratio 判据不适用；主判据改 **peak>1000**，nonzero_ratio/silent_ratio 只判极端（<0.005/>0.95）。
- **BLE 单连接约束**：L4 需 app 连设备渲染，bleak 不能同时连，故人工按键说话替代 L3 自动回放（全自动需 app 加 `--test-playback` 命令行，见 next-steps 2.7）。
- config 临时改 `wechat_input_method` 验证后改回，app 需重启读 config。
- 脚本求仓库根用 `os.path.dirname(os.path.abspath(__file__))` 同目录定位，勿用 `rsplit("\\")`（相对路径错层）；Git Bash 下路径用正斜杠（反斜杠被转义）。

### 6.4 通用测试教训

- 点动残留 active 测试**避免发首帧**（`TestCoordinatorWechatClickToTalkStaleActiveNewClickStartsNew` 发首帧触发 hotkey 重建计数归零+detached 硬超时 thread 致卡 934s）；对齐 hold 范式用 `fake_renderer->start_count` 断言（renderer 复用累加），不用 `fake_hotkey->send_click_count`（hotkey 每会话重建）。
- 跨 0.5s 阈值用真实 `sleep_for(520ms)`（core_tests 已有先例），且 sleep 须在音频帧之前。
- ASR 回调线程与测试主线程共享字段加 mutex。

---

## 7. 通用排查方法论

### 7.1 先找真实信号源

排查"设备/app 不工作"**第一步必须先找到诊断信号（日志）的真实位置并确认能读到**，再下结论。典型案例：app 日志在 `%LOCALAPPDATA%` 而非 Roaming，找错位置误判"app 不工作"绕进深睡排查；读到日志立刻看到 `connected stage=ready`，真因是 --ota 解析。要点：串口采空 ≠ 设备深睡（多为 USB 重新枚举句柄失效）；主动加临时诊断日志定位后立即移除；根据诊断信号真实内容下结论，而非"应该是"。

### 7.2 直接证据验证

验证"切换某资源指向"类功能须看被切资源的直接状态：auto_switch 验证时"微信识别到文字"在切换生效与"平时默认本就是 CABLE"两种情况下都成立，无法区分；必须录音期间直接观测默认录音设备实时切换，且先排除环境恰好满足。同理："真机验证生效"必须有直接证据（寄存器读回、电平行为对比），不能只看"识别到文字"。MSI 验证用 SHA256 而非肉眼。

### 7.3 跨工程移植方法论（8 要点）

1. **优先整组件复制**：CMake REQUIRES 只系统组件+.c 只 include 系统头即零自定义依赖，直接 cp -r（voice_ble/stick_s3_board/ui_status 均如此），重写等于丢踩坑经验。
2. **sdkconfig.defaults 全量 diff 对齐**（不只 BT/NimBLE）：漏抄 PM_ENABLE/tickless/240MHz/MAIN_XTAL_PU 致只开 MODEM_SLEEP 无配套、BT 时钟漂移偶发断连（reason=8 supervision timeout）。
3. **defaults 必须纯 ASCII 无行内注释**：中文 Windows 下 kconfgen 用 GBK 解码，中文注释 UnicodeDecodeError；行内 `# 注释` 致值解析失败用默认值（MTU 247→256、MSYS 200→12，MSYS=12 会致 BLE 音频拥堵）。
4. **改 defaults 必须删根 sdkconfig 才生效**（删 build/ 不够），改后 grep CONFIG_ 核实实际值。
5. **采集参数逐行 diff**（PGA/ALC 寄存器/HPF 系数/Opus 参数/采样率/帧大小）。
6. **独立工程默认无 USB 日志通道**：显式 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`+`UART_NUM=-1`（见 §2.3）。
7. **tx_task drain 时序照搬**：sentinel 不 break，goto drain 排空再发 audio_end（见 §1.4）。
8. **真机验证不只看编译通过**：编译只证语法；栈溢出/断连/drain 丢失都是真机才暴露。

### 7.4 方案文档位置

设计方案存当前仓库 `Doc/Plan/`（大写 Doc，git 大小写敏感；早期文档写 `docs/` 是历史误导）。写前可 `git ls-files | grep -i doc` 确认。

---

## 8. 遗留待办 / 待真机验证项

截至 2026-07-17，各记忆中明确挂起的 TODO 汇总（完成一项划掉一项）：

- **体感鼠标真机标定**：增益曲线倍率/阈值待真机标定（`Doc/Plan/air-mouse-gain-curve.md` 参数可热调）；O4 静止判据滞回（`AIR_MOUSE_OMEGA_ENTER_DPS=4/EXIT=2`）固件无单测，**真机验证待做**（串口 ESP_LOGD 统计 ACTIVE 占比+慢转测试）；开局卡顿未根治（固件校准期零反应+BLE interval 协商），留路径 2 改固件；角度控制残留文档中 gain×320 系废弃模型的旧值，以速度控制模型为准。
- **深睡 5min（S2→S3）未验证**：USB 供电下 `poweroff_allowed_now()` 返回 false 不进深睡，需拔 USB 电池供电实测；亮度 20/4 视觉可读性需肉眼确认；第二阶段深睡 IMU any-motion 唤醒（取消 `main.c:437` 的 `#if 0`）待第一阶段效果确认后推进。
- **微信模式电平修复真机回归**：PGA 18→24+maxlevel 8→12 后，解码峰值应升到 8000~15000、CABLE Input 20%+、微信能识别；关键回归项**近场大声不削波**。
- **auto_switch 真机验证第 3–8 项**：Teams/Skype 零干扰、尾音完整、残留自愈、退化、VB-CABLE 未装降级（①②已通过）。
- **VB-CABLE 授权**：公开发布前必须改走 B2-下载或取得 VB-Audio 商业分发授权（当前随包携带仅内测）。
- **E2E next-steps**（`Doc/Plan/windows-e2e-next-steps.md`）：L4 多语料抽检 → run_all.py 编排器 → compare_ogg.py 链路保真 → cer.py；L2 跳过；capture_helper/微信自动化/CI 低优先级；全自动 L4 需 app 加 `--test-playback` 命令行。
- **wechat buffer 20ms 观察项**：若真机抖动致丢字可回退 30/50ms。
- **macOS 流式精修**：仅 Windows 端已实施，macOS 端（LLMRefinementClient、SSE 流式、精修接入）待推进；当前 macOS 连非流式精修都没有。
- **firmware 偶发断连 reason=8 排查**：whisper_pen 已定位为 slow interval(latency=4)+supervision timeout（PM 配置补全根治）；firmware 是否也有此偶发断连待验证（差别点 MAX_BONDS=1 vs 3）。
- **Qt 迁移**：暂缓，留作后续 UI 美化方向。

---

## 9. 已过时 / 仅历史价值的条目

以下条目已被后续进展推翻或失去时效，保留仅为考古；正文相关位置已按最终结论改写：

| 条目 | 过时原因 |
|---|---|
| v1.7.2-release-status | "7 个提交未 push / 无 origin/main / 固件还是 1.6.8" 均为 2026-06-30 时点状态，后续早已 push 并迭代到 v2.x。 |
| whisper-pen-day-summary-2026-0711 | whisper_pen 工程已删除，经验沉淀在 `Doc/Plan/whisper-pen-es8311-port.md` 与 §7.3 移植方法论；Voice Stick 已升 v2.0.0。 |
| whisper-pen-es8311-port-done | 同上（工程已删）；其中可复用教训（栈 32768、tx_task drain、PM 配置、USB 控制台、defaults 行内注释）已并入 §7.3/§2.3/§1.4。 |
| whisper-pen-disconnect-reason8 | 工程已删；诊断手法（NVS+state_tx 上报断连 reason）已并入 §2.3，结论在 §8 留了一项 firmware 待验证。 |
| whisper-pen-tencent-vad-not-firmware | 工程已删；"腾讯 VAD 切句不续识别"结论已并入 §3.9；"audio frame slow 采样日志误判丢帧"已并入 §2.3。 |
| esp32s3-ble-wifi-coexist-lazy-start | Wi-Fi 已于 v1.8.0 物理删除（§2.1），"esp_wifi_start 延迟到 BLE 连接后"约束暂无适用代码；若未来重加 Wi-Fi 再参考。 |
| freertos-timer-callback-stack-trap | 触发现场（Wi-Fi set_config）已删除；通用教训（timer/event 回调栈不够调大栈 API、专用 worker task）已并入 §1.8。 |
| sticks3-flash-skill-outdated | meta 条目：skill 已于 2026-07-09 重写完成且三处副本同步，内容已固化，无需再跟踪"旧描述已清除"这件事本身。 |
| feedback-voicestick-build-run / feedback-windows-test-msi-commit | 用户偏好类约定，已并入 §3.1 正文，无独立查阅价值。 |
| windows-only-no-macos-streaming | 当时约束，已转为 §8 待办（macOS 精修待推进）。 |

> 另注：记忆中提及的 commit 短哈希、文件行号（如 voice_stick_coordinator.cc:467、main.c:347）均为记录时点的定位，代码演进后可能漂移，引用前以当前源码为准。
