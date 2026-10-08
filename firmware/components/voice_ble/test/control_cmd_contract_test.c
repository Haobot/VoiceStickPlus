// 固件端契约 reader（0.1 最后一端，2026-10-08）：
// 消费 tests/contract/fixtures/manifest.json——桌面端黄金样本（expect 对象）经
// control_cmd_parse（与 main/voice_ble 执行侧同一解析代码）→ 字段语义必须与契约一致。
// 反向门用例固化「未知事件静默、缺字段 fallthrough、非法 JSON 拒绝」的原链语义。
//
// 运行：由 run_tests.py 设置 VOICESTICK_REPO_ROOT；直跑需手动 export。
// 编译：cc -std=c11 -Wall -Wextra -Werror -I <voice_ble/include> -I <cJSON>
//        control_cmd_contract_test.c ../control_cmd.c <cJSON.c> -o t；运行前 export
//        VOICESTICK_REPO_ROOT=<仓库根>（run_tests.py 已自动注入）。
#include "control_cmd.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            printf("FAIL [%s] %s (line %d)\n", __func__, msg, __LINE__);      \
            fflush(stdout);                                                  \
        }                                                                    \
    } while (0)

// expect.event → 固件命令 kind（无 event 则看 power_log.cmd）。
static control_cmd_kind_t kind_from_contract(const cJSON *expect)
{
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(expect, "event");
    const char *ev = cJSON_IsString(event) ? event->valuestring : NULL;
    if (ev == NULL) {
        const cJSON *plog = cJSON_GetObjectItemCaseSensitive(expect, "power_log");
        const cJSON *cmd = plog ? cJSON_GetObjectItemCaseSensitive(plog, "cmd") : NULL;
        if (!cJSON_IsString(cmd)) return CONTROL_CMD_NONE;
        if (strcmp(cmd->valuestring, "dump") == 0) return CONTROL_CMD_POWER_LOG_DUMP;
        if (strcmp(cmd->valuestring, "clear") == 0) return CONTROL_CMD_POWER_LOG_CLEAR;
        if (strcmp(cmd->valuestring, "time_anchor") == 0) return CONTROL_CMD_POWER_LOG_TIME_ANCHOR;
        return CONTROL_CMD_POWER_LOG_UNKNOWN;
    }
    if (strcmp(ev, "ui_state") == 0) return CONTROL_CMD_UI_STATE;
    if (strcmp(ev, "interaction_mode") == 0) return CONTROL_CMD_INTERACTION_MODE;
    if (strcmp(ev, "show_imu_debug") == 0) return CONTROL_CMD_SHOW_IMU_DEBUG;
    if (strcmp(ev, "battery_status_request") == 0) return CONTROL_CMD_BATTERY_STATUS_REQUEST;
    if (strcmp(ev, "usb_auto_off") == 0) return CONTROL_CMD_USB_AUTO_OFF;
    if (strcmp(ev, "usb_auto_off_get") == 0) return CONTROL_CMD_USB_AUTO_OFF_GET;
    if (strcmp(ev, "gateway_select_target") == 0) return CONTROL_CMD_GATEWAY_SELECT_TARGET;
    if (strcmp(ev, "gateway_side_switch") == 0) return CONTROL_CMD_GATEWAY_SIDE_SWITCH;
    if (strcmp(ev, "gateway_target_info") == 0) return CONTROL_CMD_GATEWAY_TARGET_INFO;
    if (strcmp(ev, "remote_button_down") == 0) return CONTROL_CMD_REMOTE_BUTTON_DOWN;
    if (strcmp(ev, "remote_button_up") == 0) return CONTROL_CMD_REMOTE_BUTTON_UP;
    if (strcmp(ev, "ota_commit") == 0) return CONTROL_CMD_OTA_COMMIT;
    if (strcmp(ev, "imu_wake_sensitivity") == 0) return CONTROL_CMD_IMU_WAKE_SENSITIVITY;
    if (strcmp(ev, "tap_enabled") == 0) return CONTROL_CMD_TAP_ENABLED;
    if (strcmp(ev, "tap_sensitivity") == 0) return CONTROL_CMD_TAP_SENSITIVITY;
    if (strcmp(ev, "encoder_led_color") == 0) return CONTROL_CMD_ENCODER_LED_COLOR;
    if (strcmp(ev, "encoder_recording_gate") == 0) return CONTROL_CMD_ENCODER_RECORDING_GATE;
    if (strcmp(ev, "air_mouse_enabled") == 0) return CONTROL_CMD_AIR_MOUSE_ENABLED;
    if (strcmp(ev, "test_playback") == 0) return CONTROL_CMD_TEST_PLAYBACK;
    if (strcmp(ev, "gateway_keymap_set") == 0) return CONTROL_CMD_GATEWAY_KEYMAP_SET;
    if (strcmp(ev, "gateway_keymap_get") == 0) return CONTROL_CMD_GATEWAY_KEYMAP_GET;
    return CONTROL_CMD_NONE;
}

