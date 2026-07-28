# 编码器设置项（MiniEncoderC 可配置化）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Windows 设置对话框新增「编码器」一节，暴露旋转开关/翻转/自定义按键、录音灯颜色（固件 NVS 持久化）、单击/双击动作（录音或自定义按键），固件按键事件补 `source:"encoder"` 标签供桌面端分流。

**Architecture:** 全部手势语义在桌面端执行（方案 A）。固件只做三件事：按键事件加可选 `source` 字段、LED 颜色存 NVS 并在录音亮灯时使用、录音门控（`press_action=key` 时编码器按下只发事件不起录音）。桌面端新增 core 级 `key_spec` 按键解析器与 `InputInjector::SendKeyCombo`，协调器按 `source` 分流到可配置动作表，设置对话框走既有 `LayoutEntry` 声明式排版。双击 `recording` 复用固件已有 `remote_button_down/up` 通道（固件零改动）。

**Tech Stack:** 固件 C（ESP-IDF v5.5.1，esp32s3）/ Windows C++20（voicestick_core + VoiceStickApp，CMake+Ninja+MSVC）/ 测试：`desktop/windows/tests/core_tests.cc`（assert 风格）。

**Spec:** `docs/superpowers/specs/2026-07-28-encoder-settings-design.md`（已批准）

---

## 工作环境（所有 Task 共用）

- worktree 根目录：`C:/Dev/FFE/George/voicestick/.worktrees/encoder-settings`，分支 `feat/encoder-settings`。所有路径相对于此目录。
- **Windows 构建**（首次先复制辅助脚本）：
  ```bash
  cp ../mini-encoder-c/_wt_build.bat ../mini-encoder-c/_wt_ctest.bat .
  cmd //c "_wt_build.bat"     # vcvars + configure(按需) + build + ctest -R voicestick_windows_tests
  cmd //c "_wt_ctest.bat"     # 全部 CTest
  ```
- **固件构建**：`python scripts/idf_cli.py -c`（worktree 根目录）。
- **git 提交纪律**：`desktop/windows/` 被 .gitignore 忽略，提交 Windows 文件必须 `git add -f <file>`。固件/文档文件正常 `git add`。
- 核心测试注册：新 `Test...()` 函数必须加入 `desktop/windows/tests/core_tests.cc` 的 `main()`（约 5752 行起，按字母/主题插入），否则不会运行。
- 固件无单元测试，验证 = `idf_cli.py -c` 编译通过 + Task 12 真机清单。

---

### Task 1: 固件按键事件补 `source` 标签 + 协议文档

**Files:**
- Modify: `firmware/components/voice_ble/include/voice_ble.h:52-57`
- Modify: `firmware/components/voice_ble/voice_ble.c:1038-1098`
- Modify: `firmware/main/main.c`（静态变量区、`handle_primary_down` 入口、15 处 primary + 2 处 secondary 发送调用点）
- Modify: `Doc/Ref/protocol.md`

**背景：** 桌面端需要区分编码器按钮与物理主键（两者在固件侧都上报 `"button":"primary"`）。约定：编码器来源的按键事件带 `"source":"encoder"`；其它来源省略该字段（旧桌面忽略未知字段，零迁移成本）。

- [ ] **Step 1: voice_ble.h 四个发送函数加 `source` 尾参**

把 `voice_ble.h:52-57` 的四个声明改为：

```c
// source 为事件来源标签（如 "encoder"），NULL 时省略该字段（物理键/远程键行为不变）。
esp_err_t voice_ble_send_button_down(const char *button, uint32_t session_id,
                                     const char *source);
esp_err_t voice_ble_send_button_up(const char *button, uint32_t duration_ms,
                                   uint32_t session_id, const char *source);
esp_err_t voice_ble_send_button_click(const char *button, uint32_t duration_ms,
                                      uint32_t session_id, const char *source);
esp_err_t voice_ble_send_button_double_click(const char *button, const char *source);
```

- [ ] **Step 2: voice_ble.c 实现拼 JSON 时追加 source 字段**

`voice_ble.c:1038-1098` 四个函数整体替换为（注意 JSON buffer 扩容：down 96→128、up/click 128→160、double_click 64→96；source 字段拼在末尾）：

```c
esp_err_t voice_ble_send_button_down(const char *button, uint32_t session_id,
                                     const char *source)
{
    char json[128];
    char source_field[40] = "";
    if (source) {
        snprintf(source_field, sizeof(source_field), ",\"source\":\"%s\"", source);
    }
    if (session_id > 0) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_down\",\"button\":\"%s\",\"session_id\":%" PRIu32 "%s}",
                 button, session_id, source_field);
    } else {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_down\",\"button\":\"%s\"%s}", button, source_field);
    }
    return send_state_json(json);
}

esp_err_t voice_ble_send_button_up(const char *button, uint32_t duration_ms,
                                   uint32_t session_id, const char *source)
{
    char json[160];
    char source_field[40] = "";
    if (source) {
        snprintf(source_field, sizeof(source_field), ",\"source\":\"%s\"", source);
    }
    if (session_id > 0) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_up\",\"button\":\"%s\","
                 "\"duration_ms\":%" PRIu32 ",\"session_id\":%" PRIu32 "%s}",
                 button, duration_ms, session_id, source_field);
    } else {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_up\",\"button\":\"%s\",\"duration_ms\":%" PRIu32 "%s}",
                 button, duration_ms, source_field);
    }
    return send_state_json(json);
}

esp_err_t voice_ble_send_button_click(const char *button, uint32_t duration_ms,
                                      uint32_t session_id, const char *source)
{
    char json[160];
    char source_field[40] = "";
    if (source) {
        snprintf(source_field, sizeof(source_field), ",\"source\":\"%s\"", source);
    }
    if (session_id > 0) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_click\",\"button\":\"%s\","
                 "\"duration_ms\":%" PRIu32 ",\"session_id\":%" PRIu32 "%s}",
                 button, duration_ms, session_id, source_field);
    } else if (duration_ms > 0) {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_click\",\"button\":\"%s\",\"duration_ms\":%" PRIu32 "%s}",
                 button, duration_ms, source_field);
    } else {
        snprintf(json, sizeof(json),
                 "{\"event\":\"button_click\",\"button\":\"%s\"%s}", button, source_field);
    }
    ESP_LOGI(TAG, "button click button=%s session=%" PRIu32 " duration_ms=%" PRIu32 " source=%s",
             button, session_id, duration_ms, source ? source : "-");
    return send_state_json(json);
}

esp_err_t voice_ble_send_button_double_click(const char *button, const char *source)
{
    char json[96];
    char source_field[40] = "";
    if (source) {
        snprintf(source_field, sizeof(source_field), ",\"source\":\"%s\"", source);
    }
    snprintf(json, sizeof(json),
             "{\"event\":\"button_double_click\",\"button\":\"%s\"%s}", button, source_field);
    ESP_LOGI(TAG, "button double_click button=%s source=%s", button, source ? source : "-");
    return send_state_json(json);
}
```

- [ ] **Step 3: main.c 记录最近主键按下来源 + source 标签 helper**

在 `main.c` 静态变量区（`s_primary_owner` 声明之后，约 :167）加：

```c
// 最近一次主键按下（任意来源）的输入源：button_up/click/double_click 发送时据此
// 补 source 标签（编码器事件带 "encoder"，其它来源省略）。
static app_input_source_t s_primary_press_source = APP_INPUT_SOURCE_PHYSICAL;
```

在 `primary_owner_from_source()`（:914-919）之后加 helper：

```c
// 主键事件的 source 标签：编码器返回 "encoder"，其它来源返回 NULL（省略字段）。
static const char *primary_button_source_tag(void)
{
    return s_primary_press_source == APP_INPUT_SOURCE_ENCODER ? "encoder" : NULL;
}
```

在 `handle_primary_down`（:966）入口（`note_activity();` 之前）加一行：

```c
    s_primary_press_source = source;
```

- [ ] **Step 4: main.c 全部按键发送调用点补尾参**

机械替换（primary 调用点传 `primary_button_source_tag()`，secondary 传 `NULL`）：

| 行号(约) | 现状 | 改为尾参 |
|---|---|---|
| :930 | `send_button_click("secondary", s_side_pending_duration_ms, 0)` | `, NULL` |
| :945 | `send_button_double_click("secondary")` | `, NULL` |
| :990 | `send_button_double_click("primary")` | `, primary_button_source_tag()` |
| :1004 | 同上 | 同上 |
| :1027-1028 | `send_button_click("primary", primary_duration_ms, s_primary_session_id)` | `, primary_button_source_tag()` |
| :1040 | `send_button_click("primary", 0, 0)` | `, primary_button_source_tag()` |
| :1059 | `send_button_down("primary", s_primary_session_id)` | `, primary_button_source_tag()` |
| :1094 | `send_button_double_click("primary")` | `, primary_button_source_tag()` |
| :1124-1125 | `send_button_click(...)` / `send_button_down(...)` 三元 | 两处都加 `, primary_button_source_tag()` |
| :1146 | `send_button_click("primary", duration_ms, 0)`（体感） | `, primary_button_source_tag()` |
| :1217-1218 | `send_button_up("primary", primary_duration_ms, s_primary_session_id)` | `, primary_button_source_tag()` |
| :1388-1389 | `send_button_up("primary", elapsed_button_ms(...), session_id)`（OTA） | `, primary_button_source_tag()` |
| :1581 | `send_button_click("primary", 0, s_primary_session_id)` | `, primary_button_source_tag()` |
| :1622 | `send_button_down("primary", s_primary_session_id)` | `, primary_button_source_tag()` |
| :1644 | 同上 | 同上 |
| :1672 | `send_button_click("primary", s_pending_button_up_duration_ms, 0)` | `, primary_button_source_tag()` |

- [ ] **Step 5: 编译验证**

Run: `python scripts/idf_cli.py -c`
Expected: build 成功，无 warning 新增。

- [ ] **Step 6: 协议文档**

`Doc/Ref/protocol.md` 按键事件（`button_down`/`button_up`/`button_click`/`button_double_click`）小节各补一句：可选字段 `source`，编码器按钮（MiniEncoderC Hat）事件为 `"encoder"`，物理键与远程键省略该字段；并给一条示例：

```json
{"event":"button_click","button":"primary","duration_ms":131,"source":"encoder"}
```

- [ ] **Step 7: Commit**

```bash
git add firmware/components/voice_ble/include/voice_ble.h firmware/components/voice_ble/voice_ble.c firmware/main/main.c Doc/Ref/protocol.md
git commit -m "feat(firmware): 编码器按键事件补 source 标签"
```

---

### Task 2: 固件录音灯颜色（NVS 持久化 + control_rx 下发）

**Files:**
- Modify: `firmware/main/main.c`（静态变量、预设表函数、ble_control_cb、start_recording、NVS load/save、app_main）

**背景：** 录音亮灯当前硬编码红色（`main.c:603` `mini_encoder_c_set_led(255, 0, 0)`）。新增：桌面端经 control_rx 下发颜色名 → 预设表映射 RGB → 存 NVS `enc_led`（i32 0xRRGGBB）→ boot 加载。灭灯路径（:623/:1361）不动；`off` = 存 0x000000（录音也不亮）。

- [ ] **Step 1: 静态变量 + 预设表 + 前置声明**

在 `main.c` 静态变量区（`s_primary_press_source` 附近）加：

```c
// 编码器录音灯颜色（0xRRGGBB，0=off）：桌面端经 control_rx 下发颜色名，NVS 持久化。
static uint32_t s_encoder_led_rgb = 0xFF0000u;  // 默认红
```

在前置声明区（:254-257 附近）加：

```c
static void load_encoder_settings_from_nvs(void);
static void save_encoder_settings_to_nvs(void);
```

在 `ble_control_cb` 之前加预设表函数：

