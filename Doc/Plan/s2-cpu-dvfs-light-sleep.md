# S2 息屏态 CPU 动态调频（DVFS）+ light sleep 方案

> 状态：设计稿，待评审（2026-10-17）
> 范围：仅固件 `firmware/`（sdkconfig.defaults + main.c + ui_status.c）
> 前置文档：`Doc/Plan/固件待机省电策略.md`（S0–S3 状态机设计）、`Doc/Plan/power-mode-energy-profiling.md`（功耗记账）
> 关键硬件事实：ESP32-S3 @ M5Stack StickS3，电池约 200mAh；`CONFIG_BT_CTRL_MODEM_SLEEP=y` 与 `CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y` 已开启（sdkconfig.defaults:41-42）

## 1. 背景与问题

当前固件为保 USB-Serial-JTAG 调试，全局锁死 CPU：`min_freq = max_freq = 240MHz`、`light_sleep_enable = false`（main.c:2672-2683）。由此 S0/S1/S2 三态 CPU 全部满频空转，息屏（S2）只关背光、BLE 保连，整机估算 ~45mA（power_model.json 经验值，待标定）——200mAh 电池在 S2 也只能撑 ~4.4 小时，"息屏"并不省电。

**关键观察**：锁频是为了 COM 口调试，但**电池供电时设备上根本没有 COM 口可挂**——锁频的代价在电池态是白付的；而 USB 供电时电池不在放电，锁不锁频无所谓。因此正确做法是**按「供电态 × 屏幕态」动态切换 PM 配置**，而不是全局一刀切。

## 2. 目标 / 非目标

目标：

- 默认（S0/S1、USB 供电任意态）：维持现状 240MHz 锁定——用户要求的"平常锁 240MHz"不变，调试体验不回归。
- 息屏（S2）且电池供电：CPU 自动降频运行（DFS 在 40↔240MHz 间按需浮动）+ 允许 light sleep；任何活动（按键/拿起/编码器/BLE 事件/录音/OTA）自动切回 240MHz 锁定档。
- BLE 连接保持不断（唤醒即用的体验不回退）；按键亮屏延迟可感知但不劣化。

非目标（YAGNI）：

- 不动 S3（deep sleep / M5PM1 关机路径完全不变）。
- 不改 BLE 协议、不改桌面端。
- S1（暗屏 60s 过渡态）不做特殊降频——时长太短，收益有限。
- BLE slow interval 的 S2 自动切换不在本期（二期配套，见第 6 节风险表）。

## 3. 总体设计：两档 PM 配置运行期切换

`esp_pm_configure` 支持运行期重复调用（IDF 官方语义，用于切换 PM 档）。定义两档：

| 配置档 | min_freq | max_freq | light_sleep | 使用场景 |
|---|---|---|---|---|
| **PERF**（现状） | 240MHz | 240MHz | 关 | S0/S1；USB 供电任意屏幕态；录音/OTA 期间 |
| **SAVE** | 40MHz（XTAL） | 240MHz | 开 | S2 且电池供电且非录音/OTA |

SAVE 档下由 esp_pm 自动管理：BLE connection event、esp_timer 到期时把频率拉到所需档位；空闲时降 40MHz 并经 tickless idle 进 light sleep。BT modem sleep + 主 XTAL 保活（均已开启）保证连接事件不丢、链路不断。

**接入点（全部复用现有状态机转移点，纯增量）**：

| 动作 | 位置 | 行为 |
|---|---|---|
| 进 SAVE | `display_off_timer_cb`（main.c:1718，S2 入口） | `!is_external_powered()` 时切 SAVE |
| 回 PERF | `note_activity()`（main.c:478） | 已在 `s_screen_off` 分支开头恢复背光——在其内联先回 PERF；覆盖按键/拿起/编码器/BLE 连接/OTA begin/end 全部活动路径 |
| 回 PERF（保险） | `apply_app_ui_state("recording")`（main.c:1407）与 `start_recording` 路径 | host click-to-talk 不经过 `note_activity`，显式 `ensure_perf_config()` |
| 回 PERF（VBUS） | PMIC IRQ / 电池刷新路径（main.c:1538-1546） | `s_pm_save_active && is_external_powered()` 时回 PERF；USB 拔出后在同一路径双向判断补进 SAVE |
| 录音/OTA 双保险 | `acquire_recording_pm_locks` / `acquire_ota_pm_locks` | 追加 `ESP_PM_NO_LIGHT_SLEEP` 锁（防 SAVE 档下 light sleep 切入打断 I2S/Opus 时序） |

配置切换函数：

