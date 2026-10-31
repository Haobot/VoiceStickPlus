// voice_ble 连接表宿主编译单测（A4）：不依赖 ESP/NimBLE。
// 本地：cc -Wall -Wextra -Werror -I../include -o /tmp/conn_table_test conn_table_test.c ../conn_table.c && /tmp/conn_table_test
#include "voice_ble_conn_table.h"

#include <assert.h>
#include <stdio.h>

static void test_basic_lifecycle(void)
{
    voice_ble_conn_table_t t;
    voice_ble_conn_table_init(&t);
    assert(voice_ble_conn_table_count(&t) == 0);
    assert(voice_ble_conn_table_app_handle(&t) == VOICE_BLE_CONN_NONE);

    bool first = false;
    assert(voice_ble_conn_table_add(&t, 1, 1000, &first) && first);

    voice_ble_conn_t *e = voice_ble_conn_table_find(&t, 1);
    assert(e && e->connected_ms == 1000 && !e->state_sub);

    // 幂等：重复 add 不重置订阅态。
    e->state_sub = true;
    assert(voice_ble_conn_table_add(&t, 1, 2000, &first));
    assert(!first);
    e = voice_ble_conn_table_find(&t, 1);
    assert(e && e->connected_ms == 1000 && e->state_sub);

    assert(voice_ble_conn_table_remove(&t, 1, &first));
    assert(first);  // 归零
    assert(!voice_ble_conn_table_remove(&t, 1, &first));  // 不存在
    printf("test_basic_lifecycle ok\n");
}

static void test_stale_disconnect_keeps_new_link(void)
{
    // 事故回归（gateway-p1-ota-session-2026-09-20 §4）：旧连接的 stale DISCONNECT
    // 迟到，不得清掉新连接的订阅态。
    voice_ble_conn_table_t t;
    voice_ble_conn_table_init(&t);
    bool last = false;
    assert(voice_ble_conn_table_add(&t, 0x10, 1, NULL));
    voice_ble_conn_t *a = voice_ble_conn_table_find(&t, 0x10);
    a->state_sub = true;
    assert(voice_ble_conn_table_app_handle(&t) == 0x10);

    // 重连换 handle（NimBLE 重连 handle 会变），订阅 B。
    assert(voice_ble_conn_table_add(&t, 0x20, 2, NULL));
    voice_ble_conn_t *b = voice_ble_conn_table_find(&t, 0x20);
    b->state_sub = true;

    // A 的 disconnect 迟到：只清 A。
    assert(voice_ble_conn_table_remove(&t, 0x10, &last));
    assert(!last);
    assert(voice_ble_conn_table_count(&t) == 1);
    assert(voice_ble_conn_table_app_handle(&t) == 0x20);  // 新链路完好

    assert(voice_ble_conn_table_remove(&t, 0x20, &last));
    assert(last);
    assert(voice_ble_conn_table_app_handle(&t) == VOICE_BLE_CONN_NONE);
    printf("test_stale_disconnect_keeps_new_link ok\n");
}

static void test_subscribe_resync_creates_entry(void)
{
    voice_ble_conn_table_t t;
    voice_ble_conn_table_init(&t);
    // 乱序：SUBSCRIBE 先于（丢失的）CONNECT 记录到达 → 补录。
    voice_ble_conn_t *e = voice_ble_conn_table_touch(&t, 0x30, 5);
    assert(e);
    e->state_sub = true;
    assert(voice_ble_conn_table_app_handle(&t) == 0x30);
    // 再 touch 幂等。
    assert(voice_ble_conn_table_touch(&t, 0x30, 6) == e);
    printf("test_subscribe_resync_creates_entry ok\n");
}

static void test_app_handle_preference(void)
{
    voice_ble_conn_table_t t;
    voice_ble_conn_table_init(&t);
    // OS-HID 中央（只 audio？HID 实际不订我们特征——用 audio-only 模拟第三方订阅者）。
    assert(voice_ble_conn_table_add(&t, 0x40, 1, NULL));
    voice_ble_conn_table_find(&t, 0x40)->audio_sub = true;
    assert(voice_ble_conn_table_app_handle(&t) == 0x40);  // audio-only 兜底
    // state 订阅者（真桌面 app）优先。
    assert(voice_ble_conn_table_add(&t, 0x50, 2, NULL));
    voice_ble_conn_table_find(&t, 0x50)->state_sub = true;
    assert(voice_ble_conn_table_app_handle(&t) == 0x50);
    printf("test_app_handle_preference ok\n");
}

static void test_capacity(void)
{
    voice_ble_conn_table_t t;
    voice_ble_conn_table_init(&t);
    for (int i = 0; i < VOICE_BLE_CONN_MAX; ++i) {
        assert(voice_ble_conn_table_add(&t, (uint16_t)i, 0, NULL));
    }
    assert(!voice_ble_conn_table_add(&t, 0x77, 0, NULL));      // 满
    assert(voice_ble_conn_table_touch(&t, 0x77, 0) == NULL);   // 满补录失败
    assert(voice_ble_conn_table_count(&t) == VOICE_BLE_CONN_MAX);
    printf("test_capacity ok\n");
}

int main(void)
{
    test_basic_lifecycle();
    test_stale_disconnect_keeps_new_link();
    test_subscribe_resync_creates_entry();
    test_app_handle_preference();
    test_capacity();
    printf("conn_table_test: ALL PASS\n");
    return 0;
}