```c
// 颜色名 → 0xRRGGBB 预设表。未知名返回 false（调用方忽略并保持当前值）。
static bool encoder_led_rgb_from_name(const char *name, uint32_t *rgb_out)
{
    struct { const char *name; uint32_t rgb; } presets[] = {
        {"red", 0xFF0000u}, {"green", 0x00FF00u}, {"blue", 0x0000FFu},
        {"yellow", 0xFFFF00u}, {"purple", 0xFF00FFu}, {"cyan", 0x00FFFFu},
        {"white", 0xFFFFFFu}, {"off", 0x000000u},
    };
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); ++i) {
        if (strcmp(name, presets[i].name) == 0) {
            *rgb_out = presets[i].rgb;
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 2: ble_control_cb 加 `encoder_led_color` 分支**

在 `main.c:848`（`tap_sensitivity` 分支）之前插入：

```c
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "encoder_led_color") == 0) {
        const cJSON *color_item = cJSON_GetObjectItemCaseSensitive(root, "color");
        uint32_t rgb = 0;
        if (cJSON_IsString(color_item) &&
            encoder_led_rgb_from_name(color_item->valuestring, &rgb)) {
            s_encoder_led_rgb = rgb;
            save_encoder_settings_to_nvs();
            ESP_LOGI(TAG, "encoder_led_color %s -> 0x%06" PRIX32,
                     color_item->valuestring, s_encoder_led_rgb);
        } else {
            ESP_LOGW(TAG, "unknown encoder_led_color ignored");
        }
    }
```

（注意保持与前后 `else if` 链的大括号衔接：新分支以 `} else if` 开头、以 `}` 结尾。）

- [ ] **Step 3: start_recording 用存储颜色亮灯**

`main.c:601-604` 改为：

```c
    s_recording = true;
    // 录音期间编码器 LED 亮灯（颜色 NVS 可配，0=off 不亮）；LED 写失败静默忽略。
    if (mini_encoder_c_present() && s_encoder_led_rgb != 0) {
        (void)mini_encoder_c_set_led((s_encoder_led_rgb >> 16) & 0xFF,
                                     (s_encoder_led_rgb >> 8) & 0xFF,
                                     s_encoder_led_rgb & 0xFF);
    }
```

- [ ] **Step 4: NVS load/save 函数**

在 `load_tap_settings_from_nvs()`（:2202-2256）之后加：

```c
// 编码器设置：enc_led（i32，0xRRGGBB，默认红）。Task 3 会在此函数扩展 enc_rec_gate。
static void load_encoder_settings_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("voicestick", NVS_READONLY, &handle);
    int32_t led = (int32_t)0xFF0000;
    if (err == ESP_OK) {
        esp_err_t e = nvs_get_i32(handle, "enc_led", &led);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load encoder led failed: %s", esp_err_to_name(e));
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open nvs namespace failed: %s", esp_err_to_name(err));
    }
    s_encoder_led_rgb = ((uint32_t)led) & 0xFFFFFFu;
    ESP_LOGI(TAG, "encoder settings loaded from nvs: led=0x%06" PRIX32, s_encoder_led_rgb);
}

static void save_encoder_settings_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("voicestick", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open nvs namespace failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_i32(handle, "enc_led", (int32_t)s_encoder_led_rgb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save encoder led failed: %s", esp_err_to_name(err));
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit encoder settings failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "encoder settings saved to nvs: led=0x%06" PRIX32, s_encoder_led_rgb);
    }
    nvs_close(handle);
}
```

在 `app_main`（:2354 `load_tap_settings_from_nvs();` 之后）加：

```c
    load_encoder_settings_from_nvs();
```

- [ ] **Step 5: 编译验证**

Run: `python scripts/idf_cli.py -c`
Expected: build 成功。

- [ ] **Step 6: Commit**

```bash
git add firmware/main/main.c
git commit -m "feat(firmware): 编码器录音灯颜色可配（NVS 持久化）"
```

---

### Task 3: 固件编码器录音门控（`encoder_recording_gate`）

**Files:**
- Modify: `firmware/main/main.c`（静态变量、ble_control_cb、handle_primary_down/up、NVS load/save）

**背景（架构约束补丁 1，spec §5a）：** 编码器按钮与物理主键共用录音触发（hold 300ms 阈值自动开播）。桌面端把单击配为自定义按键（`press_action=key`）时，必须阻止固件侧录音，否则长按旋钮会空播音频。桌面端不新增配置键，从 `encoder_press_action` 派生下发 `{"event":"encoder_recording_gate","enabled":false|true}`。门控关闭时：编码器按下只发按键事件（单击 click / 双击 double_click），不启动音频会话、不亮录音灯；物理主键不受影响。

- [ ] **Step 1: 静态变量**

在 `s_encoder_led_rgb` 之后加：

```c
// 编码器录音门控：false 时编码器按下只发按键事件不启动录音（桌面端单击=自定义按键）。
static bool s_encoder_recording_gate = true;
```

- [ ] **Step 2: ble_control_cb 加 `encoder_recording_gate` 分支**

在 Task 2 的 `encoder_led_color` 分支之后插入：

```c
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "encoder_recording_gate") == 0 &&
               cJSON_IsBool(enabled)) {
        s_encoder_recording_gate = cJSON_IsTrue(enabled);
        save_encoder_settings_to_nvs();
        ESP_LOGI(TAG, "encoder_recording_gate %s",
                 s_encoder_recording_gate ? "enabled" : "disabled");
    }
```

- [ ] **Step 3: handle_primary_down 门控分支**

在 `main.c:1014`（`is_local_primary_source` 双击/click_to_talk 检测块的收尾 `}` 之后、:1016 `if (s_interaction_mode == INTERACTION_MODE_HOLD_TO_TALK && s_recording)` 之前）插入：

```c
    // 编码器录音门控关闭：按下只走按键事件链路（双击检测已在上方完成；
    // 松开时由 handle_primary_up 的门控分支进双击窗口补发 button_click），
    // 不启动任何音频会话。物理主键不受影响。
    // !s_recording 条件：门控运行中从开切关时若录音已在进行，放行正常停录路径。
    if (source == APP_INPUT_SOURCE_ENCODER && !s_encoder_recording_gate && !s_recording) {
        s_primary_down_us = esp_timer_get_time();
        return;
    }
```

注意：此处**不设置** `s_primary_owner`（避免门控关闭的编码器按住期间把物理主键的按下误判为 owner 冲突；门控分支的 up 路径也不读 owner）。

- [ ] **Step 4: handle_primary_up 门控分支**

在 `main.c:1158`（双击第二击释放忽略块的收尾 `}` 之后、:1160 `if (s_interaction_mode == INTERACTION_MODE_CLICK_TO_TALK)` 之前）插入：

```c
    // 门控关闭时的编码器释放：统一按短按处理进双击窗口（窗口超时补发 button_click，
    // 窗口内再按发 button_double_click），不产生 button_up，不涉及录音。
    if (source == APP_INPUT_SOURCE_ENCODER && !s_encoder_recording_gate && !s_recording) {
        if (s_primary_down_us == 0) {
            return;  // 无配对按下（如门控运行中切换），忽略
        }
        const uint32_t duration_ms = elapsed_button_ms(s_primary_down_us);
        ESP_LOGI(TAG, "encoder button up (recording gate off), double-click window (%" PRIu32 " ms)",
                 duration_ms);
        s_double_click_pending = true;
        s_pending_button_up_duration_ms = duration_ms;
        (void)esp_timer_start_once(s_double_click_timer,
                                   DOUBLE_CLICK_WINDOW_MS * 1000ULL);
        return;
    }
```

- [ ] **Step 5: NVS 扩展 enc_rec_gate**

`load_encoder_settings_from_nvs()`（Task 2 新建）改为：

```c
static void load_encoder_settings_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("voicestick", NVS_READONLY, &handle);
    int32_t led = (int32_t)0xFF0000;
    int32_t gate = 1;  // 默认开（录音语义）
    if (err == ESP_OK) {
        esp_err_t e = nvs_get_i32(handle, "enc_led", &led);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load encoder led failed: %s", esp_err_to_name(e));
        }
        e = nvs_get_i32(handle, "enc_rec_gate", &gate);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load encoder recording gate failed: %s", esp_err_to_name(e));
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "open nvs namespace failed: %s", esp_err_to_name(err));
    }
    s_encoder_led_rgb = ((uint32_t)led) & 0xFFFFFFu;
    s_encoder_recording_gate = (gate != 0);
    ESP_LOGI(TAG, "encoder settings loaded from nvs: led=0x%06" PRIX32 " gate=%d",
             s_encoder_led_rgb, s_encoder_recording_gate ? 1 : 0);
}
```

`save_encoder_settings_to_nvs()` 在 `nvs_set_i32(handle, "enc_led", ...)` 块之后、`nvs_commit` 之前加：

```c
    err = nvs_set_i32(handle, "enc_rec_gate", s_encoder_recording_gate ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save encoder recording gate failed: %s", esp_err_to_name(err));
    }
```

- [ ] **Step 6: 编译验证**

Run: `python scripts/idf_cli.py -c`
Expected: build 成功。

- [ ] **Step 7: Commit**

```bash
git add firmware/main/main.c
git commit -m "feat(firmware): 编码器录音门控（press_action=key 时只发事件不录音）"
```

---

### Task 4: core `key_spec` 按键解析器

**Files:**
- Create: `desktop/windows/src/key_spec.h`
- Create: `desktop/windows/src/key_spec.cc`
- Modify: `desktop/windows/CMakeLists.txt:55-82`（voicestick_core 源清单）
- Test: `desktop/windows/tests/core_tests.cc`

**背景：** 旋转/单击/双击的自定义按键用热键语法（`down`、`ctrl+shift+v`）。现有解析在 `global_hotkey_win.cc`（`ParseSingleKey` 返回 `MOD_*` flags，键名不含方向键/音量键，且在 App 外壳目标里）。**刻意不重构它**：新建 core 独立解析器，返回 VK 值，键名集合是现有超集。core 已有使用 Win32 VK 常量的先例（`wechat_input_method_hotkey.cc:87`）。

- [ ] **Step 1: 写失败测试**

`core_tests.cc` 在 `TestAppConfigEncoderRoundTrip`（:2130）之前加：

```cpp
void TestKeySpecParse() {
    // 单键：方向键/enter/单字符/f 键/音量键。
    auto down = ParseKeySpec("down");
    assert(down.has_value());
    assert(down->modifiers.empty());
    assert(down->vk == VK_DOWN);
    assert(down->display_text == "Down");

    auto up = ParseKeySpec("UP");  // 大小写不敏感
    assert(up.has_value() && up->vk == VK_UP);

    auto enter = ParseKeySpec(" enter ");  // 前后空白容忍
    assert(enter.has_value() && enter->vk == VK_RETURN && enter->display_text == "Enter");

    auto v = ParseKeySpec("v");
    assert(v.has_value() && v->vk == 'V' && v->display_text == "V");

    auto f5 = ParseKeySpec("f5");
    assert(f5.has_value() && f5->vk == VK_F5 && f5->display_text == "F5");

    auto vol = ParseKeySpec("volumeup");
    assert(vol.has_value() && vol->vk == VK_VOLUME_UP);

    auto pgdn = ParseKeySpec("pagedown");
    assert(pgdn.has_value() && pgdn->vk == VK_NEXT);

    // 修饰键组合：display_text 修饰键固定 Ctrl/Alt/Shift/Win 序。
    auto combo = ParseKeySpec("win+shift+ctrl+v");
    assert(combo.has_value());
    assert(combo->vk == 'V');
    assert(combo->modifiers.size() == 3);
    assert(combo->modifiers[0] == VK_CONTROL);
    assert(combo->modifiers[1] == VK_SHIFT);
    assert(combo->modifiers[2] == VK_LWIN);
    assert(combo->display_text == "Ctrl+Shift+Win+V");

    auto alt_f4 = ParseKeySpec("alt+f4");
    assert(alt_f4.has_value() && alt_f4->vk == VK_F4 && alt_f4->display_text == "Alt+F4");

    // 非法：未知键名、仅修饰键、空串、重复主键。
    assert(!ParseKeySpec("bogus").has_value());
    assert(!ParseKeySpec("ctrl").has_value());
    assert(!ParseKeySpec("").has_value());
    assert(!ParseKeySpec("ctrl+").has_value());
    assert(!ParseKeySpec("a+b").has_value());
}
```

并在 `main()` 注册 `TestKeySpecParse();`（插在 `TestAppConfigEncoderRoundTrip();` 调用之前）。

- [ ] **Step 2: 跑测试确认失败**

Run: `cmd //c "_wt_build.bat"`
Expected: 编译失败（`ParseKeySpec` 未定义）。

