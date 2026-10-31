#include "control_cmd.h"

#include "cJSON.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 与原 main.c ble_control_cb 的 if-else 链逐分支等价（含前置有效性门与优先级序），
// 副作用全部留在执行侧。详见 control_cmd.h 的行为契约注释。

static void copy_str(char *dst, size_t cap, const cJSON *item)
{
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(dst, cap, "%s", item->valuestring);
    }
}

bool control_cmd_parse(const char *json, control_cmd_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->kind = CONTROL_CMD_NONE;
    if (json == NULL) {
        return false;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }

    // power_log 命令族（voice_ble 内部执行；main 收到后按 kind 静默忽略，等价原行为）。
    const cJSON *plog = cJSON_GetObjectItemCaseSensitive(root, "power_log");
    if (plog != NULL) {
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(plog, "cmd");
        if (cJSON_IsString(cmd)) {
            if (strcmp(cmd->valuestring, "dump") == 0) {
                out->kind = CONTROL_CMD_POWER_LOG_DUMP;
                const cJSON *off = cJSON_GetObjectItemCaseSensitive(plog, "offset");
                const cJSON *mx = cJSON_GetObjectItemCaseSensitive(plog, "max");
                if (cJSON_IsNumber(off) && off->valuedouble > 0) {
                    out->value = (int32_t)off->valuedouble;
                }
                if (cJSON_IsNumber(mx) && mx->valuedouble > 0) {
                    out->uvalue = (uint32_t)mx->valuedouble;
                }
            } else if (strcmp(cmd->valuestring, "clear") == 0) {
                out->kind = CONTROL_CMD_POWER_LOG_CLEAR;
            } else if (strcmp(cmd->valuestring, "time_anchor") == 0) {
                out->kind = CONTROL_CMD_POWER_LOG_TIME_ANCHOR;
                const cJSON *epoch = cJSON_GetObjectItemCaseSensitive(plog, "epoch");
                if (cJSON_IsNumber(epoch)) {
                    double v = epoch->valuedouble;
                    if (v > INT32_MAX) {
                        v = INT32_MAX;
                    } else if (v < INT32_MIN) {
                        v = INT32_MIN;
                    }
                    out->value = (int32_t)v;
                    out->has_number = true;
                }
            } else {
                out->kind = CONTROL_CMD_POWER_LOG_UNKNOWN;
                copy_str(out->str1, sizeof(out->str1), cmd);
            }
        }
        // cmd 缺失/非字符串 → kind 保持 NONE（等价原实现的静默 return）。
        cJSON_Delete(root);
        return true;
    }

    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *button = cJSON_GetObjectItemCaseSensitive(root, "button");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *threshold_item = cJSON_GetObjectItemCaseSensitive(root, "threshold");
    const cJSON *request_id_json = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    int32_t request_id = 0;
    if (cJSON_IsNumber(request_id_json)) {
        request_id = (int32_t)request_id_json->valuedouble;
    }

    if (cJSON_IsString(event) && strcmp(event->valuestring, "ui_state") == 0 &&
        cJSON_IsString(state)) {
        out->kind = CONTROL_CMD_UI_STATE;
        copy_str(out->str1, sizeof(out->str1), state);
        copy_str(out->text, sizeof(out->text), cJSON_IsString(text) ? text : NULL);
        out->has_str1 = true;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "interaction_mode") == 0 &&
               cJSON_IsString(mode)) {
        out->kind = CONTROL_CMD_INTERACTION_MODE;
        copy_str(out->str1, sizeof(out->str1), mode);
        out->has_str1 = true;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "show_imu_debug") == 0 &&
               cJSON_IsBool(enabled)) {
        out->kind = CONTROL_CMD_SHOW_IMU_DEBUG;
        out->enabled = cJSON_IsTrue(enabled);
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "battery_status_request") == 0) {
        out->kind = CONTROL_CMD_BATTERY_STATUS_REQUEST;
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "usb_auto_off") == 0 &&
               cJSON_IsBool(enabled)) {
        out->kind = CONTROL_CMD_USB_AUTO_OFF;
        out->enabled = cJSON_IsTrue(enabled);
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "gateway_select_target") == 0) {
        out->kind = CONTROL_CMD_GATEWAY_SELECT_TARGET;
        const cJSON *index_json = cJSON_GetObjectItemCaseSensitive(root, "index");
        const cJSON *clear = cJSON_GetObjectItemCaseSensitive(root, "clear");
        const cJSON *self = cJSON_GetObjectItemCaseSensitive(root, "self");
        if (cJSON_IsBool(clear) && cJSON_IsTrue(clear)) {
            out->flag_clear = true;
        } else if (cJSON_IsBool(self) && cJSON_IsTrue(self)) {
            out->flag_self = true;
        } else if (cJSON_IsNumber(index_json)) {
            out->value = index_json->valueint;
            out->has_number = true;
        }
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "gateway_side_switch") == 0) {
        out->kind = CONTROL_CMD_GATEWAY_SIDE_SWITCH;
        copy_str(out->str1, sizeof(out->str1),
                 cJSON_GetObjectItemCaseSensitive(root, "action"));
        out->has_str1 = cJSON_IsString(
            cJSON_GetObjectItemCaseSensitive(root, "action")) != 0;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "gateway_target_info") == 0) {
        out->kind = CONTROL_CMD_GATEWAY_TARGET_INFO;
        copy_str(out->str1, sizeof(out->str1),
                 cJSON_GetObjectItemCaseSensitive(root, "name"));
        out->has_str1 = cJSON_IsString(
            cJSON_GetObjectItemCaseSensitive(root, "name")) != 0;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "usb_auto_off_get") == 0) {
        out->kind = CONTROL_CMD_USB_AUTO_OFF_GET;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "remote_button_down") == 0 &&
               cJSON_IsString(button) && strcmp(button->valuestring, "primary") == 0) {
        out->kind = CONTROL_CMD_REMOTE_BUTTON_DOWN;
        copy_str(out->str1, sizeof(out->str1), button);
        out->has_str1 = true;
        out->value = request_id;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "remote_button_up") == 0 &&
               cJSON_IsString(button) && strcmp(button->valuestring, "primary") == 0) {
        out->kind = CONTROL_CMD_REMOTE_BUTTON_UP;
        copy_str(out->str1, sizeof(out->str1), button);
        out->has_str1 = true;
        out->value = request_id;
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "ota_commit") == 0) {
        out->kind = CONTROL_CMD_OTA_COMMIT;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "imu_wake_sensitivity") == 0 &&
               cJSON_IsNumber(threshold_item)) {
        out->kind = CONTROL_CMD_IMU_WAKE_SENSITIVITY;
        double v = threshold_item->valuedouble;
        if (v > INT32_MAX) {
            v = INT32_MAX;
        } else if (v < INT32_MIN) {
            v = INT32_MIN;
        }
        out->value = (int32_t)v;
        out->has_number = true;
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "tap_enabled") == 0 &&
               cJSON_IsBool(enabled)) {
        out->kind = CONTROL_CMD_TAP_ENABLED;
        out->enabled = cJSON_IsTrue(enabled);
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "encoder_led_color") == 0) {
        out->kind = CONTROL_CMD_ENCODER_LED_COLOR;
        copy_str(out->str1, sizeof(out->str1),
                 cJSON_GetObjectItemCaseSensitive(root, "color"));
        out->has_str1 = cJSON_IsString(
            cJSON_GetObjectItemCaseSensitive(root, "color")) != 0;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "encoder_recording_gate") == 0 &&
               cJSON_IsBool(enabled)) {
        out->kind = CONTROL_CMD_ENCODER_RECORDING_GATE;
        out->enabled = cJSON_IsTrue(enabled);
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "tap_sensitivity") == 0) {
        out->kind = CONTROL_CMD_TAP_SENSITIVITY;
        const cJSON *level_item = cJSON_GetObjectItemCaseSensitive(root, "level");
        if (cJSON_IsNumber(level_item)) {
            out->value = level_item->valueint;
            out->has_number = true;
        } else {
            copy_str(out->str1, sizeof(out->str1), level_item);
            out->has_str1 = cJSON_IsString(level_item) != 0;
        }
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "air_mouse_enabled") == 0 &&
               cJSON_IsBool(enabled)) {
        out->kind = CONTROL_CMD_AIR_MOUSE_ENABLED;
        out->enabled = cJSON_IsTrue(enabled);
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "test_playback") == 0) {
        out->kind = CONTROL_CMD_TEST_PLAYBACK;
        copy_str(out->str1, sizeof(out->str1),
                 cJSON_GetObjectItemCaseSensitive(root, "file"));
        out->has_str1 = cJSON_IsString(
            cJSON_GetObjectItemCaseSensitive(root, "file")) != 0;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "gateway_keymap_set") == 0) {
        out->kind = CONTROL_CMD_GATEWAY_KEYMAP_SET;
        const cJSON *key_item = cJSON_GetObjectItemCaseSensitive(root, "key");
        const cJSON *route_item = cJSON_GetObjectItemCaseSensitive(root, "route");
        copy_str(out->str1, sizeof(out->str1), key_item);
        copy_str(out->str2, sizeof(out->str2), route_item);
        out->has_str1 = cJSON_IsString(key_item) != 0;
        out->has_str2 = cJSON_IsString(route_item) != 0;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "gateway_keymap_get") == 0) {
        out->kind = CONTROL_CMD_GATEWAY_KEYMAP_GET;
    } else if (cJSON_IsString(event) &&
               strcmp(event->valuestring, "proto_negotiate") == 0) {
        // D9：{"event":"proto_negotiate","proto":N}——桌面端协议版本上报。
        out->kind = CONTROL_CMD_PROTO_NEGOTIATE;
        const cJSON *proto_item = cJSON_GetObjectItemCaseSensitive(root, "proto");
        if (cJSON_IsNumber(proto_item)) {
            out->value = proto_item->valueint;
            out->has_number = true;
        }
    }
    // 其余未知 event → kind 保持 NONE（静默忽略，等价原链 fallthrough）。

    cJSON_Delete(root);
    return true;
}
