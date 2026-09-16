// gateway_report_parser.h — 小米遥控器 8 字节 HID 报文解析（usage 集合 diff）
//
// 报文格式（Phase 0 spike 真机结论，Doc/Plan/xiaomi-remote-stick-gateway.md §6.1）：
//   8 字节 = 2 字节头（实测 00 00，内容不承载语义）+ 3×LE16 usage 槽（0=空槽）。
//   报文语义为「当前按下集合」快照，沿（按下/松开）由本解析器对相邻报文做集合 diff 得出。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GATEWAY_REPORT_SLOTS 3
#define GATEWAY_REPORT_LEN 8

typedef struct {
    uint16_t current[GATEWAY_REPORT_SLOTS];  // 上一帧按下集合（0=空）
} gateway_report_parser_t;

void gateway_report_parser_reset(gateway_report_parser_t *parser);

// 喂入一帧报文，输出相对上一帧的沿。pressed/released 为 3 槽容量数组，
// 计数写入 *pressed_count/*released_count（可为 0）。
// 返回 0 成功；-1 参数非法（NULL/长度不足，输出参数不被触碰）。
int gateway_report_parser_feed(gateway_report_parser_t *parser, const uint8_t *data, size_t len,
                               uint16_t pressed[GATEWAY_REPORT_SLOTS], size_t *pressed_count,
                               uint16_t released[GATEWAY_REPORT_SLOTS], size_t *released_count);