static const char *expect_str(const cJSON *expect, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(expect, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int expect_has_bool(const cJSON *expect, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(expect, key);
    if (!cJSON_IsBool(v)) return 0;
    *out = cJSON_IsTrue(v);
    return 1;
}

static long expect_num(const cJSON *expect, const char *key, long fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(expect, key);
    return cJSON_IsNumber(v) ? (long)v->valuedouble : fallback;
}

static void check_one_control(const char *name, const cJSON *expect)
{
    char *printed = cJSON_PrintUnformatted((cJSON *)expect);
    if (printed == NULL) {
        CHECK(0, "cJSON_PrintUnformatted failed");
        return;
    }
    control_cmd_t cmd;
    const bool parsed = control_cmd_parse(printed, &cmd);
    if (!parsed) {
        printf("FAIL [%s] parse returned false: %s\n", name, printed);
        ++g_failed;
        free(printed);
        return;
    }
    const control_cmd_kind_t want_kind = kind_from_contract(expect);
    CHECK(cmd.kind == want_kind, "kind mismatch");

    // 公共：event 字段（expect 有 event 时）→ 逐 kind 字段。
    if (want_kind == CONTROL_CMD_UI_STATE) {
        const char *state = expect_str(expect, "state");
        const char *text = expect_str(expect, "text");
        CHECK(state != NULL && strcmp(cmd.str1, state) == 0, "state mismatch");
        CHECK(text != NULL && strcmp(cmd.text, text) == 0, "text mismatch");
    } else if (want_kind == CONTROL_CMD_INTERACTION_MODE ||
               want_kind == CONTROL_CMD_ENCODER_LED_COLOR ||
               want_kind == CONTROL_CMD_GATEWAY_TARGET_INFO ||
               want_kind == CONTROL_CMD_GATEWAY_SIDE_SWITCH) {
        const char *key = (want_kind == CONTROL_CMD_INTERACTION_MODE) ? "mode"
            : (want_kind == CONTROL_CMD_ENCODER_LED_COLOR) ? "color"
            : (want_kind == CONTROL_CMD_GATEWAY_TARGET_INFO) ? "name" : "action";
        const char *v = expect_str(expect, key);
        CHECK(v != NULL && cmd.has_str1 && strcmp(cmd.str1, v) == 0, "str1 mismatch");
    } else if (want_kind == CONTROL_CMD_SHOW_IMU_DEBUG ||
               want_kind == CONTROL_CMD_TAP_ENABLED ||
               want_kind == CONTROL_CMD_ENCODER_RECORDING_GATE ||
               want_kind == CONTROL_CMD_AIR_MOUSE_ENABLED ||
               want_kind == CONTROL_CMD_USB_AUTO_OFF) {
        bool want = false;
        CHECK(expect_has_bool(expect, "enabled", &want), "enabled missing in contract");
        CHECK(cmd.enabled == want, "enabled mismatch");
    } else if (want_kind == CONTROL_CMD_IMU_WAKE_SENSITIVITY) {
        CHECK(cmd.has_number && cmd.value == (int32_t)expect_num(expect, "threshold", -1),
              "threshold mismatch");
    } else if (want_kind == CONTROL_CMD_TAP_SENSITIVITY) {
        CHECK(cmd.has_number && cmd.value == (int32_t)expect_num(expect, "level", -1),
              "level mismatch");
    } else if (want_kind == CONTROL_CMD_GATEWAY_KEYMAP_SET) {
        const char *key = expect_str(expect, "key");
        const char *route = expect_str(expect, "route");
        CHECK(key != NULL && cmd.has_str1 && strcmp(cmd.str1, key) == 0, "key mismatch");
        CHECK(route != NULL && cmd.has_str2 && strcmp(cmd.str2, route) == 0, "route mismatch");
    } else if (want_kind == CONTROL_CMD_REMOTE_BUTTON_DOWN ||
               want_kind == CONTROL_CMD_REMOTE_BUTTON_UP) {
        const char *button = expect_str(expect, "button");
        CHECK(button != NULL && cmd.has_str1 && strcmp(cmd.str1, button) == 0, "button mismatch");
        CHECK(cmd.value == (int32_t)expect_num(expect, "request_id", -1), "request_id mismatch");
    } else if (want_kind == CONTROL_CMD_POWER_LOG_DUMP) {
        const cJSON *plog = cJSON_GetObjectItemCaseSensitive(expect, "power_log");
        const cJSON *off = plog ? cJSON_GetObjectItemCaseSensitive(plog, "offset") : NULL;
        const cJSON *mx = plog ? cJSON_GetObjectItemCaseSensitive(plog, "max") : NULL;
        CHECK(cJSON_IsNumber(off) && cmd.value == (int32_t)off->valuedouble, "offset mismatch");
        CHECK(cJSON_IsNumber(mx) && cmd.uvalue == (uint32_t)mx->valuedouble, "max mismatch");
    }
    // 无字段事件（battery_status_request/power_log_clear 等）：kind 断言已覆盖。
    free(printed);
}

static void check_gates(void)
{
    control_cmd_t cmd;
    // 非法 JSON → false。
    CHECK(!control_cmd_parse("{not json", &cmd), "invalid json must fail");
    CHECK(!control_cmd_parse(NULL, &cmd), "null must fail");
    // 未知事件 → true + NONE（静默，等价原链 fallthrough）。
    CHECK(control_cmd_parse("{\"event\":\"no_such_event\"}", &cmd) && cmd.kind == CONTROL_CMD_NONE,
          "unknown event -> NONE");
    // ui_state 缺 state → NONE（原链 IsString(state) 门 fallthrough）。
    CHECK(control_cmd_parse("{\"event\":\"ui_state\",\"text\":\"x\"}", &cmd) &&
              cmd.kind == CONTROL_CMD_NONE,
          "ui_state without state -> NONE");
    // remote_button 非 primary → NONE（原链 button 门）。
    CHECK(control_cmd_parse(
              "{\"event\":\"remote_button_down\",\"button\":\"secondary\"}", &cmd) &&
              cmd.kind == CONTROL_CMD_NONE,
          "remote non-primary -> NONE");
    // show_imu_debug 非 bool → NONE（原链 IsBool 门）。
    CHECK(control_cmd_parse("{\"event\":\"show_imu_debug\",\"enabled\":1}", &cmd) &&
              cmd.kind == CONTROL_CMD_NONE,
          "non-bool enabled -> NONE");
    printf("check_gates: %d checks done\n", 4);
}

int main(void)
{
    const char *root = getenv("VOICESTICK_REPO_ROOT");
    if (root == NULL || root[0] == '\0') {
        printf("SKIP: set VOICESTICK_REPO_ROOT to the repository root\n");
        return 77;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/tests/contract/fixtures/manifest.json", root);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("FAIL: cannot open %s\n", path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)size + 1);
    if (text == NULL || fread(text, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        printf("FAIL: cannot read %s\n", path);
        return 1;
    }
    text[size] = '\0';
    fclose(fp);

    cJSON *manifest = cJSON_Parse(text);
    free(text);
    if (manifest == NULL) {
        printf("FAIL: manifest JSON parse failed\n");
        return 1;
    }

    const cJSON *controls = cJSON_GetObjectItemCaseSensitive(manifest, "control_payloads");
    int count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, controls) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(item, "args");
        const cJSON *expect = cJSON_GetObjectItemCaseSensitive(item, "expect");
        if (!cJSON_IsString(name) || args == NULL || expect == NULL) {
            CHECK(0, "manifest control entry malformed");
            continue;
        }
        if (args == NULL) {
            CHECK(0, "manifest control entry missing args");
        }
        check_one_control(name->valuestring, expect);
        ++count;
    }
    printf("contract: %d control payload(s) checked against firmware parser\n", count);
    CHECK(count > 0, "no control_payloads in manifest");

    check_gates();
    cJSON_Delete(manifest);

    if (g_failed != 0) {
        printf("control_cmd_contract_test: %d FAILURE(S)\n", g_failed);
        return 1;
    }
    printf("control_cmd_contract_test: ALL PASS\n");
    return 0;
}
