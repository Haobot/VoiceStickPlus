// gateway_switcher.c — 目标切换器纯逻辑（无 ESP-IDF 依赖，见 include/gateway_switcher.h）。
#include "gateway_switcher.h"

#include <string.h>

static void actions_reset(gateway_switcher_actions_t *out)
{
    if (!out) {
        return;
    }
    out->count = 0;
}

static void actions_push(gateway_switcher_actions_t *out, gateway_switcher_action_t action)
{
    if (!out || action == GATEWAY_SWITCHER_ACTION_NONE) {
        return;
    }
    // 去重：同一动作在一次调用里只出一次（例如 DISCONNECT + START_ADV 各有一次）。
    for (int i = 0; i < out->count; i++) {
        if (out->items[i] == action) {
            return;
        }
    }
    if (out->count < GATEWAY_SWITCHER_MAX_ACTIONS) {
        out->items[out->count++] = action;
    }
}

void gateway_switcher_init(gateway_switcher_t *switcher)
{
    memset(switcher, 0, sizeof(*switcher));
    switcher->state = GATEWAY_SWITCHER_IDLE;
    switcher->timeout_ms = GATEWAY_SWITCHER_TIMEOUT_MS;
}

bool gateway_switcher_peer_equal(const gateway_switcher_peer_t *lhs,
                                 const gateway_switcher_peer_t *rhs)
{
    if (!lhs || !rhs) {
        return false;
    }
    return lhs->addr_type == rhs->addr_type && memcmp(lhs->addr, rhs->addr, 6) == 0;
}

static void remember_selected(gateway_switcher_t *switcher, const gateway_switcher_peer_t *target)
{
    if (!target) {
        switcher->selected_valid = false;
        return;
    }
    switcher->selected = *target;
    switcher->selected_valid = true;
}

void gateway_switcher_select(gateway_switcher_t *switcher, const gateway_switcher_peer_t *target,
                             uint32_t now_ms, gateway_switcher_actions_t *out)
{
    actions_reset(out);
    if (!switcher || !target) {
        return;
    }
    // 已连上同一目标：不折腾链路，只刷新 UI。
    if (switcher->state == GATEWAY_SWITCHER_CONNECTED && switcher->current_valid &&
        gateway_switcher_peer_equal(&switcher->current, target)) {
        remember_selected(switcher, target);
        actions_push(out, GATEWAY_SWITCHER_ACTION_UI_CONNECTED);
        return;
    }
    // 记住旧目标用于超时回退（仅当它是"已连上的目标"时才有回退价值）。
    if (switcher->current_valid) {
        switcher->previous = switcher->current;
        switcher->previous_valid = true;
    }
    remember_selected(switcher, target);
    switcher->state = GATEWAY_SWITCHER_SWITCHING;
    switcher->switch_started_ms = now_ms;
    switcher->switch_user_initiated = true;
    actions_push(out, GATEWAY_SWITCHER_ACTION_DISCONNECT_CURRENT);
    actions_push(out, GATEWAY_SWITCHER_ACTION_START_ADV);
    actions_push(out, GATEWAY_SWITCHER_ACTION_UI_SWITCHING);
}

void gateway_switcher_clear_selection(gateway_switcher_t *switcher,
                                      gateway_switcher_actions_t *out)
{
    actions_reset(out);
    if (!switcher) {
        return;
    }
    switcher->selected_valid = false;
    switcher->previous_valid = false;
    switcher->state = GATEWAY_SWITCHER_IDLE;
    actions_push(out, GATEWAY_SWITCHER_ACTION_UI_IDLE);
}

void gateway_switcher_peer_connected(gateway_switcher_t *switcher,
                                     const gateway_switcher_peer_t *peer, uint32_t now_ms,
                                     gateway_switcher_actions_t *out)
{
    (void)now_ms;
    actions_reset(out);
    if (!switcher || !peer) {
        return;
    }
    // IDLE：不限制，任何目标都接受（保持默认行为）。
    if (switcher->state == GATEWAY_SWITCHER_IDLE || !switcher->selected_valid) {
        switcher->current = *peer;
        switcher->current_valid = true;
        actions_push(out, GATEWAY_SWITCHER_ACTION_ACCEPT_PEER);
        return;
    }
    if (gateway_switcher_peer_equal(&switcher->selected, peer)) {
        switcher->current = *peer;
        switcher->current_valid = true;
        switcher->state = GATEWAY_SWITCHER_CONNECTED;
        switcher->previous_valid = false;   // 切换成功，回退点作废
        switcher->switch_user_initiated = false;
        actions_push(out, GATEWAY_SWITCHER_ACTION_ACCEPT_PEER);
        actions_push(out, GATEWAY_SWITCHER_ACTION_UI_CONNECTED);
        return;
    }
    // 非选定目标（另一台桌面端抢先连上）：拒绝，继续等目标。
    actions_push(out, GATEWAY_SWITCHER_ACTION_REJECT_PEER);
}