- [ ] **Step 3: 实现 key_spec.h / key_spec.cc**

`desktop/windows/src/key_spec.h`：

```cpp
#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace voicestick {

// 一次按键注入的规格：修饰键（VK_CONTROL/VK_MENU/VK_SHIFT/VK_LWIN，固定
// Ctrl/Alt/Shift/Win 序）+ 主键 VK + 规范化显示文本（如 "Ctrl+Shift+V"）。
// 供编码器旋转/单击/双击的自定义按键注入使用。
struct KeySpec {
    std::vector<UINT> modifiers;
    UINT vk = 0;
    std::string display_text;
};

// 解析热键语法：单键（up/down/left/right/enter/esc/tab/space/backspace/delete/
// pageup/pagedown/home/end/insert/volumeup/volumedown/volumemute/f1-f24/单字符 A-Z0-9）
// 或修饰键组合（ctrl/alt/shift/win + 单键，"+" 分隔，大小写与前后空白不敏感）。
// 仅修饰键、未知键名、多个主键均返回 nullopt。
std::optional<KeySpec> ParseKeySpec(const std::string& text);

} // namespace voicestick
```

`desktop/windows/src/key_spec.cc`：

```cpp
#include "key_spec.h"

#include <algorithm>
#include <cctype>

namespace voicestick {
namespace {

std::string Trim(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 主键名 → VK。未知名返回 nullopt。
std::optional<UINT> MainKeyVkey(const std::string& lower) {
    if (lower.size() == 1) {
        const char ch = static_cast<char>(std::toupper(lower[0]));
        if (ch >= 'A' && ch <= 'Z') return static_cast<UINT>(ch);
        if (ch >= '0' && ch <= '9') return static_cast<UINT>(ch);
        return std::nullopt;
    }
    if (lower == "space") return VK_SPACE;
    if (lower == "enter" || lower == "return") return VK_RETURN;
    if (lower == "esc" || lower == "escape") return VK_ESCAPE;
    if (lower == "tab") return VK_TAB;
    if (lower == "backspace") return VK_BACK;
    if (lower == "delete") return VK_DELETE;
    if (lower == "insert") return VK_INSERT;
    if (lower == "up") return VK_UP;
    if (lower == "down") return VK_DOWN;
    if (lower == "left") return VK_LEFT;
    if (lower == "right") return VK_RIGHT;
    if (lower == "pageup") return VK_PRIOR;
    if (lower == "pagedown") return VK_NEXT;
    if (lower == "home") return VK_HOME;
    if (lower == "end") return VK_END;
    if (lower == "volumeup") return VK_VOLUME_UP;
    if (lower == "volumedown") return VK_VOLUME_DOWN;
    if (lower == "volumemute") return VK_VOLUME_MUTE;
    if (lower[0] == 'f' && lower.size() >= 2) {
        int num = 0;
        for (size_t i = 1; i < lower.size(); ++i) {
            if (lower[i] < '0' || lower[i] > '9') return std::nullopt;
            num = num * 10 + (lower[i] - '0');
        }
        if (num >= 1 && num <= 24) return VK_F1 + (num - 1);
    }
    return std::nullopt;
}

std::string VkeyDisplayName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    switch (vk) {
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Esc";
        case VK_TAB: return "Tab";
        case VK_BACK: return "Backspace";
        case VK_DELETE: return "Delete";
        case VK_INSERT: return "Insert";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_VOLUME_UP: return "VolumeUp";
        case VK_VOLUME_DOWN: return "VolumeDown";
        case VK_VOLUME_MUTE: return "VolumeMute";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    return {};
}

} // namespace

std::optional<KeySpec> ParseKeySpec(const std::string& text) {
    KeySpec spec;
    bool ctrl = false, alt = false, shift = false, win = false;
    bool has_main = false;

    size_t pos = 0;
    const std::string input = Trim(text);
    while (true) {
        const size_t plus = input.find('+', pos);
        const std::string part = Lower(Trim(input.substr(
            pos, plus == std::string::npos ? std::string::npos : plus - pos)));
        if (part.empty()) return std::nullopt;

        if (part == "ctrl" || part == "control") {
            if (ctrl) return std::nullopt;
            ctrl = true;
        } else if (part == "alt") {
            if (alt) return std::nullopt;
            alt = true;
        } else if (part == "shift") {
            if (shift) return std::nullopt;
            shift = true;
        } else if (part == "win" || part == "windows" || part == "meta") {
            if (win) return std::nullopt;
            win = true;
        } else {
            if (has_main) return std::nullopt;  // 多个主键
            const auto vk = MainKeyVkey(part);
            if (!vk.has_value()) return std::nullopt;
            spec.vk = *vk;
            has_main = true;
        }
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    if (!has_main) return std::nullopt;  // 仅修饰键

    // 修饰键固定 Ctrl/Alt/Shift/Win 序，display_text 规范化。
    if (ctrl) spec.modifiers.push_back(VK_CONTROL);
    if (alt) spec.modifiers.push_back(VK_MENU);
    if (shift) spec.modifiers.push_back(VK_SHIFT);
    if (win) spec.modifiers.push_back(VK_LWIN);
    static const char* kModNames[] = {"Ctrl", "Alt", "Shift", "Win"};
    size_t name_idx = 0;
    for (UINT mod : spec.modifiers) {
        if (name_idx > 0) spec.display_text += "+";
        switch (mod) {
            case VK_CONTROL: spec.display_text += kModNames[0]; break;
            case VK_MENU: spec.display_text += kModNames[1]; break;
            case VK_SHIFT: spec.display_text += kModNames[2]; break;
            case VK_LWIN: spec.display_text += kModNames[3]; break;
            default: break;
        }
        ++name_idx;
    }
    if (!spec.display_text.empty()) spec.display_text += "+";
    spec.display_text += VkeyDisplayName(spec.vk);
    return spec;
}

} // namespace voicestick
```

`core_tests.cc` 顶部 include 区加 `#include "key_spec.h"`。

- [ ] **Step 4: CMake 注册源文件**

`desktop/windows/CMakeLists.txt:56`（`src/air_mouse_kin.cc` 之后）加一行：

```cmake
    src/key_spec.cc
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cmd //c "_wt_build.bat"`
Expected: 编译成功，`voicestick_windows_tests` 全过（含 `TestKeySpecParse`）。

- [ ] **Step 6: Commit**

```bash
git add -f desktop/windows/src/key_spec.h desktop/windows/src/key_spec.cc desktop/windows/CMakeLists.txt desktop/windows/tests/core_tests.cc
git commit -m "feat(windows): core key_spec 按键解析器"
```

---

### Task 5: `InputInjector::SendKeyCombo`

**Files:**
- Modify: `desktop/windows/src/voice_stick_coordinator.h:162-175`（InputInjector 接口）
- Modify: `desktop/windows/src/input_injector_win.h` / `input_injector_win.cc`
- Test: `desktop/windows/tests/core_tests.cc`（FakeInputInjector + 接线测试）

**背景：** 注入「修饰键按下 → 主键 → 全释放」。主键必须带 scan code（`MapVirtualKeyW`），否则第三方输入法不响应（既有经验，`wechat_input_method_hotkey.cc:87` 同款写法）。

- [ ] **Step 1: 写失败测试**

`core_tests.cc` 的 `FakeInputInjector`（:287-312）加 override 与记录字段（此步测试编译不过，因为接口还没有该方法——先改接口再补 Fake，见 Step 2 顺序；此处给出最终形态）：

```cpp
    void SendKeyCombo(const KeySpec& spec) override {
        sent_key_combos.push_back(spec.display_text);
    }
```

字段区加：

```cpp
    std::vector<std::string> sent_key_combos;
```

新测试函数（放在 `TestInputInjectorArrowUpFakeWiring` :2122 之后）：

```cpp
void TestInputInjectorKeyComboFakeWiring() {
    // Fake 直连验证 SendKeyCombo 接线：协调器路由测试（Task 9）依赖此记录。
    FakeInputInjector input;
    const auto spec = ParseKeySpec("ctrl+shift+v");
    assert(spec.has_value());
    input.SendKeyCombo(*spec);
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Ctrl+Shift+V");
}
```

并在 `main()` 注册 `TestInputInjectorKeyComboFakeWiring();`（`TestInputInjectorArrowUpFakeWiring();` 之后）。

- [ ] **Step 2: 接口 + Win32 实现 + Fake**

`voice_stick_coordinator.h` 顶部 include 区加 `#include "key_spec.h"`；`InputInjector`（:162-175）在 `SendArrowUp()` 声明之后加：

```cpp
    // 注入一次按键组合：修饰键按下 → 主键（带 scan code）→ 逆序全释放。
    // 单键（无修饰键）退化为一次按键。供编码器自定义按键动作使用。
    virtual void SendKeyCombo(const KeySpec& spec) = 0;
```

`input_injector_win.h` 的类声明里加 `void SendKeyCombo(const KeySpec& spec) override;`（并确认该头已可见 `KeySpec`：加 `#include "key_spec.h"`）。

`input_injector_win.cc` 在 `SendArrowUp()` 实现之后加：

```cpp
void InputInjectorWin::SendKeyCombo(const KeySpec& spec) {
    if (spec.vk == 0) return;
    // 修饰键按下。
    for (UINT mod : spec.modifiers) {
        SendKey(static_cast<WORD>(mod), true);
    }
    // 主键带 scan code：仅 wVk 时第三方输入法不响应（见 wechat_input_method_hotkey.cc）。
    const WORD scan = static_cast<WORD>(MapVirtualKeyW(spec.vk, MAPVK_VK_TO_VSC));
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(spec.vk);
    input.ki.wScan = scan;
    SendInput(1, &input, sizeof(INPUT));
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
    // 修饰键逆序释放。
    for (auto it = spec.modifiers.rbegin(); it != spec.modifiers.rend(); ++it) {
        SendKey(static_cast<WORD>(*it), false);
    }
}
```

`core_tests.cc` 顶部 `#include "key_spec.h"`（Task 4 已加则跳过），FakeInputInjector 按 Step 1 最终形态补 override 与字段。

- [ ] **Step 3: 跑测试确认通过**

Run: `cmd //c "_wt_build.bat"`
Expected: 全过（含新接线测试）。

- [ ] **Step 4: Commit**

```bash
git add -f desktop/windows/src/voice_stick_coordinator.h desktop/windows/src/input_injector_win.h desktop/windows/src/input_injector_win.cc desktop/windows/tests/core_tests.cc
git commit -m "feat(windows): InputInjector SendKeyCombo 按键组合注入"
```

---

### Task 6: app_config 新增 7 个编码器配置键

**Files:**
- Modify: `desktop/windows/src/app_config.h:183-186`（encoder 字段区）
- Modify: `desktop/windows/src/app_config.cc`（flat-key 解析 :404-408、TOML 表解析 :599-606、保存 :702-712）
- Test: `desktop/windows/tests/core_tests.cc`

