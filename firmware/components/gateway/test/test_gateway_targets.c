// test_gateway_targets.c — 目标表纯逻辑 host 单测（P1 切换器）。
//
// 覆盖：新增/刷新/查找、满表淘汰最旧、命名与显示名、删除后紧凑、
// 地址类型区分（同地址不同 addr_type 视为不同目标）。
#include "gateway_targets.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
            failures++;                                                                \
        }                                                                              \
    } while (0)

static void addr(uint8_t out[6], uint8_t seed)
{
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)(seed + i);
    }
}

static void test_add_and_find(void)
{
    gateway_target_table_t t;
    gateway_targets_core_init(&t);
    uint8_t a[6];
    addr(a, 0x10);

    const int idx = gateway_targets_core_note(&t, a, 1, 100);
    CHECK(idx == 0);
    CHECK(t.count == 1);
    CHECK(gateway_targets_core_find(&t, a, 1) == 0);
    CHECK(gateway_targets_core_find(&t, a, 0) == -1);  // 地址类型不同 ⇒ 不同目标
    CHECK(t.items[0].last_seen_s == 100);

    // 再次 note：只刷新时间，不新增条目。
    CHECK(gateway_targets_core_note(&t, a, 1, 200) == 0);
    CHECK(t.count == 1);
    CHECK(t.items[0].last_seen_s == 200);
}

static void test_evict_oldest_when_full(void)
{
    gateway_target_table_t t;
    gateway_targets_core_init(&t);
    uint8_t a[6];
    for (int i = 0; i < GATEWAY_TARGET_MAX; i++) {
        addr(a, (uint8_t)(0x20 + i * 0x10));
        CHECK(gateway_targets_core_note(&t, a, 1, (uint32_t)(10 + i)) == i);
    }
    CHECK(t.count == GATEWAY_TARGET_MAX);

    // 第 5 个目标：应淘汰 last_seen 最小的下标 0。
    uint8_t oldest[6];
    addr(oldest, 0x20);
    uint8_t fresh[6];
    addr(fresh, 0x90);
    const int idx = gateway_targets_core_note(&t, fresh, 1, 999);
    CHECK(idx == 0);
    CHECK(t.count == GATEWAY_TARGET_MAX);
    CHECK(gateway_targets_core_find(&t, fresh, 1) == 0);
    CHECK(gateway_targets_core_find(&t, oldest, 1) == -1);
    CHECK(t.items[0].last_seen_s == 999);
    CHECK(t.items[0].name[0] == '\0');  // 淘汰后名字必须清空
}

static void test_naming(void)
{
    gateway_target_table_t t;
    gateway_targets_core_init(&t);
    uint8_t a[6];
    addr(a, 0x30);
    gateway_targets_core_note(&t, a, 1, 5);

    CHECK(!gateway_targets_core_set_name(&t, a, 1, ""));        // 空名无效
    CHECK(!gateway_targets_core_set_name(&t, a, 1, NULL));      // NULL 无效
    char too_long[GATEWAY_TARGET_NAME_MAX + 4];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(!gateway_targets_core_set_name(&t, a, 1, too_long));  // 超长无效

    CHECK(gateway_targets_core_set_name(&t, a, 1, "DESKTOP-01"));
    CHECK(strcmp(t.items[0].name, "DESKTOP-01") == 0);

    char display[32];
    gateway_targets_core_display_name(&t.items[0], display, sizeof(display));
    CHECK(strcmp(display, "DESKTOP-01") == 0);

    // 未命名目标：显示名带地址后两字节。
    uint8_t b[6];
    addr(b, 0x40);
    gateway_targets_core_note(&t, b, 1, 6);
    const int idx = gateway_targets_core_find(&t, b, 1);
    gateway_targets_core_display_name(&t.items[idx], display, sizeof(display));
    CHECK(strncmp(display, "未命名-", strlen("未命名-")) == 0);

    // 名字作用于未登记的目标：拒绝。
    uint8_t c[6];
    addr(c, 0x50);
    CHECK(!gateway_targets_core_set_name(&t, c, 1, "X"));
}

static void test_remove_compacts(void)
{
    gateway_target_table_t t;
    gateway_targets_core_init(&t);
    uint8_t a[6], b[6], c[6];
    addr(a, 0x60);
    addr(b, 0x70);
    addr(c, 0x80);
    gateway_targets_core_note(&t, a, 1, 1);
    gateway_targets_core_note(&t, b, 1, 2);
    gateway_targets_core_note(&t, c, 1, 3);

    CHECK(gateway_targets_core_remove(&t, b, 1));
    CHECK(t.count == 2);
    CHECK(gateway_targets_core_find(&t, b, 1) == -1);
    CHECK(gateway_targets_core_find(&t, a, 1) == 0);
    CHECK(gateway_targets_core_find(&t, c, 1) == 1);  // c 前移到 1
    CHECK(!gateway_targets_core_remove(&t, b, 1));    // 重复删除返回 false

    // 删空后仍可继续添加。
    CHECK(gateway_targets_core_remove(&t, a, 1));
    CHECK(gateway_targets_core_remove(&t, c, 1));
    CHECK(t.count == 0);
    CHECK(gateway_targets_core_note(&t, a, 1, 9) == 0);
    CHECK(t.count == 1);
}

int main(void)
{
    test_add_and_find();
    test_evict_oldest_when_full();
    test_naming();
    test_remove_compacts();
    if (failures == 0) {
        printf("test_gateway_targets: ALL PASS\n");
        return 0;
    }
    printf("test_gateway_targets: %d failure(s)\n", failures);
    return 1;
}