void gateway_switcher_peer_disconnected(gateway_switcher_t *switcher, uint32_t now_ms,
                                        gateway_switcher_actions_t *out)
{
    actions_reset(out);
    if (!switcher) {
        return;
    }
    const bool was_current = switcher->current_valid;
    switcher->current_valid = false;
    if (!was_current) {
        return;
    }
    if (switcher->state == GATEWAY_SWITCHER_CONNECTED) {
        // 目标掉线：回到广播态等它回来（重连由桌面端心跳兜底，无需重新选目标）。
        // switch_user_initiated=false：这不是"切换失败"，不设超时回退——否则目标短暂掉线
        // 5s 后就会被清空选择，任何别的 PC 都能连进来，与"只允许这个目标"的意图相悖。
        switcher->state = GATEWAY_SWITCHER_SWITCHING;
        switcher->switch_started_ms = now_ms;
        switcher->switch_user_initiated = false;
        actions_push(out, GATEWAY_SWITCHER_ACTION_START_ADV);
        actions_push(out, GATEWAY_SWITCHER_ACTION_UI_SWITCHING);
    }
}

void gateway_switcher_tick(gateway_switcher_t *switcher, uint32_t now_ms,
                           gateway_switcher_actions_t *out)
{
    actions_reset(out);
    if (!switcher || switcher->state != GATEWAY_SWITCHER_SWITCHING) {
        return;
    }
    if (!switcher->switch_user_initiated) {
        return;  // 等选定目标自己回来，不做超时回退
    }
    if ((uint32_t)(now_ms - switcher->switch_started_ms) < switcher->timeout_ms) {
        return;
    }
    // 超时：回退到上一目标（若有），否则清空选定并报错。
    actions_push(out, GATEWAY_SWITCHER_ACTION_UI_ERROR);
    if (switcher->previous_valid) {
        const gateway_switcher_peer_t fallback = switcher->previous;
        switcher->previous_valid = false;
        remember_selected(switcher, &fallback);
        switcher->switch_started_ms = now_ms;
        actions_push(out, GATEWAY_SWITCHER_ACTION_START_ADV);
        actions_push(out, GATEWAY_SWITCHER_ACTION_UI_SWITCHING);
        return;
    }
    switcher->selected_valid = false;
    switcher->state = GATEWAY_SWITCHER_IDLE;
    actions_push(out, GATEWAY_SWITCHER_ACTION_START_ADV);
    actions_push(out, GATEWAY_SWITCHER_ACTION_UI_IDLE);
}

const char *gateway_switcher_state_name(gateway_switcher_state_t state)
{
    switch (state) {
    case GATEWAY_SWITCHER_IDLE:
        return "idle";
    case GATEWAY_SWITCHER_SWITCHING:
        return "switching";
    case GATEWAY_SWITCHER_CONNECTED:
        return "connected";
    }
    return "?";
}

const char *gateway_switcher_action_name(gateway_switcher_action_t action)
{
    switch (action) {
    case GATEWAY_SWITCHER_ACTION_NONE:
        return "none";
    case GATEWAY_SWITCHER_ACTION_DISCONNECT_CURRENT:
        return "disconnect_current";
    case GATEWAY_SWITCHER_ACTION_START_ADV:
        return "start_adv";
    case GATEWAY_SWITCHER_ACTION_ACCEPT_PEER:
        return "accept_peer";
    case GATEWAY_SWITCHER_ACTION_REJECT_PEER:
        return "reject_peer";
    case GATEWAY_SWITCHER_ACTION_UI_SWITCHING:
        return "ui_switching";
    case GATEWAY_SWITCHER_ACTION_UI_CONNECTED:
        return "ui_connected";
    case GATEWAY_SWITCHER_ACTION_UI_ERROR:
        return "ui_error";
    case GATEWAY_SWITCHER_ACTION_UI_IDLE:
        return "ui_idle";
    }
    return "?";
}