**背景：** 新增 `encoder_rotate_cw_key`/`encoder_rotate_ccw_key`/`encoder_led_color`/`encoder_press_action`/`encoder_press_key`/`encoder_double_click_action`/`encoder_double_click_key`（spec §3）。默认值全部等价当前硬编码行为，旧配置无缝升级。加载时校验：action 仅 `recording|key`、led_color 仅预设名、按键值过 `ParseKeySpec`；非法值回退默认（不写回坏值）。

- [ ] **Step 1: 写失败测试**

`core_tests.cc` 在 `TestAppConfigEncoderRoundTrip`（:2130-2146）之后加：

```cpp
void TestAppConfigEncoderSettingsRoundTrip() {
    // 默认值等价当前硬编码行为。
    const AppConfig defaults = AppConfig::Defaults();
    assert(defaults.encoder_rotate_cw_key == "down");
    assert(defaults.encoder_rotate_ccw_key == "up");
    assert(defaults.encoder_led_color == "red");
    assert(defaults.encoder_press_action == "recording");
    assert(defaults.encoder_press_key.empty());
    assert(defaults.encoder_double_click_action == "key");
    assert(defaults.encoder_double_click_key == "enter");

    // 保存/加载往返。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_encoder_settings_test.toml";
    std::filesystem::remove(temp);
    AppConfig config = AppConfig::Defaults();
    config.encoder_rotate_cw_key = "pageup";
    config.encoder_rotate_ccw_key = "pagedown";
    config.encoder_led_color = "cyan";
    config.encoder_press_action = "key";
    config.encoder_press_key = "ctrl+z";
    config.encoder_double_click_action = "recording";
    config.encoder_double_click_key = "ctrl+enter";
    config.Save(temp);
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.encoder_rotate_cw_key == "pageup");
    assert(loaded.encoder_rotate_ccw_key == "pagedown");
    assert(loaded.encoder_led_color == "cyan");
    assert(loaded.encoder_press_action == "key");
    assert(loaded.encoder_press_key == "ctrl+z");
    assert(loaded.encoder_double_click_action == "recording");
    assert(loaded.encoder_double_click_key == "ctrl+enter");
    std::filesystem::remove(temp);
}

void TestAppConfigEncoderSettingsInvalidFallback() {
    // 非法值回退默认：未知 action/颜色/按键语法。
    auto temp = std::filesystem::temp_directory_path() / "voicestick_encoder_invalid_test.toml";
    std::filesystem::remove(temp);
    {
        std::ofstream out(temp);
        out << "encoder_rotate_cw_key = \"bogus\"\n";
        out << "encoder_led_color = \"pink\"\n";
        out << "encoder_press_action = \"fly\"\n";
        out << "encoder_press_key = \"ctrl+\"\n";
        out << "encoder_double_click_action = \"fly\"\n";
        out << "encoder_double_click_key = \"a+b\"\n";
    }
    AppConfig loaded = AppConfig::Load(temp);
    assert(loaded.encoder_rotate_cw_key == "down");
    assert(loaded.encoder_led_color == "red");
    assert(loaded.encoder_press_action == "recording");
    assert(loaded.encoder_press_key.empty());
    assert(loaded.encoder_double_click_action == "key");
    assert(loaded.encoder_double_click_key == "enter");
    std::filesystem::remove(temp);
}
```

并在 `main()` 注册两个新测试（`TestAppConfigEncoderRoundTrip();` 之后）。

- [ ] **Step 2: 跑测试确认失败**

Run: `cmd //c "_wt_build.bat"`
Expected: 编译失败（字段不存在）。

- [ ] **Step 3: app_config.h 字段**

`app_config.h:186`（`encoder_rotation_invert` 声明）之后加：

```cpp
    // 编码器旋转顺时针/逆时针注入的按键（key_spec 语法，如 "down"/"ctrl+pageup"）。
    std::string encoder_rotate_cw_key = "down";
    std::string encoder_rotate_ccw_key = "up";
    // 编码器录音灯颜色：red/green/blue/yellow/purple/cyan/white/off。下发固件 NVS 持久化。
    std::string encoder_led_color = "red";
    // 编码器单击动作："recording"（同主键录音语义）或 "key"（注入 encoder_press_key）。
    std::string encoder_press_action = "recording";
    // 编码器单击自定义按键（action=key 时生效；空 = 未配置）。
    std::string encoder_press_key;
    // 编码器双击动作："key"（注入 encoder_double_click_key，默认 enter=现行为）
    // 或 "recording"（双击开始/停止录音，经 remote_button 通道）。
    std::string encoder_double_click_action = "key";
    std::string encoder_double_click_key = "enter";
```

- [ ] **Step 4: app_config.cc 校验 helper + 解析 + 保存**

`app_config.cc` 匿名命名空间（`TapSensitivityClamp` 等 helper 附近）加：

```cpp
bool IsValidEncoderButtonAction(const std::string& value) {
    return value == "recording" || value == "key";
}

bool IsValidEncoderLedColor(const std::string& value) {
    return value == "red" || value == "green" || value == "blue" || value == "yellow" ||
           value == "purple" || value == "cyan" || value == "white" || value == "off";
}
```

并确认 `app_config.cc` include 了 `key_spec.h`（没有则加）。

flat-key 解析段（:408 `encoder_rotation_invert` 行之后）加：

```cpp
    if (key == "encoder_rotate_cw_key" && ParseKeySpec(value).has_value()) config.encoder_rotate_cw_key = value;
    if (key == "encoder_rotate_ccw_key" && ParseKeySpec(value).has_value()) config.encoder_rotate_ccw_key = value;
    if (key == "encoder_led_color" && IsValidEncoderLedColor(value)) config.encoder_led_color = value;
    if (key == "encoder_press_action" && IsValidEncoderButtonAction(value)) config.encoder_press_action = value;
    if (key == "encoder_press_key" && (value.empty() || ParseKeySpec(value).has_value())) config.encoder_press_key = value;
    if (key == "encoder_double_click_action" && IsValidEncoderButtonAction(value)) config.encoder_double_click_action = value;
    if (key == "encoder_double_click_key" && ParseKeySpec(value).has_value()) config.encoder_double_click_key = value;
```

TOML 表解析段（:606 `encoder_rotation_invert` 行之后）加：

```cpp
        if (auto value = TomlString(table, "encoder_rotate_cw_key"); value && ParseKeySpec(*value).has_value()) config.encoder_rotate_cw_key = *value;
        if (auto value = TomlString(table, "encoder_rotate_ccw_key"); value && ParseKeySpec(*value).has_value()) config.encoder_rotate_ccw_key = *value;
        if (auto value = TomlString(table, "encoder_led_color"); value && IsValidEncoderLedColor(*value)) config.encoder_led_color = *value;
        if (auto value = TomlString(table, "encoder_press_action"); value && IsValidEncoderButtonAction(*value)) config.encoder_press_action = *value;
        if (auto value = TomlString(table, "encoder_press_key"); value && (value->empty() || ParseKeySpec(*value).has_value())) config.encoder_press_key = *value;
        if (auto value = TomlString(table, "encoder_double_click_action"); value && IsValidEncoderButtonAction(*value)) config.encoder_double_click_action = *value;
        if (auto value = TomlString(table, "encoder_double_click_key"); value && ParseKeySpec(*value).has_value()) config.encoder_double_click_key = *value;
```

保存段（:712 `encoder_rotation_invert` 行之后）加：

```cpp
    output << "encoder_rotate_cw_key = \"" << TomlEscape(encoder_rotate_cw_key) << "\"\n";
    output << "encoder_rotate_ccw_key = \"" << TomlEscape(encoder_rotate_ccw_key) << "\"\n";
    output << "encoder_led_color = \"" << TomlEscape(encoder_led_color) << "\"\n";
    output << "encoder_press_action = \"" << TomlEscape(encoder_press_action) << "\"\n";
    output << "encoder_press_key = \"" << TomlEscape(encoder_press_key) << "\"\n";
    output << "encoder_double_click_action = \"" << TomlEscape(encoder_double_click_action) << "\"\n";
    output << "encoder_double_click_key = \"" << TomlEscape(encoder_double_click_key) << "\"\n";
```

（`core_tests.cc` 顶部若缺 `#include <fstream>` 则补上。）

- [ ] **Step 5: 跑测试确认通过**

Run: `cmd //c "_wt_build.bat"`
Expected: 全过（含两个新测试）。

- [ ] **Step 6: Commit**

```bash
git add -f desktop/windows/src/app_config.h desktop/windows/src/app_config.cc desktop/windows/tests/core_tests.cc
git commit -m "feat(windows): app_config 编码器设置项（旋转键/灯色/手势动作）"
```

---

### Task 7: `StateEvent.source` 解析

**Files:**
- Modify: `desktop/windows/src/ble_protocol.h:28-42`
- Modify: `desktop/windows/src/ble_protocol.cc:147-167`
- Test: `desktop/windows/tests/core_tests.cc`

- [ ] **Step 1: 写失败测试**

`core_tests.cc` 在 `TestEncoderRotateStateParsing`（:768-790）之后加：

```cpp
void TestStateEventSourceParsing() {
    // 编码器按键事件带 source 字段。
    const std::string json = "{\"event\":\"button_click\",\"button\":\"primary\",\"duration_ms\":131,\"source\":\"encoder\"}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "button_click");
    assert(event->source == "encoder");

    // 物理键事件不带 source：解析为空串（缺省=物理键）。
    const std::string plain = "{\"event\":\"button_down\",\"button\":\"primary\",\"session_id\":42}";
    ByteVector plain_frame = {1, 0x10};
    AppendLe16(plain_frame, static_cast<std::uint16_t>(plain.size()));
    plain_frame.insert(plain_frame.end(), plain.begin(), plain.end());
    auto plain_event = BleProtocol::ParseStateEvent(plain_frame);
    assert(plain_event.has_value());
    assert(plain_event->source.empty());
}
```

并在 `main()` 注册（`TestEncoderRotateStateParsing();` 之后）。

- [ ] **Step 2: 跑测试确认失败**

Run: `cmd //c "_wt_build.bat"`
Expected: 编译失败（`StateEvent::source` 不存在）。

- [ ] **Step 3: 实现**

`ble_protocol.h` 的 `StateEvent`（:41 `steps` 字段之后）加：

```cpp
    // 事件来源标签：编码器按钮事件为 "encoder"；物理键/远程键省略该字段（空串）。
    std::string source;
```

`ble_protocol.cc` 的 `ParseStateEvent`（:164 `event.steps = ...` 之后）加：

```cpp
    event.source = JsonStringValue(json, "source");
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cmd //c "_wt_build.bat"`
Expected: 全过。

- [ ] **Step 5: Commit**

```bash
git add -f desktop/windows/src/ble_protocol.h desktop/windows/src/ble_protocol.cc desktop/windows/tests/core_tests.cc
git commit -m "feat(windows): StateEvent 解析 source 来源标签"
```

---

### Task 8: LED 颜色与录音门控的 BLE 下发链路

**Files:**
- Modify: `desktop/windows/src/ble_protocol.h`（payload 声明）/ `ble_protocol.cc`（payload 实现，:224 附近 TapEnabledPayload 之后）
- Modify: `desktop/windows/src/voice_stick_coordinator.h:67-104`（BleCentral 接口）
- Modify: `desktop/windows/src/ble_central_win.h` / `ble_central_win.cc`（:534-555 SendTapEnabled 为模板）
- Modify: `desktop/windows/src/voice_stick_coordinator.cc:99-104`（连接全量重发）与 :189-194（UpdateConfig 重发）
- Test: `desktop/windows/tests/core_tests.cc`

**背景：** 照抄 `SendTapEnabled` 全链路模式。门控值从 `encoder_press_action` 派生（`recording` → enabled=true），配置端无独立键（spec §5a）。

- [ ] **Step 1: 写失败测试**

`core_tests.cc` 的 `FakeBleCentral` 加 override 与记录字段（最终形态）：

