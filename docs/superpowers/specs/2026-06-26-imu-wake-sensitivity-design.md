# IMU 拿起/晃动亮屏灵敏度设置

## 背景

当前固件在 `bmi270.c` 中用固定阈值 `BMI270_PICKUP_DELTA_THRESHOLD = 800 LSB`（约 0.2g）判定“拿起/晃动亮屏”。用户反馈灵敏度偏低，需要晃得很厉害才能亮屏，因此需要在 Windows 设置中提供 Low / Medium / High 三档灵敏度，默认 Low 保持现有行为。

## 设计决策

- 采用 **方案 B**：Windows 端把档位映射成具体阈值后通过 BLE 下发给固件，而不是只下发档位名称。
- 设置**持久化保持**：
  - Windows 层：`imu_wake_sensitivity` 写入 `config.toml`。
  - 固件层：收到阈值后写入 NVS（`voicestick` 命名空间，`pickup_thr` 键），设备重启后恢复。

## 阈值映射（初值）

| 档位 | 下发阈值（LSB） | 约等于 g | 备注 |
|------|----------------|----------|------|
| Low    | 800 | ≈0.20g | 当前灵敏度，保持不变 |
| Medium | 500 | ≈0.12g | 中等灵敏度 |
| High   | 250 | ≈0.06g | 较高灵敏度，轻微晃动即亮屏 |

> 这些数值为初值，OTA 刷机实测后可微调。

## Windows 端改动

### 配置层

- `app_config.h` 新增枚举：
  ```cpp
  enum class ImuWakeSensitivity {
      kLow,
      kMedium,
      kHigh,
  };
  ```
- `AppConfig` 新增字段：
  ```cpp
  ImuWakeSensitivity imu_wake_sensitivity = ImuWakeSensitivity::kLow;
  ```
- `config.toml` 保存为字符串：
  ```toml
  imu_wake_sensitivity = "low"
  ```
- 新增辅助函数：
  - `std::string ImuWakeSensitivityName(ImuWakeSensitivity)`
  - `ImuWakeSensitivity ImuWakeSensitivityFromName(std::string_view)`
  - `std::string ImuWakeSensitivityDisplayName(ImuWakeSensitivity)`

### BLE 协议层

- `BleProtocol` 新增：
  ```cpp
  static ByteVector ImuWakeSensitivityPayload(int threshold_lsb);
  ```
  生成 JSON：
  ```json
  {"event":"imu_wake_sensitivity","threshold":500}
  ```
- `BleCentral` 纯虚接口新增：
  ```cpp
  virtual void SendImuWakeSensitivity(int threshold_lsb,
                                      const std::optional<std::string>& device_id) = 0;
  ```
- `BleCentralWin` 实现该接口，向已就绪设备广播或单发。

### 协调器

- `VoiceStickCoordinator` 在 BLE 连接成功回调和 `UpdateConfig` 中，根据 `config_.imu_wake_sensitivity` 换算阈值并下发。

### UI 层

- `SettingsDialog` 增加标签 + ComboBox：
  - 英文：Wake Sensitivity
  - 中文：拿起灵敏度
  - 选项：Low / Medium / High（中文：低 / 中 / 高）
- `localization.h/.cc` 新增：
  - `kSettingsImuWakeSensitivity`
  - `kSettingsImuWakeSensitivityLow`
  - `kSettingsImuWakeSensitivityMedium`
  - `kSettingsImuWakeSensitivityHigh`

## 固件端改动

### bmi270 驱动

- 把硬编码宏 `BMI270_PICKUP_DELTA_THRESHOLD` 替换为运行时可写变量 `s_pickup_threshold`。
- 默认值 800 LSB。
- 新增 API：
  ```c
  void bmi270_set_pickup_threshold(float threshold_lsb);
  float bmi270_get_pickup_threshold(void);
  ```

### main.c

- 新增 NVS 读写函数（命名空间 `voicestick`，键 `pickup_thr`，类型 `int32_t`）：
  - `load_pickup_threshold_from_nvs()`：启动时调用，若不存在则使用默认值。
  - `save_pickup_threshold_to_nvs(int32_t threshold)`：收到新阈值后调用。
- 在 `ble_control_cb` 中解析 `imu_wake_sensitivity` 事件：
  - 读取 `threshold` 字段。
  - 校验范围 50 ~ 2000 LSB，越界则忽略。
  - 调用 `bmi270_set_pickup_threshold` 并写入 NVS。
  - 打印日志：`imu_wake_sensitivity threshold=500`。

## BLE 协议文档

在 `Doc/Ref/protocol.md` 的 Control Event 表格中新增：

| Event | Field | Direction | Meaning |
|-------|-------|-----------|---------|
| `imu_wake_sensitivity` | `threshold`: integer (LSB) | Windows -> StickS3 | 设置拿起/晃动亮屏灵敏度阈值。建议范围 50~2000 LSB。 |

## 验证计划

1. Windows 端 `build_win.bat` 编译通过。
2. 固件 `idf.py build` 编译通过。
3. 使用 BLE 触发的 HTTP OTA 把新固件刷到 `VS-5D74`。
4. 在 Settings 里切换 Low / Medium / High，观察设备从息屏/变暗状态被晃亮的难易程度。
5. 重启设备（不重启 Windows 端），确认固件从 NVS 恢复阈值，灵敏度不变。

## 依赖与风险

- 依赖 `voice_ble_init` 已完成 `nvs_flash_init`（当前已满足）。
- 阈值过小可能导致误触发，High 档（250 LSB）需要实测确认。
- 固件端新增 NVS 键，回滚到旧固件后旧固件会忽略该键，不影响功能。
