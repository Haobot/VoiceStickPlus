# StickS3 屏幕字号与电池布局调整实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 M5Stack StickS3 屏幕上加大设备号、Wi-Fi 信息和电池百分比字号，并把电池百分比移到电池图标正下方。

**Architecture:** 仅修改 `firmware/components/ui_status/ui_status.c` 中 `create_status_ui()` 的 LVGL 控件位置与字体；不改动协议、配置或桌面端代码。完成后通过 `idf.py build` 编译，再经 BLE 触发 LAN HTTP OTA 刷入 `VS-5D74` 验证。

**Tech Stack:** ESP-IDF v5.5.1, LVGL v9, C, Python http.server, VoiceStickCtl (Windows IPC OTA 工具)

---

## 文件变更清单

- **Modify** `firmware/components/ui_status/ui_status.c`
  - `create_status_ui()`：调整 `s_top_label`、`s_battery_label`、`s_imu_label`、`s_wifi_label` 的字体与位置。
- **No changes** to `ui_status.h`, Windows/macOS 桌面端, BLE 协议或配置。

---

### Task 1: 加大左上角设备号字号

**Files:**
- Modify: `firmware/components/ui_status/ui_status.c:227-233`

- [ ] **Step 1: 修改设备号标签字体与宽度**

```c
    s_top_label = lv_label_create(s_screen);
    lv_label_set_text(s_top_label, s_device_name);
    lv_obj_set_style_text_font(s_top_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_top_label, lv_color_hex(0x7f7180), 0);
    lv_label_set_long_mode(s_top_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_top_label, 72);
    lv_obj_align(s_top_label, LV_ALIGN_TOP_LEFT, 12, 2);
```

- [ ] **Step 2: 编译检查**

Run: `python scripts/idf_cli.py -c`
Expected: 编译通过，无新增警告。

---

### Task 2: 把电池百分比移到电池图标正下方并加大字号

**Files:**
- Modify: `firmware/components/ui_status/ui_status.c:155-162`

- [ ] **Step 1: 修改电池百分比标签**

```c
    s_battery_shell = lv_obj_create(screen);
    lv_obj_remove_style_all(s_battery_shell);
    lv_obj_set_size(s_battery_shell, 20, 10);
    lv_obj_set_style_radius(s_battery_shell, 3, 0);
    lv_obj_set_style_border_width(s_battery_shell, 1, 0);
    lv_obj_set_style_border_color(s_battery_shell, lv_color_hex(0x675f71), 0);
    lv_obj_set_style_bg_opa(s_battery_shell, LV_OPA_TRANSP, 0);
    lv_obj_align(s_battery_shell, LV_ALIGN_TOP_RIGHT, -8, 4);

    s_battery_fill = lv_obj_create(s_battery_shell);
    lv_obj_remove_style_all(s_battery_fill);
    lv_obj_set_size(s_battery_fill, 12, 6);
    lv_obj_set_style_radius(s_battery_fill, 2, 0);
    lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(0x67c59b), 0);
    lv_obj_align(s_battery_fill, LV_ALIGN_LEFT_MID, 2, 0);

    s_battery_tip = lv_obj_create(screen);
    lv_obj_remove_style_all(s_battery_tip);
    lv_obj_set_size(s_battery_tip, 2, 5);
    lv_obj_set_style_radius(s_battery_tip, 2, 0);
    lv_obj_set_style_bg_opa(s_battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_battery_tip, lv_color_hex(0x675f71), 0);
    lv_obj_align_to(s_battery_tip, s_battery_shell, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    s_battery_label = lv_label_create(screen);
    lv_label_set_text(s_battery_label, "--%");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x675f71), 0);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_battery_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_battery_label, 46);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(s_battery_label, s_battery_shell, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 6);
```

- [ ] **Step 2: 编译检查**

Run: `python scripts/idf_cli.py -c`
Expected: 编译通过。

---

### Task 3: IMU 行整体下移，避免与电池百分比重叠

**Files:**
- Modify: `firmware/components/ui_status/ui_status.c:247`

- [ ] **Step 1: 调整 IMU 行 Y 坐标**

```c
    lv_obj_align(s_imu_label, LV_ALIGN_TOP_MID, 0, 38);
```

- [ ] **Step 2: 编译检查**

Run: `python scripts/idf_cli.py -c`
Expected: 编译通过。

---

### Task 4: 加大 Wi-Fi 信息字号并下移

**Files:**
- Modify: `firmware/components/ui_status/ui_status.c:272-281`