```cpp
    void SendEncoderLedColor(const std::string& color,
                             const std::optional<std::string>& device_id) override {
        sent_encoder_led_colors.push_back(std::pair{color, device_id});
    }
    void SendEncoderRecordingGate(bool enabled,
                                  const std::optional<std::string>& device_id) override {
        sent_encoder_recording_gates.push_back(std::pair{enabled, device_id});
    }
```

字段区加：

```cpp
    std::vector<std::pair<std::string, std::optional<std::string>>> sent_encoder_led_colors;
    std::vector<std::pair<bool, std::optional<std::string>>> sent_encoder_recording_gates;
```

新测试（放在 `TestCoordinatorSyncsTapSensitivityOnConnectionAndConfigUpdate` :1518 之后）：

```cpp
void TestBleEncoderPayloads() {
    auto led = BleProtocol::EncoderLedColorPayload("cyan");
    assert(std::string(led.begin(), led.end()) == "{\"event\":\"encoder_led_color\",\"color\":\"cyan\"}");
    auto led_off = BleProtocol::EncoderLedColorPayload("off");
    assert(std::string(led_off.begin(), led_off.end()) == "{\"event\":\"encoder_led_color\",\"color\":\"off\"}");

    auto gate_off = BleProtocol::EncoderRecordingGatePayload(false);
    assert(std::string(gate_off.begin(), gate_off.end()) == "{\"event\":\"encoder_recording_gate\",\"enabled\":false}");
    auto gate_on = BleProtocol::EncoderRecordingGatePayload(true);
    assert(std::string(gate_on.begin(), gate_on.end()) == "{\"event\":\"encoder_recording_gate\",\"enabled\":true}");
}

void TestCoordinatorSyncsEncoderSettingsOnConnectionAndConfigUpdate() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_led_color = "purple";
    config.encoder_press_action = "key";  // 派生门控关闭
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();

    // 连接时全量重发：LED 颜色 + 门控（从 press_action 派生）。
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    assert(!ble_ptr->sent_encoder_led_colors.empty());
    assert(ble_ptr->sent_encoder_led_colors.back().first == "purple");
    assert(!ble_ptr->sent_encoder_recording_gates.empty());
    assert(ble_ptr->sent_encoder_recording_gates.back().first == false);

    // UpdateConfig 同样重发；press_action=recording 派生门控打开。
    AppConfig updated = AppConfig::Defaults();
    updated.encoder_led_color = "green";
    updated.encoder_press_action = "recording";
    coordinator.UpdateConfig(updated);
    assert(ble_ptr->sent_encoder_led_colors.back().first == "green");
    assert(ble_ptr->sent_encoder_recording_gates.back().first == true);
}
```

并在 `main()` 注册两个新测试。

- [ ] **Step 2: 跑测试确认失败**

Run: `cmd //c "_wt_build.bat"`
Expected: 编译失败（payload/接口方法不存在）。

- [ ] **Step 3: payload + 接口 + Win32 实现 + Fake**

`ble_protocol.h`（:78 `TapEnabledPayload` 声明之后）加：

```cpp
    static ByteVector EncoderLedColorPayload(std::string_view color);
    static ByteVector EncoderRecordingGatePayload(bool enabled);
```

`ble_protocol.cc`（`TapEnabledPayload` 实现 :212-216 之后）加：

```cpp
ByteVector BleProtocol::EncoderLedColorPayload(std::string_view color) {
    const auto json = std::string("{\"event\":\"encoder_led_color\",\"color\":\"") +
                      JsonEscape(color) + "\"}";
    return ByteVector(json.begin(), json.end());
}

ByteVector BleProtocol::EncoderRecordingGatePayload(bool enabled) {
    const auto json = std::string("{\"event\":\"encoder_recording_gate\",\"enabled\":") +
                      (enabled ? "true" : "false") + "}";
    return ByteVector(json.begin(), json.end());
}
```

`voice_stick_coordinator.h` 的 `BleCentral`（:76 `SendTapSensitivity` 声明之后）加：

```cpp
    // 编码器录音灯颜色（预设名 red/green/.../off）：固件侧 NVS 持久化，录音亮灯时使用。
    virtual void SendEncoderLedColor(const std::string& color,
                                     const std::optional<std::string>& device_id) = 0;
    // 编码器录音门控：enabled=false 时固件对编码器按下只发按键事件不启动录音
    // （桌面端把单击配为自定义按键时下发 false，从 encoder_press_action 派生）。
    virtual void SendEncoderRecordingGate(bool enabled,
                                          const std::optional<std::string>& device_id) = 0;
```

`ble_central_win.h` 类声明加同样两个 override 声明。`ble_central_win.cc`（`SendTapEnabled` :534-555 之后）加（完整复制其 targets 收集模式，仅换 payload）：

```cpp
void BleCentralWin::SendEncoderLedColor(const std::string& color,
                                        const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::EncoderLedColorPayload(color);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }
    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}

void BleCentralWin::SendEncoderRecordingGate(bool enabled,
                                             const std::optional<std::string>& device_id) {
    auto payload = BleProtocol::EncoderRecordingGatePayload(enabled);
    std::vector<std::shared_ptr<DeviceSession>> targets;
    {
        std::lock_guard lock(mutex_);
        if (device_id.has_value()) {
            auto it = sessions_by_device_id_.find(*device_id);
            if (it != sessions_by_device_id_.end() && it->second->ready) {
                targets.push_back(it->second);
            }
        } else {
            for (const auto& [_, session] : sessions_by_device_id_) {
                if (session->ready) targets.push_back(session);
            }
        }
    }
    for (auto& session : targets) {
        WriteControlPayloadAsync(std::move(session), payload);
    }
}
```

（实现前先读一眼 `SendTapEnabled` 的 targets 收集代码原样照搬，上面是按其模式展开的完整代码；如原文细节有出入以原文为准调整。）

`core_tests.cc` FakeBleCentral 按 Step 1 最终形态补 override 与字段。

- [ ] **Step 4: 协调器下发编排**

`voice_stick_coordinator.cc` 连接回调（:104 `SendImuWakeSensitivity` 行之后）加：

```cpp
        ble_->SendEncoderLedColor(config_.encoder_led_color, std::nullopt);
        ble_->SendEncoderRecordingGate(config_.encoder_press_action == "recording", std::nullopt);
```

`UpdateConfig`（:194 同样 `SendImuWakeSensitivity` 块之后）加相同两行。

- [ ] **Step 5: 跑测试确认通过**

Run: `cmd //c "_wt_build.bat"`
Expected: 全过（含两个新测试）。

- [ ] **Step 6: Commit**

```bash
git add -f desktop/windows/src/ble_protocol.h desktop/windows/src/ble_protocol.cc desktop/windows/src/voice_stick_coordinator.h desktop/windows/src/voice_stick_coordinator.cc desktop/windows/src/ble_central_win.h desktop/windows/src/ble_central_win.cc desktop/windows/tests/core_tests.cc
git commit -m "feat(windows): 编码器 LED 颜色与录音门控 BLE 下发链路"
```

---

### Task 9: 协调器编码器事件路由（单击/双击/旋转可配置）

**Files:**
- Modify: `desktop/windows/src/voice_stick_coordinator.h`（私有方法声明，:245 附近）
- Modify: `desktop/windows/src/voice_stick_coordinator.cc`（HandleStateEvent :397-420、HandleButtonDoubleClick :900-938、HandleEncoderRotate :963-995 + 新增 4 个 handler）
- Test: `desktop/windows/tests/core_tests.cc`

**背景（spec §4）：** `source=="encoder"` 的 primary 按键事件分流到可配置动作表；物理路径一字不改。单击 `recording` 走现有主键路径、`key` 在 click 时注入；双击 `key` 沿用现有取消结构注入配置键（默认 enter=现行为）、`recording` 经 `SendRemoteButton` 切换起停（spec §5b）；旋转查配置表注入，非法配置回退方向键。

- [ ] **Step 1: 写失败测试**

`core_tests.cc` 的 `ButtonEvent` helper（:534-542）改为带 source 版本（新增重载，不改既有调用）：

```cpp
// 构造编码器按键事件（固件上报带 "source":"encoder"）。
StateEvent EncoderButtonEvent(const std::string& event,
                              std::optional<std::uint32_t> session_id = std::nullopt) {
    StateEvent state_event;
    state_event.event = event;
    state_event.button = "primary";
    state_event.session_id = session_id;
    state_event.source = "encoder";
    return state_event;
}
```

新测试（放在 `TestEncoderRotateStepsClamped` :2251 之后）：

```cpp
void TestEncoderPressRecordingStartsSession() {
    // 默认 press_action=recording：编码器 button_down 走主键路径启动录音会话。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_down", 30));
    assert(asr_ptr->started);  // 与物理主键 down 同一行为
    assert(input.sent_key_combos.empty());
}

void TestEncoderPressKeyInjectsComboWithoutRecording() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_press_action = "key";
    config.encoder_press_key = "ctrl+z";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_click"));
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Ctrl+Z");
    assert(!asr_ptr->started);  // 不录音
}

void TestEncoderPressKeyInvalidIgnored() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_press_action = "key";
    config.encoder_press_key = "bogus";  // 运行期非法（绕过配置校验直造）
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_click"));
    assert(input.sent_key_combos.empty());  // 记日志忽略，不注入
}

void TestEncoderDoubleClickDefaultEnterCancelsSession() {
    // 双击默认 enter：取消活跃录音 + 注入 Enter（等价物理主键双击现行为）。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    VoiceStickCoordinator coordinator(AppConfig::Defaults(), std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));  // 物理键开播
    assert(asr_ptr->started);

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_double_click"));
    assert(input.send_enter_called);
    assert(asr_ptr->cancelled);
}

void TestEncoderDoubleClickCustomKey() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_double_click_action = "key";
    config.encoder_double_click_key = "ctrl+enter";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_double_click"));
    assert(input.sent_key_combos.size() == 1);
    assert(input.sent_key_combos[0] == "Ctrl+Enter");
    assert(!input.send_enter_called);  // 不再走 SendEnter
}

void TestEncoderDoubleClickRecordingTogglesRemoteButton() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    auto* asr_ptr = asr.get();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_double_click_action = "recording";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 空闲双击 → remote down 开播。
    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_double_click"));
    assert(!ble_ptr->sent_remote_buttons.empty());
    assert(ble_ptr->sent_remote_buttons.back().action == RemoteButtonAction::kDown);

    // 录音中双击 → remote up 停播。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_down", "primary", 30));
    assert(asr_ptr->started);
    ble_ptr->on_state_event("5A74", EncoderButtonEvent("button_double_click"));
    assert(ble_ptr->sent_remote_buttons.back().action == RemoteButtonAction::kUp);
}

void TestEncoderRotateCustomKeys() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_rotate_cw_key = "pagedown";
    config.encoder_rotate_ccw_key = "pageup";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    ble_ptr->on_state_event("5A74", EncoderRotateEvent("ccw", 1));
    assert(input.sent_key_combos.size() == 3);
    assert(input.sent_key_combos[0] == "PageDown");
    assert(input.sent_key_combos[1] == "PageDown");
    assert(input.sent_key_combos[2] == "PageUp");
    assert(input.arrow_down_count == 0);  // 不再走硬编码方向键
}

void TestEncoderRotateInvalidKeyFallsBackToArrows() {
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_rotate_cw_key = "bogus";  // 运行期非法
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    ble_ptr->on_state_event("5A74", EncoderRotateEvent("cw", 2));
    assert(input.arrow_down_count == 2);  // 回退方向键
    assert(input.sent_key_combos.empty());
}

void TestPhysicalPrimaryUnaffectedByEncoderConfig() {
    // press_action=key 只影响 source=encoder 的事件；物理主键单击行为不变。
    auto ble = std::make_unique<FakeBleCentral>();
    auto* ble_ptr = ble.get();
    auto asr = std::make_unique<FakeAsrClient>();
    FakeUi ui;
    FakeInputInjector input;
    AppConfig config = AppConfig::Defaults();
    config.encoder_press_action = "key";
    config.encoder_press_key = "ctrl+z";
    VoiceStickCoordinator coordinator(config, std::move(ble), std::move(asr), &ui, &input);
    coordinator.Start();
    ble_ptr->connected_device_ids.insert("5A74");
    ble_ptr->on_connection_change({ConnectedDevice{"5A74", "VS-5A74"}});

    // 物理主键单击（无 source）：hold_to_talk 默认下走现有 ready 回写，不注入组合键。
    ble_ptr->on_state_event("5A74", ButtonEvent("button_click", "primary"));
    assert(input.sent_key_combos.empty());
}
```

