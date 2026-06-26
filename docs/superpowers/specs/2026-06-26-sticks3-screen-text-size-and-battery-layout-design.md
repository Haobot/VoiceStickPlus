# StickS3 屏幕字号与电池布局调整设计

## 目标

在 M5Stack StickS3 的 135×240 屏幕上调整三项视觉元素：

1. 已连接 Wi-Fi 信息（SSID + IP）字号加大。
2. 左上角设备号（`VS-XXXX`）字号加大。
3. 电池图标保持在右上角，电池百分比移到电池图标正下方（第二行右侧），并加大字号。

## 用户确认的展示方案

- **布局**：电池百分比在右上角第二行、IMU 整体下移错开，避免重叠。
- **字号**：设备号 16px，电池百分比 16px，Wi-Fi 信息 14px（SSID 与 IP 各占一行完整显示）。

## 屏幕布局（最终）

| 元素 | 位置 | 字号 | 说明 |
|---|---|---|---|
| BLE 状态点 | 左上角 | 8×8 圆点 | 不变 |
| 设备号 `VS-XXXX` | 左上角、BLE 点右侧 | 16px | 宽度 72，避免与电池图标区域重叠 |
| 电池图标 | 右上角 | 20×10 图标 | 右偏移 `-8`，为下方百分比让出对齐基准 |
| 电池百分比 | 右上角、电池图标正下方 | 16px | 宽度 46，右对齐，距图标底部 6px |
| IMU（X/Y/Z） | 顶部居中，y≈38 | 16px | 整体下移，给电池百分比让出空间 |
| Wi-Fi 信息 | IMU 下方居中，y≈104 | 14px | SSID 一行、IP 一行，完整显示 |
| 状态文字 / 提示 | 屏幕中下部 | 16px | 保持不变，仍有足够空间 |

## 实现范围

仅修改 `firmware/components/ui_status/ui_status.c`：

- 在 `create_status_ui()` 中调整各控件的 `lv_obj_align`/`lv_obj_set_style_text_font`/`lv_obj_set_width`。
- 设备号 `s_top_label`：字体改为 `&lv_font_montserrat_16`，宽度改为 `72`。
- 电池图标 `s_battery_shell`：右偏移从 `-31` 改为 `-8`，让图标更靠右，避免与设备号区域重叠。
- 电池百分比 `s_battery_label`：字体改为 `&lv_font_montserrat_16`，宽度改为 `46`，对齐方式改为电池图标正下方右对齐，垂直间距 6px。
- IMU 行 `s_imu_label`：y 坐标从 `14` 下移到 `38`。
- Wi-Fi 行 `s_wifi_label`：字体改为 `&lv_font_montserrat_14`，y 坐标从 `80` 下移到 `104`。

`ui_status_set_wifi_text()`、`ui_status_set_battery()` 等外部 API 保持不变。

## 依赖

- 使用 LVGL 内置字体 `lv_font_montserrat_14` 与 `lv_font_montserrat_16`。
- 当前 `firmware/sdkconfig` 已启用 `CONFIG_LV_FONT_MONTSERRAT_14=y` 和 `CONFIG_LV_FONT_MONTSERRAT_16=y`，无需额外配置。

## 错误处理与边界情况

- 长 SSID 在 14px、119px 宽度内自动换行，IP 单独一行，基本不受影响。
- 设备号固定为 `VS-XXXX` 格式，宽度 72、16px 下可完整显示，且不与电池图标区域重叠。
- 电池百分比 16px 右对齐，宽度 46，百分比文本 `"100%"` 可完整显示。
- 所有文字颜色继续使用现有主题色（根据 resting/pairing/error 状态动态更新）。

## 验证计划

1. 本地编译：`idf.py build` 通过。
2. 通过 BLE 触发 HTTP OTA 把新固件推送到 `VS-5D74`。
3. 在设备上观察：
   - 设备号、电池百分比、Wi-Fi 信息是否加大。
   - 电池百分比是否在电池图标正下方。
   - IMU 与电池百分比、Wi-Fi 与状态文字之间是否有重叠。
   - 切换录音/识别/休眠等场景，文字颜色是否正常。

## 不在本次范围内的变更

- 不修改 Windows/macOS 桌面端代码。
- 不修改 BLE 协议、`show_wifi_info` 控制命令或配置项。
- 不调整电池图标尺寸与颜色逻辑。
- 不改动状态图标动画。