- [ ] **Step 1: 修改 Wi-Fi 标签字体与位置**

```c
    s_wifi_label = lv_label_create(s_screen);
    lv_label_set_text(s_wifi_label, "");
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x3f3440), 0);
    lv_label_set_long_mode(s_wifi_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wifi_label, LCD_H_RES - 16);
    lv_obj_set_style_text_align(s_wifi_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi_label, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_add_flag(s_wifi_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wifi_label);
```

- [ ] **Step 2: 编译检查**

Run: `python scripts/idf_cli.py -c`
Expected: 编译通过。

---

### Task 5: 完整构建并验证二进制

**Files:**
- Test: `firmware/build/voice_stick.bin`

- [ ] **Step 1: 完整编译**

Run: `python scripts/idf_cli.py -c`
Expected: 输出末尾显示 `Project build complete.`, 生成 `firmware/build/voice_stick.bin`。

- [ ] **Step 2: 确认固件文件存在**

Run: `ls -lh firmware/build/voice_stick.bin`
Expected: 文件存在且大小 > 0。

---

### Task 6: 通过 BLE 触发 LAN HTTP OTA 更新到设备

**Files:**
- Test: 设备 `VS-5D74` 屏幕显示

前置条件：
- Windows 桌面端 `VoiceStick.exe` 已启动并已连接 `VS-5D74`。
- 主机 IP `192.168.3.96` 与设备在同一局域网。
- `desktop/windows/build-x64/VoiceStickCtl.exe` 存在（如不存在先运行 `build_win.bat`）。

- [ ] **Step 1: 启动本地 HTTP 服务提供固件**

Run:
```bash
cd firmware/build
python -m http.server 8000
```
Expected: 终端显示 `Serving HTTP on :: port 8000`。

- [ ] **Step 2: 计算固件 SHA256**

在另一个终端运行：
```bash
sha256sum firmware/build/voice_stick.bin
```
复制输出的 64 位十六进制哈希，例如 `aabbccdd...`。

- [ ] **Step 3: 触发 OTA pull**

Run:
```bash
desktop/windows/build-x64/VoiceStickCtl.exe ota-pull \
  --device VS-5D74 \
  --url http://192.168.3.96:8000/voice_stick.bin \
  --sha256 <上一步的哈希> \
  --wait success
```
Expected: 输出包含 OTA 进度，最终显示 `done ok=true`。

- [ ] **Step 4: 观察设备屏幕**

等待设备重启并重新连接后，检查：
1. 左上角设备号 `VS-5D74` 为 16px，清晰可读。
2. 右上角电池百分比在电池图标正下方，16px，右对齐。
3. IMU 行未与电池百分比重叠。
4. 若 Windows 端开启了 `show_wifi_info`，Wi-Fi 信息在 IMU 下方，14px，显示 SSID 与 IP 两行。
5. 状态文字（Ready/Listening/Thinking 等）未与 Wi-Fi 信息或状态图标重叠。

---

### Task 7: 提交变更

**Files:**
- `firmware/components/ui_status/ui_status.c`
- `docs/superpowers/specs/2026-06-26-sticks3-screen-text-size-and-battery-layout-design.md`
- `docs/superpowers/plans/2026-06-26-sticks3-screen-text-size-and-battery-layout-plan.md`

- [ ] **Step 1: 提交固件与文档变更**

```bash
git add firmware/components/ui_status/ui_status.c
git add docs/superpowers/specs/2026-06-26-sticks3-screen-text-size-and-battery-layout-design.md
git add docs/superpowers/plans/2026-06-26-sticks3-screen-text-size-and-battery-layout-plan.md
git commit -m "feat(firmware/ui): enlarge device ID, battery % and Wi-Fi text; move battery % below icon"
```

---

## 自我审查

**Spec coverage:**
- 设备号加大 → Task 1
- 电池百分比移位/加大 → Task 2
- IMU 下移避免重叠 → Task 3
- Wi-Fi 信息加大/下移 → Task 4
- OTA 验证 → Task 6

**Placeholder scan:** 无 TBD/TODO；所有代码块、命令、路径均具体。

**类型一致性：** 仅使用 `lv_font_montserrat_16` 与 `lv_font_montserrat_14`；`sdkconfig` 已确认启用这两个字体。

**边界情况：**
- 设备号固定 `VS-XXXX` 格式，72px 宽度在 16px 字体下可容纳，且不与电池图标重叠。
- 电池百分比 `"100%"` 在 46px 宽度、右对齐下可容纳。
- 长 SSID 在 119px 宽度下自动换行，不会裁切。