（`FakeAsrClient` 用 `started`/`cancelled` 布尔标志，:152-172；`SentRemoteButton` 结构体的 `action` 字段类型为 `RemoteButtonAction`，:55-60。）

并在 `main()` 注册全部 9 个新测试。

- [ ] **Step 2: 跑测试确认失败**

Run: `cmd //c "_wt_build.bat"`
Expected: 编译失败（handler 未实现/行为未接）。

- [ ] **Step 3: HandleStateEvent 分流**

`voice_stick_coordinator.cc:407-414` 四个按钮分支改为：

```cpp
    } else if (event.event == "button_down") {
        if (event.button == "primary" && event.source == "encoder") {
            HandleEncoderButtonDown(event, device_id);
        } else {
            HandleButtonDown(event, device_id);
        }
    } else if (event.event == "button_up") {
        if (event.button == "primary" && event.source == "encoder") {
            HandleEncoderButtonUp(event, device_id);
        } else {
            HandleButtonUp(event, device_id);
        }
    } else if (event.event == "button_click") {
        if (event.button == "primary" && event.source == "encoder") {
            HandleEncoderButtonClick(event, device_id);
        } else {
            HandleButtonClick(event, device_id);
        }
    } else if (event.event == "button_double_click") {
        if (event.button == "primary" && event.source == "encoder") {
            HandleEncoderButtonDoubleClick(event, device_id);
        } else {
            HandleButtonDoubleClick(event, device_id);
        }
    }
```

- [ ] **Step 4: 抽取双击取消结构 + 改造 HandleButtonDoubleClick**

把 `HandleButtonDoubleClick`（:900-938）primary 路径的取消块抽为私有方法，整个函数改为：

```cpp
void VoiceStickCoordinator::CancelActiveSessionsForDoubleClick(const std::string& device_id) {
    // wechat 模式走专用停止路径（发 hotkey.SendUp，让第三方输入法把已识别文字送入
    // 输入框），主路径走 ASR 取消。字幕会话一并取消。
    if (IsWechatInputMethodActive()) {
        StopWechatInputMethodSession();
    } else {
        std::lock_guard lock(audio_mutex_);
        if (active_session_id_.has_value() && active_device_id_ == device_id) {
            CancelAudioEndTimeout();
            asr_->Cancel();
            pending_paste_state_ = {};
            active_session_id_.reset();
            debug_audio_recorder_.Discard();
            FinishRecognitionCycle();
        }
    }
    CancelSubtitleCyclesForDevice(device_id, "double_click");
}

void VoiceStickCoordinator::HandleButtonDoubleClick(const StateEvent& event, const std::string& device_id) {
    // 侧键双击：恢复上次输入确认（与侧键单击=进/退体感分离）。
    if (event.button == "secondary") {
        if (IsAirMouseActive(device_id)) return;
        if (IsWechatInputMethodActive()) {
            StopWechatInputMethodSession();
        }
        LogCoordinatorLine("secondary double-click on VS-" + device_id + ", restoring last input");
        RestoreLastInputConfirmation(device_id);
        return;
    }
    if (event.button != "primary") return;

    LogCoordinatorLine("double-click detected on VS-" + device_id + ", sending Enter");
    CancelActiveSessionsForDoubleClick(device_id);
    input_injector_->SendEnter();
    ble_->SendUiState("ready", "", device_id);
    EnterReady("double_click_enter");
}
```

（行为与原实现逐行等价，仅取消块搬家。原 :916-917 的注释要点并入新函数注释。）

- [ ] **Step 5: 新增 4 个编码器 handler**

放在 `HandleButtonDoubleClick` 之后：

```cpp
void VoiceStickCoordinator::HandleEncoderButtonDown(const StateEvent& event,
                                                    const std::string& device_id) {
    if (config_.encoder_press_action == "recording") {
        HandleButtonDown(event, device_id);
        return;
    }
    // key 动作：down/up 不注入（在 click 成对确认时注入一次），仅记日志。
    LogCoordinatorLine("encoder button down on VS-" + device_id + " (press_action=key, ignored)");
}

void VoiceStickCoordinator::HandleEncoderButtonUp(const StateEvent& event,
                                                  const std::string& device_id) {
    if (config_.encoder_press_action == "recording") {
        HandleButtonUp(event, device_id);
        return;
    }
    LogCoordinatorLine("encoder button up on VS-" + device_id + " (press_action=key, ignored)");
}

void VoiceStickCoordinator::HandleEncoderButtonClick(const StateEvent& event,
                                                     const std::string& device_id) {
    if (config_.encoder_press_action == "recording") {
        HandleButtonClick(event, device_id);
        return;
    }
    const auto spec = ParseKeySpec(config_.encoder_press_key);
    if (!spec.has_value()) {
        LogCoordinatorLine("encoder press key invalid: \"" + config_.encoder_press_key +
                           "\", click ignored");
        return;
    }
    LogCoordinatorLine("encoder click on VS-" + device_id + ", injecting " + spec->display_text);
    input_injector_->SendKeyCombo(*spec);
}

void VoiceStickCoordinator::HandleEncoderButtonDoubleClick(const StateEvent& event,
                                                           const std::string& device_id) {
    (void)event;
    if (config_.encoder_double_click_action == "recording") {
        // 切换录音起停：复用固件 remote_button 通道（固件侧等价一次远程按下/松开，
        // 音频链路真实完整，等同 click_to_talk 点按起停）。
        const bool has_active = HasActiveSession() || IsWechatInputMethodActive() ||
                                HasActiveSubtitleSession(device_id);
        LogCoordinatorLine("encoder double-click on VS-" + device_id +
                           (has_active ? ", remote stop recording" : ", remote start recording"));
        ble_->SendRemoteButton(has_active ? RemoteButtonAction::kUp : RemoteButtonAction::kDown,
                               "primary", device_id, next_hotkey_request_id_++);
        return;
    }
    // key 动作：沿用物理主键双击的取消结构，注入配置的按键（默认 enter=现行为）。
    CancelActiveSessionsForDoubleClick(device_id);
    const auto spec = ParseKeySpec(config_.encoder_double_click_key);
    if (spec.has_value()) {
        LogCoordinatorLine("encoder double-click on VS-" + device_id + ", injecting " +
                           spec->display_text);
        input_injector_->SendKeyCombo(*spec);
    } else {
        LogCoordinatorLine("encoder double-click key invalid: \"" +
                           config_.encoder_double_click_key + "\", fallback to Enter");
        input_injector_->SendEnter();
    }
    ble_->SendUiState("ready", "", device_id);
    EnterReady("encoder_double_click_key");
}
```

（`next_hotkey_request_id_` 复用热键的请求计数器，`voice_stick_coordinator.h:498`。）

- [ ] **Step 6: HandleEncoderRotate 改查表注入**

`HandleEncoderRotate`（:963-995）中 :980-994 段（`send_up` 计算 + 日志 + 注入循环）替换为：

```cpp
    // 方向映射：默认 cw→encoder_rotate_cw_key / ccw→encoder_rotate_ccw_key；
    // encoder_rotation_invert=true 时翻转。direction 非 "ccw"（含空串/未知值）按 cw 处理。
    const bool effective_ccw = (event.direction == "ccw") != config_.encoder_rotation_invert;
    const std::string& key_text = effective_ccw ? config_.encoder_rotate_ccw_key
                                                : config_.encoder_rotate_cw_key;
    const auto spec = ParseKeySpec(key_text);
    if (!spec.has_value()) {
        // 非法配置（绕过加载校验直改内存/未来新键名）回退方向键，保持可用。
        LogCoordinatorLine("encoder rotate key invalid: \"" + key_text +
                           "\", fallback to arrows");
        for (std::uint32_t i = 0; i < steps; ++i) {
            if (effective_ccw) {
                input_injector_->SendArrowUp();
            } else {
                input_injector_->SendArrowDown();
            }
        }
        return;
    }
    LogCoordinatorLine("encoder rotate on VS-" + device_id + " direction=" + event.direction +
                       " steps=" + std::to_string(steps) +
                       (raw_steps > steps ? " (clamped from " + std::to_string(raw_steps) + ")" : "") +
                       " -> " + spec->display_text);
    for (std::uint32_t i = 0; i < steps; ++i) {
        input_injector_->SendKeyCombo(*spec);
    }
```

（`:969` 起的总开关/体感/录音门控与 steps 钳制段保持原样不动。）

- [ ] **Step 7: 头文件声明**

`voice_stick_coordinator.h` 私有方法区（`HandleButtonDoubleClick` 声明附近）加：

```cpp
    void HandleEncoderButtonDown(const StateEvent& event, const std::string& device_id);
    void HandleEncoderButtonUp(const StateEvent& event, const std::string& device_id);
    void HandleEncoderButtonClick(const StateEvent& event, const std::string& device_id);
    void HandleEncoderButtonDoubleClick(const StateEvent& event, const std::string& device_id);
    // 双击取消结构：取消活跃录音（wechat/主路径）与字幕会话，供物理主键双击与
    // 编码器双击 key 动作共用。
    void CancelActiveSessionsForDoubleClick(const std::string& device_id);
```

并确认 `voice_stick_coordinator.h` 已 include `key_spec.h`（Task 5 已加）。

- [ ] **Step 8: 跑测试确认通过**

Run: `cmd //c "_wt_build.bat"`
Expected: 全过（含 9 个新测试与全部既有编码器/双击测试）。

- [ ] **Step 9: Commit**

```bash
git add -f desktop/windows/src/voice_stick_coordinator.h desktop/windows/src/voice_stick_coordinator.cc desktop/windows/tests/core_tests.cc
git commit -m "feat(windows): 协调器编码器事件路由（单击/双击/旋转可配置）"
```

---

### Task 10: 设置对话框「编码器」一节

**Files:**
- Modify: `desktop/windows/src/localization.h`（StringId 枚举末尾追加）
- Modify: `desktop/windows/src/localization.cc`（英文表 :53 区域风格 + 中文表 :289 区域风格）
- Modify: `desktop/windows/src/settings_dialog.h`（控件成员 :103-105 区域、ID 常量 :166-168 之后、私有方法声明）
- Modify: `desktop/windows/src/settings_dialog.cc`（BuildControls 布局 :872-889 区域、LoadSettings :1133 区域、SaveSettings :1246 区域、WM_COMMAND 处理、kClientHeight :132）
- 验证：编译 + 手动开对话框目视（无对话框自动化测试）

**背景：** 沿用 `LayoutEntry` 声明式排版（`add(row_h + Dp(10), {...})`）与 `remember`/`remember_label`/`CreateButton`/`CreateCombo`/`CreateEdit` helper（模板见 `tap_to_arrow_check_` 行 :827-836 与 `imu_wake_sensitivity_combo_` 行 :811-826）。新一节插在「设备交互」之后（`settings_dialog.cc:888` air mouse Y 块收尾 `}` 与 :889 `separator();` 之间）。

- [ ] **Step 1: 控件 ID 与成员声明**

`settings_dialog.h` ID 常量区（:168 `kIdHotwordCandidateDismiss = 2038` 之后）加：