```c
static bool s_pm_save_active;

static void apply_pm_config_perf(void)  // min=max=240, light_sleep=false
{
    if (!s_pm_save_active) return;
    s_pm_save_active = false;
    esp_pm_configure(&(esp_pm_config_t){
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = false,
    });
    ESP_LOGI(TAG, "pm config -> PERF (240MHz lock)");
}

static void apply_pm_config_save(void)  // min=40, max=240, light_sleep=true
{
    if (s_pm_save_active) return;
    s_pm_save_active = true;
    esp_pm_configure(&(esp_pm_config_t){
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = true,
    });
    ESP_LOGI(TAG, "pm config -> SAVE (40-240MHz DFS + light sleep)");
}
```

所有切换都在 app_event_task 单线程上下文执行（定时器回调投队列），无竞态。

## 4. 详细设计

### 4.1 sdkconfig：放开 light sleep GPIO 唤醒

`CONFIG_PM_SLP_DISABLE_GPIO` 现状为 y（main.c:2678 注释：该选项使按键 gpio_wakeup 失效），需置 `n`：

```c
// sdkconfig.defaults 追加
CONFIG_PM_SLP_DISABLE_GPIO=n
```

GPIO11（主键）/GPIO12（侧键）在 ESP32-S3 RTC GPIO（0–21）范围内，可作 light sleep 唤醒源。代价：light sleep 期间 RTC IO 域保持供电，µA 级，可忽略。

### 4.2 按键唤醒链路

- **物理键/侧键**：iot_button 已按 `enable_power_save = true` 注册（main.c:1627-1637），button 组件内部走 gpio 唤醒路径。S2 下按键 → light sleep 唤醒 → `note_activity()` → 回 PERF + 亮屏。组件版本 ^4.1.6 的具体行为以真机验证为准。
- **编码器按钮**：I2C 外设不能作唤醒源，靠轮询定时器唤醒后检测（见 4.3）。

### 4.3 S2 轮询降频（不降级 light sleep 进不去）

`CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3`：需连续 30ms 无任务才进 light sleep。当前周期定时器全是阻断项，S2 下统一降频，退出 S2 恢复原值：

| 定时器 | 现状 | S2 下 | 理由 |
|---|---|---|---|
| LVGL tick（ui_status.c:43） | 10ms | 50ms | 纯维持 tick；S2 无动画；50ms 对唤醒后的第一帧渲染无感 |
| 编码器轮询（main.c:77） | 10ms | 50ms | S2 下旋转仍会唤醒上报（保留现状行为），50ms 检测无感；兼作编码器按钮的"唤醒源" |
| IMU 朝向轮询（main.c:70） | 200ms | 500ms | 朝向自动旋转非关键路径 |
| 拿起轮询（main.c:50） | 500ms | 1000ms | 拿起亮屏 1s 延迟可接受 |
| 电池/power_log 采样 | 60s | 不变 | 已足够低 |

实现：S2 进入/退出时对对应 `esp_timer_stop()` + `esp_timer_start_periodic()` 重启周期。常量沿用现有 `*_INTERVAL_US` 风格新增 `*_S2_INTERVAL_US`。

### 4.4 录音/OTA 保护

- 录音必经 `acquire_recording_pm_locks`（s_cpu_freq_lock 提 240MHz）；SAVE 档下 light sleep 仍可能切入——故该锁函数追加 `ESP_PM_NO_LIGHT_SLEEP` 锁，release 时对称释放。OTA 同。
- 正常路径录音前已回 PERF（按键路径经 `note_activity`，host 路径经 4.4 的显式 ensure），NO_LIGHT_SLEEP 是双保险。

### 4.5 VBUS 动态处理

PMIC IRQ（GPIO13）在充电/VBUS 状态变化时投递 `APP_EVENT_POWER_IRQ` → `update_battery_status()`。在该路径补双向判断：

- SAVE 中且 `is_external_powered()` 变 true → 回 PERF（USB 态调试行为与今天一致）。
- PERF 中、处于 S2 且外部供电消失 → 进 SAVE（如电池/USB 供电切换瞬间）。

## 5. 预期收益与验证

- S2 电流目标：45mA → **3–8mA**（40MHz 基底 + light sleep 占空 + BLE 100–400ms 间隔唤醒）。S2 驻留为主的场景续航从"小时级"提升到"天级"。
- 验证手段（复用现有工具链）：改前先用 `scripts/e2e_test/power_log_dump.py` 导基线跑 `power_report.py`；改后同流程对比 S2 段 VBAT 回归斜率，把实测电流回填 `power_model.json` 的 `S2_SCREEN_OFF`。
- 唤醒体验：按键亮屏延迟目标 <0.5s；BLE 全程保连无重连。

