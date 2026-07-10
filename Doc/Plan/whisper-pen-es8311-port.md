# whisper_pen_firmware 改造为 ES8311（复用 StickS3 硬件）

## 背景与目标

脚手架 `whisper_pen_firmware/` 原按 ICS-41351 数字 MEMS 设计（裸 I2S、无 MCLK、无 I2C）。
实际复用 M5Stack StickS3 硬件，麦克风是 ES8311 codec，需改为 `esp_codec_dev` 驱动栈。
移植基准为 `firmware/` 已踩坑的实战实现，照搬配置值，不重踩寄存器/供电坑。

## 硬件对照

| 项 | 脚手架现状（ICS-41351） | StickS3 实际 | 权威源 |
|---|---|---|---|
| I2S | BCLK4/WS5/DIN6，无 MCLK | BCLK17/LRCK15/DOUT16(麦)/DIN14(喇叭)/MCLK18 | stick_s3_board.h:14-21 |
| I2C | 无 | SCL48/SDA47（codec+PMIC 共用） | stick_s3_board.h:11-12 |
| 采集驱动 | 裸 driver/i2s_std（mono read） | esp_codec_dev（stereo read 转 mono） | firmware/audio_pipeline.c:333 |
| PTT | GPIO4（与 BCLK 撞）/7 | GPIO11（主键，RTC 唤醒已验证） | stick_s3_board.h:7 |
| 电源 | 直连锂电 | M5PM1 PMIC（codec 走 L3B 供电） | stick_s3_board.c:230 |
| 评估卖点2 | slot_bit_width=32 硬件取高16 | **作废**（ES8311 直接 16-bit 输出） | — |

## 移植基准（firmware/ 行号佐证，照搬）

- `init_i2s`：firmware/audio_pipeline.c:114-155
  - I2S_NUM_1 MASTER，dma_desc=4 dma_frame=120，slot=16BIT **STEREO**（非 MONO）
  - rx+tx 都 init + **enable**（codec_open 前 enable，否则 reconfig 报 "not enabled"）
- `init_codec`：firmware/audio_pipeline.c:157-240
  - I2C bus 从 `stick_s3_board_i2c_bus()` 拿
  - es8311_cfg：use_mclk=true, digital_mic=false, master_mode=false, pa_pin=-1
  - `esp_codec_dev_open` channel=2
  - **PGA=18.0**（set_in_gain，踩坑值，见 memory `audio-gain-alc-tuning`）
  - **ALC 寄存器**：write_reg(0x18,0x83)/(0x19,0x80)/(0x1A,0x00)
    - 位域以 Linux 主线 es8311.h 为准（memory `es8311-alc-bitfield-authoritative-source`）
- 采集：firmware/audio_pipeline.c:332-340
  - `esp_codec_dev_read(stereo, 640*2)` -> `mono[i]=stereo[i*2]`（取左声道）
  - 再过软件 HPF（脚手架已有，保留）
- `deinit`：firmware/audio_pipeline.c:265-319
  - deinit_codec：close+delete+delete_if（codec_if/data_if/gpio_if/ctrl_if）
  - deinit_i2s：**只 del_channel 不 disable**（codec_close 已 disable，重复 disable 报 not enabled，memory `audio-i2s-disable-not-enabled`）

## 逐文件改造清单

### 1. 新增 `components/stick_s3_board/`（复制自 firmware/）
- 完整复制 `firmware/components/stick_s3_board/`（.h + .c + CMakeLists.txt），零踩坑风险
- 它只做 I2C + PMIC 初始化，不依赖 LCD/IMU，脚手架可直接用
- 电池/LCD 函数不被调用即死代码，不影响

### 2. `components/audio_pipeline/` 改造
- **audio_pipeline.h**：删 `AP_PIN_BCLK/WS/DIN`（引脚归 stick_s3_board），保留采样率/帧参数
- **audio_pipeline.c**：
  - 删裸 `driver/i2s_std` 的 init 与 24->16 硬件取高16 逻辑（评估卖点2 作废，改注释说明）
  - 换入 firmware 的 `init_i2s` + `init_codec`（STEREO slot + esp_codec_dev + ES8311 寄存器全套）
  - 采集 `i2s_channel_read(mono)` -> `esp_codec_dev_read(stereo)` + `mono[i]=stereo[i*2]`
  - `deinit` 换入 firmware 版（只 del 不 disable）
  - HPF / Opus / drain / 双核任务逻辑保留不动
