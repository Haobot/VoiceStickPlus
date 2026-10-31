// test_gateway_switcher.c — 切换器纯逻辑 host 单测（P1 目标切换器）。
#include "gateway_switcher.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static gateway_switcher_peer_t make_peer(uint8_t seed, uint8_t type)
{
    gateway_switcher_peer_t p;
    for (int i = 0; i < 6; i++) {
        p.addr[i] = (uint8_t)(seed + i);
    }
    p.addr_type = type;
    return p;
}

static bool has(const gateway_switcher_actions_t *a, gateway_switcher_action_t action)
{
    for (int i = 0; i < a->count; i++) {
        if (a->items[i] == action) return true;
    }
    return false;
}

static void test_idle_accepts_anyone(void)
{
    gateway_switcher_t s;
    gateway_switcher_actions_t a;
    gateway_switcher_peer_t p = make_peer(0x10, 1);
    gateway_switcher_init(&s);
    CHECK(s.state == GATEWAY_SWITCHER_IDLE);
    CHECK(s.timeout_ms == GATEWAY_SWITCHER_TIMEOUT_MS);

    gateway_switcher_peer_connected(&s, &p, 100, &a);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_ACCEPT_PEER));
    CHECK(!has(&a, GATEWAY_SWITCHER_ACTION_REJECT_PEER));
    CHECK(s.current_valid);
    CHECK(s.state == GATEWAY_SWITCHER_IDLE);  /* IDLE 下连接不改状态 */
}

static void test_switch_happy_path(void)
{
    gateway_switcher_t s;
    gateway_switcher_actions_t a;
    gateway_switcher_peer_t a_peer = make_peer(0x20, 1);
    gateway_switcher_peer_t b_peer = make_peer(0x30, 1);
    gateway_switcher_init(&s);

    gateway_switcher_peer_connected(&s, &a_peer, 0, &a);  /* 默认行为下先连上 A */

    gateway_switcher_select(&s, &b_peer, 1000, &a);
    CHECK(s.state == GATEWAY_SWITCHER_SWITCHING);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_DISCONNECT_CURRENT));
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_START_ADV));
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_UI_SWITCHING));
    CHECK(s.previous_valid);  /* 记录 A 作为回退点 */

    gateway_switcher_peer_disconnected(&s, 1100, &a);  /* 旧目标断开：无额外动作 */
    CHECK(a.count == 0);

    gateway_switcher_peer_connected(&s, &b_peer, 1500, &a);
    CHECK(s.state == GATEWAY_SWITCHER_CONNECTED);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_ACCEPT_PEER));
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_UI_CONNECTED));
    CHECK(!s.previous_valid);
    CHECK(gateway_switcher_peer_equal(&s.current, &b_peer));

    gateway_switcher_select(&s, &b_peer, 2000, &a);  /* 再选同一目标：只刷 UI */
    CHECK(a.count == 1);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_UI_CONNECTED));
    CHECK(!has(&a, GATEWAY_SWITCHER_ACTION_DISCONNECT_CURRENT));
    CHECK(s.state == GATEWAY_SWITCHER_CONNECTED);
}

static void test_non_target_rejected(void)
{
    gateway_switcher_t s;
    gateway_switcher_actions_t a;
    gateway_switcher_peer_t target = make_peer(0x40, 1);
    gateway_switcher_peer_t intruder = make_peer(0x50, 1);
    gateway_switcher_init(&s);

    gateway_switcher_select(&s, &target, 0, &a);
    gateway_switcher_peer_connected(&s, &intruder, 100, &a);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_REJECT_PEER));
    CHECK(!has(&a, GATEWAY_SWITCHER_ACTION_ACCEPT_PEER));
    CHECK(s.state == GATEWAY_SWITCHER_SWITCHING);
    CHECK(!s.current_valid);

    gateway_switcher_peer_connected(&s, &target, 200, &a);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_ACCEPT_PEER));
    CHECK(s.state == GATEWAY_SWITCHER_CONNECTED);
}

static void test_timeout_rolls_back(void)
{
    gateway_switcher_t s;
    gateway_switcher_actions_t a;
    gateway_switcher_peer_t a_peer = make_peer(0x60, 1);
    gateway_switcher_peer_t b_peer = make_peer(0x70, 1);
    gateway_switcher_init(&s);

    gateway_switcher_peer_connected(&s, &a_peer, 0, &a);
    gateway_switcher_select(&s, &b_peer, 1000, &a);

    gateway_switcher_tick(&s, 1000 + GATEWAY_SWITCHER_TIMEOUT_MS - 1, &a);
    CHECK(a.count == 0);  /* 未到超时 */

    gateway_switcher_tick(&s, 1000 + GATEWAY_SWITCHER_TIMEOUT_MS, &a);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_UI_ERROR));
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_START_ADV));
    CHECK(s.state == GATEWAY_SWITCHER_SWITCHING);
    CHECK(gateway_switcher_peer_equal(&s.selected, &a_peer));  /* 已回退到 A */
    CHECK(!s.previous_valid);

    gateway_switcher_peer_connected(&s, &a_peer, 9000, &a);
    CHECK(s.state == GATEWAY_SWITCHER_CONNECTED);
}

