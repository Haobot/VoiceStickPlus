# StickS3 分模式功耗记账与耗电评估方案

> 状态：设计稿，待评审（2026-08-01）
> 范围：固件 `firmware/`（新增 power_log 组件）+ BLE 协议扩展 + `scripts/` 分析工具；桌面端仅做日志导出
> 目标板：M5Stack StickS3（ESP32-S3），PMIC=M5PM1（I2C 0x6e）
>
> **关键硬件事实（已由 M5PM1 官方库源码核实）**：
> 1. M5PM1 **无电流测量寄存器**。可读量仅：VBAT(0x22/0x23)、VIN(0x24/0x25)、5VINOUT(0x26/0x27)、GPIO ADC(0x28)。板上无法直接测瞬时电流。
> 2. M5PM1 有 **32 字节 RTC RAM（0xA0–0xBF，睡眠保持）** 与秒级定时器（0x38–0x3D），可用于跨关机/冷启动保留记账锚点。
> 3. 固件已有 S0/S1/S2/S3 四级电源状态机（`main/main.c`，设计见 `Doc/Plan/固件待机省电策略.md`），模式边界即耗电时段分界。

## 1. 背景与问题

用户反馈 Stick 续航偏短。要制定省电策略，首先缺的是**可观测性**：不知道一天里电花在哪个模式（亮屏待机？熄屏保连？录音？断连广播？），也无从验证某项省电改动是否真的有效。

现有基础：

- 固件电源状态机已完备：S0 Active（背光 32）→ S1 Resting（背光 8）→ S2 ScreenOff（熄屏保连）→ S3 PowerOff（M5PM1 真关机），另有录音、BLE 广播（断连）两个高耗电叠加态。
- `stick_s3_board_battery_voltage_mv()` 已可读 VBAT（`components/stick_s3_board/stick_s3_board.c:267`）。
- SPIFFS `storage` 分区约 1984 KB，功耗日志只需几十 KB 环形空间。

缺的是：把「何时处于什么模式、持续了多久、期间电池掉了多少」记录下来并导出分析。

## 2. 设计目标

- 持续记录设备在**各时段、各工作模式**下的驻留时长与电池电压变化。
- 离线产出**分模式耗电评估报告**：各模式时长占比、估算能耗（mAh）占比、VBAT 衰减曲线。
- 支撑两类决策：①找出耗电大头指导省电策略；②省电改动前后对比验证（A/B）。
- 不干扰正常音频/BLE 链路，自身功耗可忽略，默认常开。

非目标（YAGNI）：

- 不做毫秒级瞬时电流波形（无硬件支持，属 bench 标定范畴）。
- 不做桌面端实时功耗 UI（第一版只要导出 + 离线报告）。
- 不改动现有电源状态机行为（纯观察，不加策略）。

## 3. 方案选型

| 方案 | 做法 | 精度 | 成本 | 结论 |
|---|---|---|---|---|
| **A. 固件分模式时间记账 + VBAT 压降估算** | 模式切换记账 + 周期 VBAT 采样，时长 × 标定电流估算能耗 | 中（占比可靠，绝对值依赖标定） | 零硬件，纯固件+脚本 | **主体方案** |
| B. 外置功耗仪 bench 标定 | INA219/PPK2/Joulescope 串电池线，逐模式驻留测真值 | 高 | 需拆机接线、额外硬件 | 作为 A 的**可选标定手段** |
| C. Grove 口挂 INA219 常驻 | 外接 INA219 模块在线测流 | 高 | 需改硬件串入电池回路，不现实 | 否决 |

**推荐 A 为主、B 为可选标定。** A 回答「日常使用中电花在哪」，B 回答「每个模式的真实电流是多少」，两者结合即可从相对占比升级到绝对 mAh 估算。

## 4. 总体架构

```
固件 power_log 组件
  │  模式切换事件（复用现有状态机钩子） + 周期 VBAT 采样
  ▼
RAM 环形缓冲 ──定时/满时 flush──▶ SPIFFS /storage/power_log.bin（环形覆盖）
  │                                   ▲ M5PM1 RTC RAM 存关机锚点（跨 S3 冷启动）
  │  BLE 导出（control_rx 命令 → state_tx 分片）/ 串口 dump（开发期）
  ▼
scripts/e2e_test/power_log_dump.py  →  power_log.csv
  ▼
scripts/e2e_test/power_report.py + power_model.json（每模式标定电流）
  ▼
报告：分模式时长/能耗占比、VBAT 衰减曲线、估算续航
```