```cpp
    static constexpr UINT kIdEncoderToArrow = 2039;
    static constexpr UINT kIdEncoderRotationInvert = 2040;
    static constexpr UINT kIdEncoderRotateCwKey = 2041;
    static constexpr UINT kIdEncoderRotateCcwKey = 2042;
    static constexpr UINT kIdEncoderLedColor = 2043;
    static constexpr UINT kIdEncoderPressAction = 2044;
    static constexpr UINT kIdEncoderPressKey = 2045;
    static constexpr UINT kIdEncoderDoubleClickAction = 2046;
    static constexpr UINT kIdEncoderDoubleClickKey = 2047;
```

控件成员区（:105 `tap_sensitivity_value_label_` 之后）加：

```cpp
    HWND encoder_to_arrow_check_ = nullptr;
    HWND encoder_rotation_invert_check_ = nullptr;
    HWND encoder_rotate_cw_key_edit_ = nullptr;
    HWND encoder_rotate_ccw_key_edit_ = nullptr;
    HWND encoder_led_color_combo_ = nullptr;
    HWND encoder_press_action_combo_ = nullptr;
    HWND encoder_press_key_edit_ = nullptr;
    HWND encoder_double_click_action_combo_ = nullptr;
    HWND encoder_double_click_key_edit_ = nullptr;
```

私有方法声明区（:42 `UpdateAirMouseSensitivityYLabel` 之后）加：

```cpp
    // 单击/双击动作组合框切换时启用/禁用对应按键编辑框。
    void UpdateEncoderKeyEditStates();
```

`kClientHeight`（:132）从 `1240` 改为 `1510`（新一节 7 行 + 节标题，约 +270）。

- [ ] **Step 2: 本地化字符串**

`localization.h` 的 `StringId` 枚举**末尾**（保持既有序号不变）追加：

```cpp
    kSettingsSectionEncoder,
    kSettingsEncoderToArrow,
    kSettingsEncoderRotationInvert,
    kSettingsEncoderRotateCwKey,
    kSettingsEncoderRotateCcwKey,
    kSettingsEncoderLedColor,
    kSettingsEncoderPressAction,
    kSettingsEncoderDoubleClickAction,
    kSettingsEncoderActionRecording,
    kSettingsEncoderActionKey,
    kSettingsEncoderLedRed,
    kSettingsEncoderLedGreen,
    kSettingsEncoderLedBlue,
    kSettingsEncoderLedYellow,
    kSettingsEncoderLedPurple,
    kSettingsEncoderLedCyan,
    kSettingsEncoderLedWhite,
    kSettingsEncoderLedOff,
    kSettingsEncoderInvalidKey,
```

`localization.cc` 英文表追加：

```cpp
    table[Index(StringId::kSettingsSectionEncoder)] = "Encoder";
    table[Index(StringId::kSettingsEncoderToArrow)] = "Inject keys on rotate";
    table[Index(StringId::kSettingsEncoderRotationInvert)] = "Invert rotation direction";
    table[Index(StringId::kSettingsEncoderRotateCwKey)] = "Clockwise key";
    table[Index(StringId::kSettingsEncoderRotateCcwKey)] = "Counter-clockwise key";
    table[Index(StringId::kSettingsEncoderLedColor)] = "Recording LED color";
    table[Index(StringId::kSettingsEncoderPressAction)] = "Press action";
    table[Index(StringId::kSettingsEncoderDoubleClickAction)] = "Double-click action";
    table[Index(StringId::kSettingsEncoderActionRecording)] = "Recording";
    table[Index(StringId::kSettingsEncoderActionKey)] = "Custom key";
    table[Index(StringId::kSettingsEncoderLedRed)] = "Red";
    table[Index(StringId::kSettingsEncoderLedGreen)] = "Green";
    table[Index(StringId::kSettingsEncoderLedBlue)] = "Blue";
    table[Index(StringId::kSettingsEncoderLedYellow)] = "Yellow";
    table[Index(StringId::kSettingsEncoderLedPurple)] = "Purple";
    table[Index(StringId::kSettingsEncoderLedCyan)] = "Cyan";
    table[Index(StringId::kSettingsEncoderLedWhite)] = "White";
    table[Index(StringId::kSettingsEncoderLedOff)] = "Off";
    table[Index(StringId::kSettingsEncoderInvalidKey)] = "Invalid encoder key syntax (e.g. \"down\", \"ctrl+z\"); the field was not saved.";
```

中文表追加：

```cpp
    table[Index(StringId::kSettingsSectionEncoder)] = "编码器";
    table[Index(StringId::kSettingsEncoderToArrow)] = "旋转时注入按键";
    table[Index(StringId::kSettingsEncoderRotationInvert)] = "旋转方向翻转";
    table[Index(StringId::kSettingsEncoderRotateCwKey)] = "顺时针按键";
    table[Index(StringId::kSettingsEncoderRotateCcwKey)] = "逆时针按键";
    table[Index(StringId::kSettingsEncoderLedColor)] = "录音灯颜色";
    table[Index(StringId::kSettingsEncoderPressAction)] = "单击动作";
    table[Index(StringId::kSettingsEncoderDoubleClickAction)] = "双击动作";
    table[Index(StringId::kSettingsEncoderActionRecording)] = "录音";
    table[Index(StringId::kSettingsEncoderActionKey)] = "自定义按键";
    table[Index(StringId::kSettingsEncoderLedRed)] = "红";
    table[Index(StringId::kSettingsEncoderLedGreen)] = "绿";
    table[Index(StringId::kSettingsEncoderLedBlue)] = "蓝";
    table[Index(StringId::kSettingsEncoderLedYellow)] = "黄";
    table[Index(StringId::kSettingsEncoderLedPurple)] = "紫";
    table[Index(StringId::kSettingsEncoderLedCyan)] = "青";
    table[Index(StringId::kSettingsEncoderLedWhite)] = "白";
    table[Index(StringId::kSettingsEncoderLedOff)] = "关";
    table[Index(StringId::kSettingsEncoderInvalidKey)] = "编码器按键语法无效（示例：down、ctrl+z），该字段未保存。";
```

- [ ] **Step 3: BuildControls 布局**

`settings_dialog.cc:888`（air mouse Y 块收尾 `}`）之后、:889 `separator();` 之前插入（`label_w`/`ctrl_x`/`ctrl_w`/`row_h` 均复用上下文现有变量）：

```cpp
    separator();

    // ===== 编码器 =====
    section_title(StringId::kSettingsSectionEncoder);
    {
        HWND eta_label = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, Dp(20), instance_));
        encoder_to_arrow_check_ = remember(CreateButton(
            hwnd_, TrW(StringId::kSettingsEncoderToArrow, language).c_str(),
            0, 0, ctrl_w, Dp(22), kIdEncoderToArrow, instance_, BS_AUTOCHECKBOX));
        add(row_h + Dp(10), {
            {eta_label, Dp(10), Dp(3), label_w, Dp(20)},
            {encoder_to_arrow_check_, ctrl_x, 0, ctrl_w, Dp(22)},
        });
    }
    {
        HWND eri_label = remember_label(CreateLabel(hwnd_, L"", 0, 0, label_w, Dp(20), instance_));
        encoder_rotation_invert_check_ = remember(CreateButton(
            hwnd_, TrW(StringId::kSettingsEncoderRotationInvert, language).c_str(),
            0, 0, ctrl_w, Dp(22), kIdEncoderRotationInvert, instance_, BS_AUTOCHECKBOX));
        add(row_h + Dp(10), {
            {eri_label, Dp(10), Dp(3), label_w, Dp(20)},
            {encoder_rotation_invert_check_, ctrl_x, 0, ctrl_w, Dp(22)},
        });
    }
    {
        HWND cw_label = remember_label(CreateLabel(
            hwnd_, label_text(StringId::kSettingsEncoderRotateCwKey).c_str(),
            0, 0, label_w, Dp(20), instance_));
        encoder_rotate_cw_key_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                                          kIdEncoderRotateCwKey, instance_));
        add(row_h + Dp(10), {
            {cw_label, Dp(10), Dp(3), label_w, Dp(20)},
            {encoder_rotate_cw_key_edit_, ctrl_x, 0, ctrl_w, Dp(24)},
        });
    }
    {
        HWND ccw_label = remember_label(CreateLabel(
            hwnd_, label_text(StringId::kSettingsEncoderRotateCcwKey).c_str(),
            0, 0, label_w, Dp(20), instance_));
        encoder_rotate_ccw_key_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                                           kIdEncoderRotateCcwKey, instance_));
        add(row_h + Dp(10), {
            {ccw_label, Dp(10), Dp(3), label_w, Dp(20)},
            {encoder_rotate_ccw_key_edit_, ctrl_x, 0, ctrl_w, Dp(24)},
        });
    }
    {
        HWND led_label = remember_label(CreateLabel(
            hwnd_, label_text(StringId::kSettingsEncoderLedColor).c_str(),
            0, 0, label_w, Dp(20), instance_));
        encoder_led_color_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(200),
                                                        kIdEncoderLedColor, instance_));
        const StringId led_names[] = {
            StringId::kSettingsEncoderLedRed, StringId::kSettingsEncoderLedGreen,
            StringId::kSettingsEncoderLedBlue, StringId::kSettingsEncoderLedYellow,
            StringId::kSettingsEncoderLedPurple, StringId::kSettingsEncoderLedCyan,
            StringId::kSettingsEncoderLedWhite, StringId::kSettingsEncoderLedOff,
        };
        for (const auto id : led_names) {
            SendMessageW(encoder_led_color_combo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(TrW(id, language).c_str()));
        }
        add(row_h + Dp(10), {
            {led_label, Dp(10), Dp(3), label_w, Dp(20)},
            {encoder_led_color_combo_, ctrl_x, 0, ctrl_w, Dp(200)},
        });
    }
    {
        // 单击动作 + 单击按键：动作选「自定义按键」时按键框启用。
        HWND pa_label = remember_label(CreateLabel(
            hwnd_, label_text(StringId::kSettingsEncoderPressAction).c_str(),
            0, 0, label_w, Dp(20), instance_));
        encoder_press_action_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(120),
                                                           kIdEncoderPressAction, instance_));
        SendMessageW(encoder_press_action_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsEncoderActionRecording, language).c_str()));
        SendMessageW(encoder_press_action_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsEncoderActionKey, language).c_str()));
        add(row_h + Dp(10), {
            {pa_label, Dp(10), Dp(3), label_w, Dp(20)},
            {encoder_press_action_combo_, ctrl_x, 0, ctrl_w, Dp(120)},
        });
        encoder_press_key_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                                      kIdEncoderPressKey, instance_));
        add(row_h + Dp(10), {
            {encoder_press_key_edit_, ctrl_x, 0, ctrl_w, Dp(24)},
        });
    }
    {
        // 双击动作（自定义按键/录音，默认自定义=enter）+ 双击按键。
        HWND da_label = remember_label(CreateLabel(
            hwnd_, label_text(StringId::kSettingsEncoderDoubleClickAction).c_str(),
            0, 0, label_w, Dp(20), instance_));
        encoder_double_click_action_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(120),
                                                                  kIdEncoderDoubleClickAction, instance_));
        SendMessageW(encoder_double_click_action_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsEncoderActionKey, language).c_str()));
        SendMessageW(encoder_double_click_action_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(TrW(StringId::kSettingsEncoderActionRecording, language).c_str()));
        add(row_h + Dp(10), {
            {da_label, Dp(10), Dp(3), label_w, Dp(20)},
            {encoder_double_click_action_combo_, ctrl_x, 0, ctrl_w, Dp(120)},
        });
        encoder_double_click_key_edit_ = remember(CreateEdit(hwnd_, 0, 0, ctrl_w, Dp(24),
                                                             kIdEncoderDoubleClickKey, instance_));
        add(row_h + Dp(10), {
            {encoder_double_click_key_edit_, ctrl_x, 0, ctrl_w, Dp(24)},
        });
    }
```

