// gateway_targets_core.c — 网关目标表的纯逻辑核心（无 ESP-IDF 依赖，进 host 单测）。
//
// 表语义与调用约定见 include/gateway_targets.h。设备侧 NVS 封装在 gateway_targets.c。
#include "gateway_targets.h"

#include <stdio.h>
#include <string.h>

// 定长字符串复制：不用 strncpy（MSVC 视其为不安全并报 C4996，host 单测开了 /WX）。
static void copy_bounded(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    size_t i = 0;
    for (; src && src[i] != '\0' && i + 1 < dst_len; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void gateway_targets_core_init(gateway_target_table_t *table)
{
    memset(table, 0, sizeof(*table));
}

int gateway_targets_core_find(const gateway_target_table_t *table, const uint8_t id_addr[6],
                              uint8_t addr_type)
{
    for (int i = 0; i < table->count; i++) {
        if (table->items[i].addr_type == addr_type &&
            memcmp(table->items[i].id_addr, id_addr, 6) == 0) {
            return i;
        }
    }
    return -1;
}

int gateway_targets_core_note(gateway_target_table_t *table, const uint8_t id_addr[6],
                              uint8_t addr_type, uint32_t now_s)
{
    int index = gateway_targets_core_find(table, id_addr, addr_type);
    if (index < 0) {
        if (table->count < GATEWAY_TARGET_MAX) {
            index = table->count++;
        } else {
            // 满表：淘汰 last_seen_s 最小的一条（相同则取下标最小的，保证确定性）。
            int oldest = 0;
            for (int i = 1; i < table->count; i++) {
                if (table->items[i].last_seen_s < table->items[oldest].last_seen_s) {
                    oldest = i;
                }
            }
            index = oldest;
        }
        memset(&table->items[index], 0, sizeof(table->items[index]));
        memcpy(table->items[index].id_addr, id_addr, 6);
        table->items[index].addr_type = addr_type;
    }
    table->items[index].last_seen_s = now_s;
    return index;
}

bool gateway_targets_core_set_name(gateway_target_table_t *table, const uint8_t id_addr[6],
                                   uint8_t addr_type, const char *name)
{
    if (!name || name[0] == '\0' || strlen(name) >= GATEWAY_TARGET_NAME_MAX) {
        return false;
    }
    const int index = gateway_targets_core_find(table, id_addr, addr_type);
    if (index < 0) {
        return false;
    }
    copy_bounded(table->items[index].name, GATEWAY_TARGET_NAME_MAX, name);
    return true;
}

bool gateway_targets_core_remove(gateway_target_table_t *table, const uint8_t id_addr[6],
                                 uint8_t addr_type)
{
    const int index = gateway_targets_core_find(table, id_addr, addr_type);
    if (index < 0) {
        return false;
    }
    // 尾部前移补齐（顺序无关，保持数组紧凑）。
    for (int i = index; i + 1 < table->count; i++) {
        table->items[i] = table->items[i + 1];
    }
    table->count--;
    memset(&table->items[table->count], 0, sizeof(table->items[table->count]));
    return true;
}

void gateway_targets_core_display_name(const gateway_target_t *item, char *out, size_t out_len)
{
    if (!item || !out || out_len == 0) {
        return;
    }
    if (item->name[0] != '\0') {
        copy_bounded(out, out_len, item->name);
        return;
    }
    // 未命名：用地址后两字节区分（"未命名-53AA"）。
    snprintf(out, out_len, "未命名-%02X%02X", item->id_addr[4], item->id_addr[5]);
}