## 5. 固件设计：power_log 组件

新建 `firmware/components/power_log/`（结构仿 `bmi270` 组件），`REQUIRES: stick_s3_board voice_ble esp_timer nvs_flash spiffs`。main 组件 `REQUIRES` 追加 `power_log`。

### 5.1 模式枚举

与现有状态机一一对应，加一个叠加优先级规则：**录音 > 广播 > S0/S1/S2**（录音和广播期间屏幕态不重要，耗电由录音/广播主导）。

```c
typedef enum {
    POWER_MODE_S0_ACTIVE = 0,   // 亮屏交互（背光 32）
    POWER_MODE_S1_RESTING,      // 暗屏待机（背光 8）
    POWER_MODE_S2_SCREEN_OFF,   // 熄屏保连
    POWER_MODE_S3_POWER_OFF,    // 关机/深睡段（时长由锚点推算）
    POWER_MODE_RECORDING,       // 录音会话（含 BLE 音频流）
    POWER_MODE_ADVERTISING,     // 断连广播中
    POWER_MODE_OTA,             // BLE OTA（罕见，单列便于剔除）
    POWER_MODE_COUNT
} power_mode_t;
```

### 5.2 记录格式与存储

事件记录定长 12 字节，RAM 环形缓冲（容量 64 条），flush 到 SPIFFS 文件 `/storage/power_log.bin`（环形文件，上限 256 KB，写满回卷覆盖最旧段；文件头记写指针与 epoch 序号）：

```c
typedef struct __attribute__((packed)) {
    uint32_t uptime_s;    // esp_timer 秒级 uptime（设备无 RTC，用相对时间）
    uint16_t vbat_mv;     // 记录时刻电池电压
    uint8_t  mode;        // power_mode_t（事件含义见下）
    uint8_t  flags;       // bit0=充电中, bit1=USB供电, bit2=周期采样(非切换事件), bit3=关机段恢复记录
    uint8_t  reserved[4]; // 对齐预留
} power_log_entry_t;      // 12 字节
```

- **切换事件**：`power_log_note_mode(mode)` 在模式变化时调用（记录进入新模式的时刻）。
- **周期采样**：每 60s 一条 `flags.bit2=1` 的 VBAT 采样（模式字段填当前模式）。60s × 12B ≈ 17 KB/天，256KB 环形可存约两周。
- **flush 策略**：缓冲满或每 10 分钟 flush 一次；录音/OTA 期间禁止 flush（避免 SPIFFS 写抢占音频链路），顺延到会话结束。

### 5.3 跨 S3 关机的记账（M5PM1 RTC RAM）

设备无 RTC，S3 真关机后 ESP32 掉电，uptime 归零。处理：

1. 进 S3 前：向 M5PM1 RTC RAM（0xA0 起）写入 `{关机前 uptime_s, vbat_mv, 魔数, CRC}`，并把一条 `POWER_MODE_S3_POWER_OFF` 切换事件 flush 进 SPIFFS。
2. 冷启动时：读回 RTC RAM 校验魔数/CRC，恢复一条「关机段」记录——时长未知（无 wall clock），标记 `flags.bit3=关机段`，时长由第 6 节的 BLE 时间锚点或桌面端连接记录补全。
3. RTC RAM 读写复用 `stick_s3_board` 的 `pmic_read_regs/pmic_write_reg`（需导出为组件接口或新增 `stick_s3_board_pmic_rtc_ram_read/write`）。

### 5.4 时间基准与 wall clock 锚点

- 基准为 uptime 相对时间；分析阶段需要把它映射到真实时钟。
- BLE 每次连接成功后，桌面端在现有握手/电量轮询流程里附带当前 epoch 秒（复用 `control_rx` 配置通道下发），固件记一条锚点事件（复用 flags 位或 `power_log_set_time_anchor(epoch, uptime)`，分析脚本据此对齐）。
- 未连接时段无需对齐——分模式统计只依赖相对时长。

### 5.5 接入点（main.c，纯增量钩子）