## 6. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| light sleep 下 I2S/Opus 时序受扰 | 录音破音/丢帧 | 录音双保险回 PERF + NO_LIGHT_SLEEP 锁；真机录音回归（含 host click-to-talk） |
| 按键 light sleep 唤醒不生效（组件/symbol 差异） | 息屏按键无响应 | 真机验证；**退化预案**：SAVE 档退回"只开 DFS、不开 light_sleep_enable"——仍保留 40MHz 降频的 4–5× 收益 |
| `esp_pm_configure` 运行期热切换异常 | 配置不生效/异常 | 同退化预案：可改为 boot 常驻 SAVE 档 + PERF 场景用 CPU_FREQ_MAX/NO_LIGHT_SLEEP 锁表达（语义等价，锁开销略高） |
| USB 插入瞬间 COM 口行为 | 调试体验回归 | USB 态永远 PERF，不切换；VBUS 检测经 PMIC IRQ，响应足够快 |
| LVGL 50ms tick 导致回 S0 首帧慢 | 亮屏瞬间卡顿 | 退出 S2 即恢复 10ms；50ms tick 本身仍在跑 handler，只是计数粒度粗，无实际卡顿 |
| BLE connection event 频率高吞噬收益 | 省电打折 | 二期配套：S2 自动请求 slow interval（API 已存在）；本期即便 fast 7.5ms，40MHz+light sleep 仍有显著收益 |
| S2 下旋转编码器 50ms 检测引入的延迟 | 手感变化 | 旋转上报延迟 +≤50ms，S2 场景可接受；S0 不变 |

## 7. 实施步骤

1. `sdkconfig.defaults` 加 `CONFIG_PM_SLP_DISABLE_GPIO=n`；`idf.py build` 通过。
2. main.c：`apply_pm_config_perf/save` + `s_pm_save_active`；接入 `display_off_timer_cb`、`note_activity`、`POWER_IRQ`/电池刷新、`apply_app_ui_state("recording")`。
3. 4.3 轮询降频表落地（S2 进入/退出切换定时器周期）。
4. `acquire_recording_pm_locks` / `acquire_ota_pm_locks` 加 NO_LIGHT_SLEEP 锁（需新建对应 handle）。
5. 真机验证清单（第 8 节）全过；power_log 改前基线 / 改后对比；实测 S2 电流回填 `power_model.json`，结论沉淀到 `Doc/Expe/`。
6. 提交（固件常规 commit 流程，无协议变更，无需同步 protocol.md）。

## 8. 测试计划（真机）

- [ ] USB 供电态：COM 口枚举、日志、串口烧录（idf_cli）完全正常（行为不变）。
- [ ] 电池态息屏：日志出现 `pm config -> SAVE`；期间 BLE 连接保持 ≥30min 不断连。
- [ ] S2 按前键/侧键：light sleep 唤醒、亮屏回 S0（目标 <0.5s）。
- [ ] S2 按编码器按钮 / 旋转：检测并亮屏上报（附加延迟 ≤50ms）。
- [ ] S2 拿起：亮屏（≤1s 延迟）。
- [ ] 录音全流程（按键 + host click-to-talk 两路径）：无破音、无丢帧、无 watchdog。
- [ ] BLE OTA 全流程。
- [ ] S2→S3 关机、按键唤醒冷启动：链路正常。
- [ ] `power_report.py` 对比：S2 段 VBAT 斜率显著下降；回填标定值。

## 附：现状关键代码索引

| 功能 | 文件：行 |
|---|---|
| 全局锁 240MHz / 关 light sleep（要改的配置点） | `firmware/main/main.c:2672-2683` |
| S2 进入（display_off_timer_cb） | `firmware/main/main.c:1718-1733` |
| 活动钩子（note_activity） | `firmware/main/main.c:478-498` |
| host recording ui_state（不经 note_activity 的路径） | `firmware/main/main.c:1407-1411` |
| PMIC IRQ / 电池刷新（VBUS 检测点） | `firmware/main/main.c:1538-1546` |
| 录音/OTA 现有 CPU 锁 | `firmware/main/main.c:352-418` |
| 按键 power-save 注册 | `firmware/main/main.c:1627-1637` |
| 轮询周期常量（10/200/500ms） | `firmware/main/main.c:50,70,77` |
| LVGL tick 10ms | `firmware/components/ui_status/ui_status.c:43` |
| BT modem sleep / main XTAL 保活 | `firmware/sdkconfig.defaults:41-42` |
