// gateway_report_parser.c — 8 字节报文集合 diff 实现（纯逻辑，无 ESP-IDF 依赖）
#include "gateway_report_parser.h"

#include <stdbool.h>
#include <string.h>

void gateway_report_parser_reset(gateway_report_parser_t *parser) {
    if (parser == NULL) {
        return;
    }
    memset(parser->current, 0, sizeof(parser->current));
}

// 小工具：usage 是否在集合中（0 视为空槽不算成员）
static bool slot_contains(const uint16_t *set, uint16_t usage) {
    for (int i = 0; i < GATEWAY_REPORT_SLOTS; i++) {
        if (set[i] == usage) {
            return true;
        }
    }
    return false;
}

int gateway_report_parser_feed(gateway_report_parser_t *parser, const uint8_t *data, size_t len,
                               uint16_t pressed[GATEWAY_REPORT_SLOTS], size_t *pressed_count,
                               uint16_t released[GATEWAY_REPORT_SLOTS], size_t *released_count) {
    if (parser == NULL || data == NULL || len < GATEWAY_REPORT_LEN) {
        return -1;
    }

    // 解析本帧按下集合；同报重复 usage 按集合语义去重（首见槽有效，重复槽忽略）
    uint16_t next[GATEWAY_REPORT_SLOTS] = {0, 0, 0};
    int filled = 0;
    for (int i = 0; i < GATEWAY_REPORT_SLOTS; i++) {
        uint16_t usage =
            (uint16_t)(data[2 + 2 * i] | (data[3 + 2 * i] << 8));
        if (usage == 0) {
            continue;
        }
        if (slot_contains(next, usage)) {
            continue;
        }
        next[filled++] = usage;
    }

    size_t pc = 0, rc = 0;
    for (int i = 0; i < filled; i++) {
        if (!slot_contains(parser->current, next[i])) {
            pressed[pc++] = next[i];
        }
    }
    for (int i = 0; i < GATEWAY_REPORT_SLOTS; i++) {
        uint16_t usage = parser->current[i];
        if (usage != 0 && !slot_contains(next, usage)) {
            released[rc++] = usage;
        }
    }

    memcpy(parser->current, next, sizeof(next));
    *pressed_count = pc;
    *released_count = rc;
    return 0;
}