| 钩子 | 位置 | 调用 |
|---|---|---|
| S0→S1 | `s_display_dim_timer` 到期处理 | `power_log_note_mode(S1)` |
| S1→S2 | `s_display_off_timer` 到期处理 | `power_log_note_mode(S2)` |
| →S3 | `enter_power_off()` | `power_log_note_mode(S3)` + RTC RAM 锚点 + flush |
| 活动回 S0 | `note_activity()` | `power_log_note_mode(S0)` |
| 录音起停 | 录音 session begin/end | `power_log_note_mode(RECORDING / 回到先前屏幕态)` |
| BLE 连接/断连 | `APP_EVENT_BLE_CONNECTED/DISCONNECTED` | `ADVERTISING` ↔ 屏幕态；连接时请求时间锚点 |
| OTA 起止 | OTA 事件 | `power_log_note_mode(OTA)` |

均为一行式调用，不改变现有状态机逻辑。`power_log_init()` 在 `app_main` 中 `stick_s3_board_init()` 之后调用（挂载 SPIFFS、加载环形文件头、恢复 RTC RAM 关机段、启动 60s 采样定时器）。

### 5.6 自身开销

I2C 读 VBAT 每 60s 一次（<1ms），RAM 缓冲 768B，SPIFFS 写每 10 分钟一次。对功耗影响可忽略；采样本身不打断 light sleep（esp_timer 周期唤醒本来就是 tickless 的正常行为）。

## 6. BLE 协议扩展（导出通道）

复用现有 `control_rx`（主机→设备）与 `state_tx`（设备→主机）JSON 通道，不新增 GATT 特征：

- 请求：`{"power_log":{"cmd":"dump","offset":<字节偏移>,"max":<本片最大字节>}}`
- 响应（state_tx，可分片）：`{"power_log":{"seq":<n>,"offset":<o>,"total":<T>,"eof":<0|1>,"data":"<base64>"}}`，单片 ≤160B 控制 MTU 压力（遵循 AGENTS.md 的 JSON 长度约束）。
- 控制：`{"power_log":{"cmd":"clear"}}`（清空日志）、`{"power_log":{"cmd":"time_anchor","epoch":<秒>}}`（时间锚点下发）。
- 开发期回退：串口命令 `power_log dump`（走现有 USB JTAG 控制台）以 hex/base64 打印，不依赖 BLE。
- 同步更新 `Doc/Ref/protocol.md`（协议变更的既定要求）。

## 7. 导出与分析工具

遵循 `scripts/e2e_test/` 现有风格（bleak 独立 BLE 连接，要求 VoiceStickApp 先断开，StickS3 BLE 独占单连接）：

- `scripts/e2e_test/power_log_dump.py`：连设备 → 发 dump 命令 → 收分片 → 落盘 `power_log.bin` 并解析为 `power_log.csv`（列：相对时间、对齐后 wall time、模式、VBAT、充电/USB 标志）。
- `scripts/e2e_test/power_report.py`：读 CSV + `power_model.json`，输出：
  - 各模式驻留时长与占比（区分充电/纯电池时段，充电段不计入耗电）；
  - 各模式估算能耗 `mAh = 时长(h) × I_mode(mA)` 与占比排序；
  - 纯电池长时段的 VBAT 衰减斜率回归 → 实测平均电流，与模型总量交叉校验；
  - 按当前使用分布估算续航时间。
- `power_model.json` 初值用经验估计（见第 8 节），bench 标定后更新。

## 8. 标定方案（可选，对应方案 B）

目标：给 `power_model.json` 的每个模式一个可信电流常数。

- **无仪器路径（默认）**：利用固件自身记录做标定——满电后脚本通过 BLE 命令让设备逐模式驻留 ≥30 分钟（现有 `control_rx` 已能下发 ui_state/配置，必要时加 `{"power_log":{"cmd":"hold_mode","mode":<m>}}` 调试命令），用该段 VBAT 线性回归 ΔV/Δt，结合电池标称容量（StickS3 约 200 mAh 级，按实际批次填写）与电压-SOC 近似曲线换算 mA。VBAT-SOC 非线性段的误差通过对同一段电压区间做多次测量取平均缓解。
- **有仪器路径（更准）**：INA219/PPK2/Joulescope 串电池正极，跑同样的逐模式驻留脚本直接读真值，写回 `power_model.json`。
- 标定是一次性工作；电池老化后占比分析不受影响（占比只依赖时长与相对电流比）。

## 9. 精度与局限（如实说明）