static void test_timeout_without_previous_goes_idle(void)
{
    gateway_switcher_t s;
    gateway_switcher_actions_t a;
    gateway_switcher_peer_t target = make_peer(0x80, 1);
    gateway_switcher_init(&s);

    gateway_switcher_select(&s, &target, 0, &a);  /* 从未连过任何目标 */
    gateway_switcher_tick(&s, GATEWAY_SWITCHER_TIMEOUT_MS, &a);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_UI_ERROR));
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_UI_IDLE));
    CHECK(s.state == GATEWAY_SWITCHER_IDLE);
    CHECK(!s.selected_valid);
}

static void test_target_disconnect_waits(void)
{
    gateway_switcher_t s;
    gateway_switcher_actions_t a;
    gateway_switcher_peer_t target = make_peer(0x90, 1);
    gateway_switcher_init(&s);

    gateway_switcher_select(&s, &target, 0, &a);
    gateway_switcher_peer_connected(&s, &target, 10, &a);
    CHECK(s.state == GATEWAY_SWITCHER_CONNECTED);

    gateway_switcher_peer_disconnected(&s, 100, &a);
    CHECK(s.state == GATEWAY_SWITCHER_SWITCHING);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_START_ADV));
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_UI_SWITCHING));

    gateway_switcher_peer_connected(&s, &target, 200, &a);
    CHECK(s.state == GATEWAY_SWITCHER_CONNECTED);
}

/* 目标掉线不做超时回退：等它回来，期间仍拒绝非目标。 */
static void test_dropped_target_is_waited_for(void)
{
    gateway_switcher_t s;
    gateway_switcher_actions_t a;
    gateway_switcher_peer_t target = make_peer(0x98, 1);
    gateway_switcher_peer_t other = make_peer(0x99, 1);
    gateway_switcher_init(&s);

    gateway_switcher_select(&s, &target, 0, &a);
    gateway_switcher_peer_connected(&s, &target, 10, &a);
    gateway_switcher_peer_disconnected(&s, 100, &a);

    /* 远超超时窗口：仍不应回退/清空选择。 */
    gateway_switcher_tick(&s, 100 + GATEWAY_SWITCHER_TIMEOUT_MS * 3, &a);
    CHECK(a.count == 0);
    CHECK(s.state == GATEWAY_SWITCHER_SWITCHING);
    CHECK(s.selected_valid);

    /* 期间别的 PC 来连：仍被拒。 */
    gateway_switcher_peer_connected(&s, &other, 50000, &a);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_REJECT_PEER));
}

static void test_clear_selection(void)
{
    gateway_switcher_t s;
    gateway_switcher_actions_t a;
    gateway_switcher_peer_t target = make_peer(0xA0, 1);
    gateway_switcher_peer_t other = make_peer(0xB0, 1);
    gateway_switcher_init(&s);

    gateway_switcher_select(&s, &target, 0, &a);
    gateway_switcher_peer_connected(&s, &target, 10, &a);

    gateway_switcher_clear_selection(&s, &a);
    CHECK(s.state == GATEWAY_SWITCHER_IDLE);
    CHECK(!s.selected_valid);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_UI_IDLE));

    gateway_switcher_peer_connected(&s, &other, 20, &a);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_ACCEPT_PEER));
}

static void test_addr_type_distinguishes(void)
{
    gateway_switcher_t s;
    gateway_switcher_actions_t a;
    gateway_switcher_peer_t target = make_peer(0xC0, 1);
    gateway_switcher_peer_t other_type = make_peer(0xC0, 0);
    gateway_switcher_init(&s);

    gateway_switcher_select(&s, &target, 0, &a);
    gateway_switcher_peer_connected(&s, &other_type, 10, &a);
    CHECK(has(&a, GATEWAY_SWITCHER_ACTION_REJECT_PEER));  /* 地址类型不同 ⇒ 不同目标 */
}

int main(void)
{
    test_idle_accepts_anyone();
    test_switch_happy_path();
    test_non_target_rejected();
    test_timeout_rolls_back();
    test_timeout_without_previous_goes_idle();
    test_target_disconnect_waits();
    test_dropped_target_is_waited_for();
    test_clear_selection();
    test_addr_type_distinguishes();
    if (failures == 0) {
        printf("test_gateway_switcher: ALL PASS\n");
        return 0;
    }
    printf("test_gateway_switcher: %d failure(s)\n", failures);
    return 1;
}
