#include "voice_ble_conn_table.h"

#include <string.h>

void voice_ble_conn_table_init(voice_ble_conn_table_t *table)
{
    memset(table, 0, sizeof(*table));
}

int voice_ble_conn_table_count(const voice_ble_conn_table_t *table)
{
    int count = 0;
    for (int i = 0; i < VOICE_BLE_CONN_MAX; ++i) {
        if (table->entries[i].used) {
            ++count;
        }
    }
    return count;
}

bool voice_ble_conn_table_add(voice_ble_conn_table_t *table, uint16_t handle,
                              uint32_t connected_ms, bool *out_first)
{
    if (out_first) {
        *out_first = voice_ble_conn_table_count(table) == 0;
    }
    voice_ble_conn_t *existing = voice_ble_conn_table_find(table, handle);
    if (existing) {
        // 幂等：重连同 handle 保留已有订阅态（SUBSCRIBE 事件可能晚于重复 CONNECT）。
        return true;
    }
    for (int i = 0; i < VOICE_BLE_CONN_MAX; ++i) {
        if (!table->entries[i].used) {
            voice_ble_conn_t *e = &table->entries[i];
            memset(e, 0, sizeof(*e));
            e->handle = handle;
            e->connected_ms = connected_ms;
            e->used = true;
            return true;
        }
    }
    return false;
}

bool voice_ble_conn_table_remove(voice_ble_conn_table_t *table, uint16_t handle,
                                 bool *out_last)
{
    voice_ble_conn_t *e = voice_ble_conn_table_find(table, handle);
    if (!e) {
        return false;
    }
    memset(e, 0, sizeof(*e));
    if (out_last) {
        *out_last = voice_ble_conn_table_count(table) == 0;
    }
    return true;
}

voice_ble_conn_t *voice_ble_conn_table_find(voice_ble_conn_table_t *table,
                                            uint16_t handle)
{
    for (int i = 0; i < VOICE_BLE_CONN_MAX; ++i) {
        if (table->entries[i].used && table->entries[i].handle == handle) {
            return &table->entries[i];
        }
    }
    return NULL;
}

voice_ble_conn_t *voice_ble_conn_table_touch(voice_ble_conn_table_t *table,
                                             uint16_t handle,
                                             uint32_t connected_ms)
{
    voice_ble_conn_t *e = voice_ble_conn_table_find(table, handle);
    if (e) {
        return e;
    }
    if (!voice_ble_conn_table_add(table, handle, connected_ms, NULL)) {
        return NULL;
    }
    return voice_ble_conn_table_find(table, handle);
}

uint16_t voice_ble_conn_table_app_handle(const voice_ble_conn_table_t *table)
{
    uint16_t fallback = VOICE_BLE_CONN_NONE;
    for (int i = 0; i < VOICE_BLE_CONN_MAX; ++i) {
        const voice_ble_conn_t *e = &table->entries[i];
        if (!e->used || (!e->state_sub && !e->audio_sub)) {
            continue;
        }
        if (e->state_sub) {
            return e->handle;  // state 订阅者优先（桌面 app 必订 state）
        }
        if (fallback == VOICE_BLE_CONN_NONE) {
            fallback = e->handle;
        }
    }
    return fallback;
}