- **CMakeLists.txt**：`REQUIRES voice_ble stick_s3_board esp_driver_i2s esp_codec_dev`
- **idf_component.yml**：加 `espressif/esp_codec_dev: "^1.3.4"`

### 3. `components/voice_ble/` 整组件复制（替换脚手架现状）
- 删脚手架现状 voice_ble（16-bit 0xFF10/11/12，仅 send_audio+button_event）
- 复制 `firmware/components/voice_ble/`（.h + .c + CMakeLists.txt，1120 行，128-bit 协议完整实现）
- 依赖核实（已确认零自定义依赖）：REQUIRES 仅 `app_update bt nvs_flash`；voice_ble.c 只 include 系统头 + voice_ble.h
- 复制后不改 device_info：`hardware:"stick_s3"` 符合脚手架硬件；`firmware_version` 取 esp_app_desc（构建版本），自动来自脚手架自身
- OTA（ota_rx/ota_tx）随组件带入但 main 不注册 ota 回调，自然不启用；保留以备后用
- send_tap/send_motion 函数存在但脚手架无 IMU 不调用，无副作用

### 4. `components/ptt/` 改造
- **ptt.h**：`PTT_PIN 4` -> `11`（或引用 `STICK_S3_PIN_BUTTON_FRONT`）
- ptt.c 逻辑不变（hold_to_talk + 双击 + ext0 唤醒，GPIO11 是 RTC_GPIO 可 ext0）

### 5. `main/main.c` 改造
- app_main 编排顺序：`stick_s3_board_init()` -> `voice_ble_init` + 注册 connection/control 回调 -> `audio_pipeline_init` -> `ptt_init`
  - board_init 必须最先（PMIC 配 L3B 给 codec 供电 + I2C bus）
- 连接建立回调：调 `voice_ble_send_device_info`（上报固件版本/能力，桌面端配对识别）
- control_rx 回调：解析 `interaction_mode`（hold_to_talk/click_to_talk，影响 ptt 行为）；`ui_state` 无 LCD 忽略；air_mouse/tap 相关忽略
- PTT 回调对齐 voice_ble 新接口：
  - on_recording_start：`audio_pipeline_start` + `voice_ble_send_button_down("primary", session_id)`
  - on_stop：`voice_ble_send_button_up("primary", duration, session_id)` + `audio_pipeline_stop`
  - on_double_click：`voice_ble_send_button_double_click("primary")`
- idle deep sleep 前调 `audio_pipeline_stop` + `stick_s3_board_prepare_deep_sleep`

### 5. `README.md` 改造
- 评估第2点标注"仅适用 ICS-41351 独立板；ES8311 版本 codec 直接 16-bit 输出"
- 待真机验证项更新为 ES8311 相关

## 已确认决策

**结论**：D1=改 128-bit 互通桌面端（复制 firmware/voice_ble 组件，1120行）；D2=stick_s3_board 复制到脚手架。
依赖核实（firmware 组件 REQUIRES 图）：voice_ble REQUIRES `app_update bt nvs_flash`、stick_s3_board REQUIRES `driver esp_driver_i2c`，均零自定义依赖，干净复制；ui_status（LCD/LVGL）/ bmi270（IMU）脚手架不带，voice_ble 不依赖它们。

**原考虑过程（已废弃）**

**D1. voice_ble 协议形态**（影响是否改 voice_ble.c）
- (a) 保持现状 16-bit UUID（0xFF10/11/12），独立测试，不互通现有桌面端 —— 改造量最小，先把音频链路打通
- (b) 改 128-bit UUID + 完整 state_tx/control_rx 协议，互通现有 Voice Stick 桌面端 —— 可复用桌面端 ASR 链路验证
- 推荐 (a)：真机验证目标是"采到声 + 编码 + BLE 发出"，独立测试端够用；互通留作后续

**D2. stick_s3_board 复用方式**
- (a) 完整复制到脚手架 components/（推荐，零踩坑，独立工程干净）
- (b) EXTRA_COMPONENT_DIRS 引用 ../firmware/components（不复制，但脚手架依赖 firmware/ 路径）

## 待真机验证项（ES8311 版本）
1. codec 能读出非零 PCM（验证 PMIC L3B 供电 + I2C 通信）
2. PCM peak 日志（需补：见下）确认采声 + 不削波（PGA=18dB + ALC）
3. Opus 帧大小日志（需补）确认编码正常
4. MTU/interval 实测（BLE 侧日志已有）
5. drain 尾音（松开读 2 帧）真机体感
6. ext0 唤醒 GPIO11
7. PSRAM 栈水位