（`CreateEdit` 签名已核实：`CreateEdit(parent, x, y, w, h, id, inst, extra_style=0)`，`settings_dialog.cc:99`；上面调用与该签名一致。）

- [ ] **Step 4: WM_COMMAND 启用/禁用联动 + UpdateEncoderKeyEditStates**

在 `settings_dialog.cc` 的 WM_COMMAND 处理（:249 `case kIdOutputTarget:` 同款风格，每个 case 直接 `return TRUE`）加：

```cpp
        case kIdEncoderPressAction:
        case kIdEncoderDoubleClickAction:
            if (HIWORD(w_param) == CBN_SELCHANGE) UpdateEncoderKeyEditStates();
            return TRUE;
```

实现（放在 `UpdateOutputTargetVisibility` 附近）：

```cpp
void SettingsDialog::UpdateEncoderKeyEditStates() {
    // 单击动作 idx 1=自定义按键 时启用单击按键框；双击动作 idx 0=自定义按键 时启用双击按键框。
    const int press_idx = static_cast<int>(SendMessageW(encoder_press_action_combo_, CB_GETCURSEL, 0, 0));
    EnableWindow(encoder_press_key_edit_, press_idx == 1 ? TRUE : FALSE);
    const int double_idx = static_cast<int>(SendMessageW(encoder_double_click_action_combo_, CB_GETCURSEL, 0, 0));
    EnableWindow(encoder_double_click_key_edit_, double_idx == 0 ? TRUE : FALSE);
}
```

- [ ] **Step 5: LoadSettings 回填**

`settings_dialog.cc:1133`（`tap_to_arrow_check_` 的 `BM_SETCHECK` 行）之后加：

```cpp
    SendMessageW(encoder_to_arrow_check_, BM_SETCHECK, config_.encoder_to_arrow ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(encoder_rotation_invert_check_, BM_SETCHECK, config_.encoder_rotation_invert ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(encoder_rotate_cw_key_edit_, Utf16(config_.encoder_rotate_cw_key).c_str());
    SetWindowTextW(encoder_rotate_ccw_key_edit_, Utf16(config_.encoder_rotate_ccw_key).c_str());
    {
        static const char* kLedColors[] = {"red", "green", "blue", "yellow",
                                           "purple", "cyan", "white", "off"};
        int led_idx = 0;
        for (int i = 0; i < 8; ++i) {
            if (config_.encoder_led_color == kLedColors[i]) led_idx = i;
        }
        SendMessageW(encoder_led_color_combo_, CB_SETCURSEL, led_idx, 0);
    }
    SendMessageW(encoder_press_action_combo_, CB_SETCURSEL,
                 config_.encoder_press_action == "key" ? 1 : 0, 0);
    SetWindowTextW(encoder_press_key_edit_, Utf16(config_.encoder_press_key).c_str());
    SendMessageW(encoder_double_click_action_combo_, CB_SETCURSEL,
                 config_.encoder_double_click_action == "recording" ? 1 : 0, 0);
    SetWindowTextW(encoder_double_click_key_edit_, Utf16(config_.encoder_double_click_key).c_str());
    UpdateEncoderKeyEditStates();
```

（`Utf16`/`Utf8`/`GetWindowText` helper 已核实存在：`settings_dialog.cc:24`/`:35`/`:1262` 同款用法。）

- [ ] **Step 6: SaveSettings 读取 + 校验**

`settings_dialog.cc:1246`（`config_.tap_to_arrow = ...` 行）之后加：

```cpp
    config_.encoder_to_arrow = SendMessageW(encoder_to_arrow_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config_.encoder_rotation_invert = SendMessageW(encoder_rotation_invert_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    // 按键字段保存前过 ParseKeySpec；非法值提示且不写回（保留旧值）。
    bool encoder_key_invalid = false;
    const std::string cw_key = Utf8(GetWindowText(encoder_rotate_cw_key_edit_));
    if (ParseKeySpec(cw_key).has_value()) {
        config_.encoder_rotate_cw_key = cw_key;
    } else {
        encoder_key_invalid = true;
    }
    const std::string ccw_key = Utf8(GetWindowText(encoder_rotate_ccw_key_edit_));
    if (ParseKeySpec(ccw_key).has_value()) {
        config_.encoder_rotate_ccw_key = ccw_key;
    } else {
        encoder_key_invalid = true;
    }
    {
        static const char* kLedColors[] = {"red", "green", "blue", "yellow",
                                           "purple", "cyan", "white", "off"};
        const int led_idx = static_cast<int>(SendMessageW(encoder_led_color_combo_, CB_GETCURSEL, 0, 0));
        if (led_idx >= 0 && led_idx < 8) {
            config_.encoder_led_color = kLedColors[led_idx];
        }
    }
    const int press_idx = static_cast<int>(SendMessageW(encoder_press_action_combo_, CB_GETCURSEL, 0, 0));
    config_.encoder_press_action = (press_idx == 1) ? "key" : "recording";
    const std::string press_key = Utf8(GetWindowText(encoder_press_key_edit_));
    if (press_key.empty() || ParseKeySpec(press_key).has_value()) {
        config_.encoder_press_key = press_key;
    } else {
        encoder_key_invalid = true;
    }
    const int double_idx = static_cast<int>(SendMessageW(encoder_double_click_action_combo_, CB_GETCURSEL, 0, 0));
    config_.encoder_double_click_action = (double_idx == 1) ? "recording" : "key";
    const std::string double_key = Utf8(GetWindowText(encoder_double_click_key_edit_));
    if (ParseKeySpec(double_key).has_value()) {
        config_.encoder_double_click_key = double_key;
    } else {
        encoder_key_invalid = true;
    }
    if (encoder_key_invalid) {
        const auto msg_language = EffectiveUiLanguage(config_.ui_language);
        MessageBoxW(hwnd_, TrW(StringId::kSettingsEncoderInvalidKey, msg_language).c_str(),
                    TrW(StringId::kSettingsTitle, msg_language).c_str(), MB_OK | MB_ICONWARNING);
    }
```

并确认 `settings_dialog.cc` include 了 `key_spec.h`（没有则加）。

保存流程不变：`config_.Save()` → `on_config_changed` → `UpdateConfig` 即时下发 LED 颜色与门控（Task 8 已编排）。

- [ ] **Step 7: 编译 + 全量测试**

Run: `cmd //c "_wt_build.bat"` 然后 `cmd //c "_wt_ctest.bat"`
Expected: 编译成功，全部 CTest 过。

- [ ] **Step 8: 手动目视验证**

运行 `desktop\windows\build-x64\VoiceStick.exe`，打开设置：「编码器」一节出现 7 行控件；切换单击/双击动作组合框，对应按键编辑框启用/禁用正确；填入非法按键（如 `foo`）保存弹提示且不写回（重开对话框该字段为旧值）。

- [ ] **Step 9: Commit**

```bash
git add -f desktop/windows/src/localization.h desktop/windows/src/localization.cc desktop/windows/src/settings_dialog.h desktop/windows/src/settings_dialog.cc
git commit -m "feat(windows): 设置对话框新增编码器一节"
```

---

### Task 11: 文档同步（config.example.toml + AGENTS/CLAUDE）

**Files:**
- Modify: `desktop/macos/Config/config.example.toml`
- Modify: `AGENTS.md`（配置节）
- Modify: `CLAUDE.md`（配置节，与 AGENTS.md 同源同步）

- [ ] **Step 1: config.example.toml**

在 `desktop/macos/Config/config.example.toml` 的 `encoder_to_arrow`/`encoder_rotation_invert` 注释块（若无则找 `tap_to_arrow` 附近）追加：

```toml
# 编码器旋转顺时针/逆时针注入的按键（热键语法：单键 up/down/left/right/enter/tab/
# pageup/pagedown/volumeup/volumedown/f1-f24/单字符，或 ctrl/alt/shift/win 组合）
# encoder_rotate_cw_key = "down"
# encoder_rotate_ccw_key = "up"
# 编码器录音灯颜色：red/green/blue/yellow/purple/cyan/white/off（off=录音也不亮）。
# 保存设置时经 BLE 下发固件并 NVS 持久化。
# encoder_led_color = "red"
# 编码器单击动作：recording（同主键录音语义）或 key（注入 encoder_press_key）。
# 配 key 时桌面端会下发门控关闭固件侧录音（长按旋钮只发按键事件）。
# encoder_press_action = "recording"
# encoder_press_key = ""
# 编码器双击动作：key（注入 encoder_double_click_key）或 recording（双击开始/停止录音）。
# encoder_double_click_action = "key"
# encoder_double_click_key = "enter"
```

- [ ] **Step 2: AGENTS.md / CLAUDE.md 配置节**

两文件的「关键配置项」列表中 `tap_to_arrow` 条目之后加一条：

```markdown
- `encoder_to_arrow` / `encoder_rotation_invert` / `encoder_rotate_cw_key` / `encoder_rotate_ccw_key`：编码器旋转注入开关、方向翻转与 cw/ccw 自定义按键（热键语法）。
- `encoder_led_color`：编码器录音灯颜色（red/.../off），BLE 下发固件 NVS 持久化。
- `encoder_press_action` / `encoder_press_key` / `encoder_double_click_action` / `encoder_double_click_key`：编码器单击/双击动作（recording|key）与自定义按键；`press_action=key` 派生固件录音门控关闭，双击 recording 走 remote_button 切换起停。
```

（AGENTS.md 与 CLAUDE.md 内容同源，两处同样位置同步追加。）

- [ ] **Step 3: Commit**

```bash
git add -f desktop/macos/Config/config.example.toml
git add AGENTS.md CLAUDE.md
git commit -m "docs: 编码器设置项配置文档同步"
```

---

### Task 12: 全量验证 + 真机清单

**Files:** 无（纯验证）

- [ ] **Step 1: Windows 全量测试**

Run: `cmd //c "_wt_build.bat"` 然后 `cmd //c "_wt_ctest.bat"`
Expected: 编译成功，全部 CTest 通过（`voicestick_windows_tests` 全绿；`voicestick_integration_tests` 无 key 时 SKIP 属正常）。

- [ ] **Step 2: 固件编译**

Run: `python scripts/idf_cli.py -c`
Expected: build 成功。

- [ ] **Step 3: 真机验证清单（需 VS-53A8 + MiniEncoderC Hat，按 sticks3-flash-ota 技能 BLE OTA 或串口烧录新固件）**

逐项确认并记录结果：

1. **source 标签**：编码器单击/双击/长按，Windows 日志（`%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`）可见事件；物理主键/侧键事件无 source（行为与合并前一致）。
2. **LED 换色即时生效**：设置改灯色保存 → 下次录音亮新色；改 `off` → 录音不亮灯。
3. **NVS 保持**：改灯色后设备重启（EN 复位），录音仍亮新色。
4. **门控关闭**：`press_action=key`（如 `ctrl+z`）保存后，长按旋钮 3 秒——不录音、不亮录音灯、桌面无浮窗；松开注入一次 `ctrl+z`；双击仍按双击配置生效。
5. **门控恢复**：改回 `press_action=recording`，编码器长按录音恢复（红灯亮、桌面浮窗）。
6. **双击 recording**：配 `double_click_action=recording`，空闲双击开播、再双击停播（remote_button 通道）。
7. **旋转自定义键**：配 cw=`pagedown`/ccw=`pageup`，在文本编辑器旋转验证；invert 后方向对调。
8. **旧配置兼容**：用不含新键的旧 config.toml 启动，行为与合并前完全一致（默认值路径）。
9. **物理键回归**：物理主键 hold_to_talk 录音、双击 Enter、侧键单击体感/双击恢复——与合并前一致。

- [ ] **Step 4: 收尾**

真机清单全过后向用户汇报，按用户指示合并 `feat/encoder-settings` → main（合并流程参照 feat/mini-encoder-c：临时 worktree 验证编译+CTest 再合并）。