- 本方案不是电流计：单模式内部的瞬时波动（BLE 发射尖峰、背光 PWM）被平均化，结论粒度是「模式级分钟级」。
- 绝对 mAh 估算的误差主要来自 VBAT-SOC 非线性与电池容量漂移；**分模式占比与改动前后对比是可靠的**，绝对值当作 ±20% 量级参考。
- S3 关机段时长依赖 BLE 重连锚点补全；长期不连接的设备关机段时长为未知（但关机段功耗近零，不影响找耗电大头）。
- 温度、电池内阻随电量的变化未建模。

## 10. 实施步骤（分阶段）

- **阶段 1：固件记账核心**。新建 `power_log` 组件（5.1–5.3、5.5），SPIFFS 环形存储 + 60s VBAT 采样 + 全部模式钩子；串口 `power_log dump` 验证。验证方式：`idf.py build` + 真机跑半天后串口 dump 检查记录完整性。
- **阶段 2：BLE 导出 + 时间锚点**。协议扩展（第 6 节）+ `power_log_dump.py`；同步 `Doc/Ref/protocol.md`。
- **阶段 3：分析报告**。`power_report.py` + `power_model.json` 初值；用真实一天的数据出第一份分模式耗电报告。
- **阶段 4：标定（可选）**。按第 8 节流程更新 `power_model.json`；输出标定结论到 `Doc/Expe/`。
- 每阶段独立可用：阶段 1 完成后即可串口取数人工分析。

## 11. 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| SPIFFS 写与音频链路抢占 | 录音卡顿 | 录音/OTA 期间禁 flush；flush 周期 10min 且单次仅数百字节 |
| 无 RTC 导致时间对齐缺失 | 报告只有相对时段 | BLE 锚点 + 相对时长足够支撑分模式统计；文档明示 |
| RTC RAM 跨关机数据丢失（M5PM1 休眠策略差异） | S3 段时长估不准 | 魔数+CRC 校验，失败则该段标记未知，不伪造数据 |
| VBAT-SOC 非线性 | 绝对 mA 估算偏差 | 以占比为主要结论；标定时对同电压区间多次回归 |
| 环形文件写穿 SPIFFS 磨损 | 长期 flash 寿命 | 17KB/天、10min 一次小写，磨损可忽略；回卷机制成熟模式 |
| 记录自身引入功耗 | 观测干扰 | 开销测算 <0.1%（5.6 节），可忽略 |

## 12. 测试计划

固件无自动化测试框架，按项目惯例以编译 + 真机清单验证：

- [ ] `idf.py build` 通过，`power_log_init` 日志正常，SPIFFS 挂载失败时优雅降级（仅 RAM 记录 + 警告）。
- [ ] 各模式切换均产生记录：S0→S1→S2→S3、录音起停、断连广播、OTA。
- [ ] 60s VBAT 周期采样连续，充电/USB 标志正确。
- [ ] S3 关机→按键唤醒冷启动后，RTC RAM 锚点恢复出关机段记录。
- [ ] 环形文件写满回卷后最旧数据被覆盖、新数据完整。
- [ ] 录音全程无 flush 发生（日志确认），录音结束后补 flush。
- [ ] BLE dump 分片传输完整（与串口 dump 二进制一致）；`clear` 生效。
- [ ] 断开 VoiceStickApp 后 `power_log_dump.py` 成功导出 CSV；`power_report.py` 报告数值与手工核算一致。
- [ ] 连续记录 24h 真实使用，报告能复现「续航偏短」的耗电分布画像。

## 附：现状关键代码索引

| 功能 | 文件:行 |
|---|---|
| M5PM1 寄存器定义与 I2C 读写 | `firmware/components/stick_s3_board/stick_s3_board.c:16-56, 63-105` |
| VBAT 读取 | `firmware/components/stick_s3_board/stick_s3_board.c:267-284` |
| S0/S1/S2/S3 状态机与计时器 | `firmware/main/main.c:101-106, 408-470` |
| `enter_power_off()`（S3 进入） | `firmware/main/main.c:463` 起 |
| `note_activity()` 活动钩子 | `firmware/main/main.c:434-452` |
| 分区表（storage SPIFFS 约 1984KB） | `firmware/partitions_ota.csv` |
| BLE control/state JSON 通道 | `firmware/components/voice_ble/voice_ble.c` |
| 电源状态机设计背景 | `Doc/Plan/固件待机省电策略.md` |
| M5PM1 官方库（寄存器事实来源） | https://github.com/m5stack/M5PM1 |