## 需补的诊断日志
- PCM peak：每 N 帧打印 int16 峰值（看采声 + 削波）
- Opus 帧大小：每帧打印 encoded len（看编码正常）

## 风险
- PMIC 未配致 codec 无电：mitigate = main 最先调 stick_s3_board_init
- stereo->mono 取错声道采到空：mitigate = 照搬 firmware `mono[i]=stereo[i*2]`

## 执行结果

编译通过（idf_cli.py -c，EXIT_CODE=0，约 19 分钟，1317 编译单元）：
- `build/whisper_pen.bin` 815216 字节（比 ICS-41351 版 737568 增约 78KB，对应 voice_ble 1120 行 + stick_s3_board + esp_codec_dev）
- `sdkconfig`：CONFIG_BT_ENABLED=y / BT_NIMBLE_ENABLED=y / ATT_PREFERRED_MTU=256 / SPIRAM=y / SPIRAM_MODE_OCT=y
- managed_components：78__esp-opus + espressif__esp_codec_dev 已拉取
- 无 stderr 错误

诊断日志已加：`audio_task` 每秒打印 `diag: pcm_peak= opus_avg= (N frames)`，看采声/削波/编码。

待真机验证项见 README.md（10 项，含 codec 供电、PCM peak、互通桌面端等）。

改造额外修正（方案外发现）：
- ptt 回调加 duration（`ptt_stop_fn(uint32_t)` + `ptt_button_fn(...,uint32_t)`），对齐 voice_ble `send_button_up` 的 duration_ms 参数
- deep sleep 从 ext0 改路径 B ext1（移植 firmware/main.c:453-495，含 rtc_gpio_pullup + settle high + prepare_deep_sleep）

## 真机验证结果（2026-07-11）

COM17 烧录 + 真机验证通过，全链路打通（屏幕亮 + 连 Win 端 + 识别到文字）。启动日志实证：
- `stick_s3_board: I2C probe ok + M5PM1 l3b_power=on` ✅ PMIC/L3B 供电
- `ui_status: display ready` ✅ LCD + LVGL 初始化
- `voice_ble: BLE initialized as VS-D010 + advertising` ✅ NimBLE 128-bit 互通
- `voice_ble: connected handle=1 + interval=6 + phy updated` ✅ 桌面端连接，7.5ms interval + PHY 更新生效
- `main: interaction_mode=hold_to_talk` ✅ control_rx 下发生效
- 录音 -> ASR -> 文字全链路打通

真机烧录过程额外修复（disciplined-execution 逐个找根因）：
1. **sdkconfig.defaults 行内注释致配置被忽略**：`ATT_PREFERRED_MTU=247` / `MSYS_1_BLOCK_COUNT=200` 因行内 `#` 注释被 kconfgen 当值解析失败、用默认值（256/12）。删行内注释修复。MSYS 默认 12 块会致 BLE 音频拥堵（memory ble-audio-congestion-progress）。
2. **缺 USB JTAG 控制台配置**：StickS3 无 UART 引出，必须配 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` + `UART_NUM=-1`，否则 USB 串口读不到任何日志（对齐 firmware）。注意不能用 `ESP_CONSOLE_UART_NONE`（与 USB_SERIAL_JTAG 互斥致 NONE 生效）。
3. **audio_task 栈溢出**：脚手架 24KB 栈在加 LVGL 后溢出（`stack overflow in task audio_pipeline`），改 32768 对齐 firmware + 补 stack_hwm 日志。
4. **加回 ui_status 组件**：原改造去掉 LCD，屏幕黑。从 firmware 复制 ui_status（LCD+LVGL+5 图标）+ 加 LVGL sdkconfig 配置（LV_CONF_SKIP/COLOR_DEPTH_16/MONTSERRAT_16 等）+ main 集成状态渲染。

bin 体积：800KB（无LCD）-> 1.39MB（加 LCD+LVGL+图标）。

待验证（需长录音触发）：
- stack_hwm 是否够用（32768 栈水位）
- pcm_peak 衰减：上次日志显示 3984->752->435->346 递减，疑似 ALC 增益问题（第一秒正常后递减）。短录音 ASR 靠服务端 AGC 拉起可用，长录音/远场待调参。
- I2S enable/disable 归属错报 not enabled：mitigate = 照搬 firmware deinit 只 del 不 disable
