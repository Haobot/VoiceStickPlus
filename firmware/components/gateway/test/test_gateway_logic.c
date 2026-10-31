// test_gateway_logic.c — gateway 纯逻辑模块 host 侧单测
//
// 覆盖 gateway_report_parser（8 字节报文→usage 集合 diff→沿）与
// gateway_keymap（小米 usage→标准 HID 动作翻译）。
// 报文格式依据 Phase 0 spike 真机结论（Doc/Plan/xiaomi-remote-stick-gateway.md §6.1）：
//   8 字节 = 2 字节头 + 3×LE16 usage 槽（0=空），松开帧全零。
// 用例事实源：back=0xF1/vol+=0x80/vol-=0x81 真机捕获；其余 13 键表见 usage-tap 文档 §1。
#include <stdio.h>
#include <string.h>

#include "gateway_hogp_report.h"
#include "gateway_keymap.h"
#include "gateway_report_parser.h"

static int s_failed = 0;
static int s_total = 0;

#define CHECK(cond, desc)                                              \
    do {                                                               \
        s_total++;                                                     \
        if (!(cond)) {                                                 \
            s_failed++;                                                \
            printf("  [失败] %s（第 %d 行）\n", desc, __LINE__);       \
        }                                                              \
    } while (0)

// 辅助：构造一帧报文（三槽 usage，0 表示空槽）
static void make_report(uint8_t out[8], uint16_t s0, uint16_t s1, uint16_t s2) {
    memset(out, 0, 8);
    out[2] = (uint8_t)(s0 & 0xFF);
    out[3] = (uint8_t)(s0 >> 8);
    out[4] = (uint8_t)(s1 & 0xFF);
    out[5] = (uint8_t)(s1 >> 8);
    out[6] = (uint8_t)(s2 & 0xFF);
    out[7] = (uint8_t)(s2 >> 8);
}

static void feed(gateway_report_parser_t *p, const uint8_t rep[8],
                 uint16_t pressed[3], size_t *pc, uint16_t released[3], size_t *rc) {
    int err = gateway_report_parser_feed(p, rep, 8, pressed, pc, released, rc);
    if (err != 0) {
        printf("  [失败] feed 返回 %d（应为 0）\n", err);
        s_failed++;
    }
    s_total++;
}

// ---------- gateway_report_parser ----------

static void test_parser_initial_empty_report(void) {
    printf("[用例] 初始态收到全零报文：不产生任何沿\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8] = {0};
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 0 && rc == 0, "空报文无沿");
}

static void test_parser_single_press(void) {
    printf("[用例] 单键按下：pressed 含该 usage\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8];
    make_report(rep, 0x00F1, 0, 0);  // back（真机捕获值）
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 1 && pressed[0] == 0x00F1, "back 按下沿");
    CHECK(rc == 0, "无松开沿");

    printf("[用例] 同报文重复：集合不变，无沿\n");
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 0 && rc == 0, "重复报文无沿");

    printf("[用例] 松开帧（全零）：released 含该 usage\n");
    uint8_t rel[8] = {0};
    feed(&p, rel, pressed, &pc, released, &rc);
    CHECK(rc == 1 && released[0] == 0x00F1, "back 松开沿");
    CHECK(pc == 0, "无按下沿");
}

static void test_parser_multi_key(void) {
    printf("[用例] 双键同按：一次报文 pressed 两个 usage\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8];
    make_report(rep, 0x0080, 0x0081, 0);  // vol+ + vol-
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 2, "两键按下");
    CHECK((pressed[0] == 0x0080 && pressed[1] == 0x0081) ||
              (pressed[0] == 0x0081 && pressed[1] == 0x0080),
          "集合含两 usage（顺序无关）");

    printf("[用例] 一键保持一键新增：pressed 仅新键\n");
    make_report(rep, 0x0080, 0x0081, 0x00F1);
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 1 && pressed[0] == 0x00F1, "仅新增键产生按下沿");
    CHECK(rc == 0, "无松开沿");

    printf("[用例] 一键保持一键松开：released 仅松开键\n");
    make_report(rep, 0x0080, 0, 0x00F1);  // vol- 松开
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(rc == 1 && released[0] == 0x0081, "仅消失键产生松开沿");
    CHECK(pc == 0, "无按下沿");
}

static void test_parser_header_ignored(void) {
    printf("[用例] 头两字节内容不影响解析（只认三槽）\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8];
    make_report(rep, 0x0052, 0, 0);
    rep[0] = 0xAB;  // 真机实测头为 00 00，但解析器不依赖头内容
    rep[1] = 0xCD;
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 1 && pressed[0] == 0x0052, "头部任意值仍解析出 up 按下");
}

static void test_parser_duplicate_usage_dedup(void) {
    printf("[用例] 同报重复 usage（异常输入）：集合语义去重，只产生一个沿\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t rep[8];
    make_report(rep, 0x0028, 0x0028, 0);
    uint16_t pressed[3], released[3];
    size_t pc, rc;
    feed(&p, rep, pressed, &pc, released, &rc);
    CHECK(pc == 1, "重复 usage 去重后仅一个按下沿");

    printf("[用例] 松开：去重后仅一个松开沿\n");
    uint8_t rel[8] = {0};
    feed(&p, rel, pressed, &pc, released, &rc);
    CHECK(rc == 1, "仅一个松开沿");
}

static void test_parser_invalid_length(void) {
    printf("[用例] 长度不足 8：拒绝并保持状态\n");
    gateway_report_parser_t p;
    gateway_report_parser_reset(&p);
    uint8_t short_rep[7] = {0};
    uint16_t pressed[3], released[3];
    size_t pc = 9, rc = 9;  // 哨兵值：失败路径不应改动
    int err = gateway_report_parser_feed(&p, short_rep, 7, pressed, &pc, released, &rc);
    CHECK(err != 0, "短报文返回错误");
    CHECK(pc == 9 && rc == 9, "失败路径不污染输出参数");

    printf("[用例] NULL 指针：返回错误不崩溃\n");
    err = gateway_report_parser_feed(NULL, short_rep, 7, pressed, &pc, released, &rc);
    CHECK(err != 0, "NULL 解析器返回错误");
    err = gateway_report_parser_feed(&p, NULL, 8, pressed, &pc, released, &rc);
    CHECK(err != 0, "NULL 报文返回错误");
}

// ---------- gateway_keymap ----------

static void test_keymap_consumer_keys(void) {
    // 三键 + mute：键盘页非标 usage → 翻译为 Consumer 页标准 usage（R1 目标）
    printf("[用例] 三键翻译为标准 Consumer usage\n");
    struct {
        uint16_t in;
        uint16_t want;
        const char *name;
    } cases[] = {
        {0x00F1, 0x0224, "back→AC Back"},
        {0x0080, 0x00E9, "vol+→Volume Increment"},
        {0x0081, 0x00EA, "vol-→Volume Decrement"},
        {0x007F, 0x00E2, "mute→Mute"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gateway_key_action_t a = gateway_keymap_translate(cases[i].in);
        s_total++;
        if (a.kind != GATEWAY_KEY_CONSUMER || a.value != cases[i].want) {
            s_failed++;
            printf("  [失败] %s：得到 kind=%d value=0x%04x\n", cases[i].name, (int)a.kind, a.value);
        }
    }
}

static void test_keymap_keyboard_keys(void) {
    // 方向/ok/menu/home：小米报的是键盘页 usage，值域恰与 HID keycode 一致，直接透传
    printf("[用例] 方向/OK/菜单/Home 键盘页透传\n");
    struct {
        uint16_t in;
        uint16_t want;
        const char *name;
    } cases[] = {
        {0x0052, 0x52, "up"},
        {0x0051, 0x51, "down"},
        {0x0050, 0x50, "left"},
        {0x004F, 0x4F, "right"},
        {0x0028, 0x28, "ok→Enter"},
        {0x0065, 0x65, "menu→Application"},
        {0x004A, 0x4A, "home→Home"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gateway_key_action_t a = gateway_keymap_translate(cases[i].in);
        s_total++;
        if (a.kind != GATEWAY_KEY_KEYBOARD || a.value != cases[i].want) {
            s_failed++;
            printf("  [失败] %s：得到 kind=%d value=0x%04x\n", cases[i].name, (int)a.kind, a.value);
        }
    }
}

static void test_keymap_intercept_keys(void) {
    printf("[用例] 截留键：语音/电源/tv/未知 usage 不向目标设备输出\n");
    uint16_t intercepts[] = {
        0x003E,  // 语音键（键盘页 F5；桌面端直连即识别为 F5，待真机复核 usage 值）
        0x0066,  // power：防误关机
        0x0035,  // tv
        0x0299,  // 未知 usage
    };
    for (size_t i = 0; i < sizeof(intercepts) / sizeof(intercepts[0]); i++) {
        gateway_key_action_t a = gateway_keymap_translate(intercepts[i]);
        s_total++;
        if (a.kind != GATEWAY_KEY_INTERCEPT) {
            s_failed++;
            printf("  [失败] usage 0x%04x 应截留，得到 kind=%d\n", intercepts[i], (int)a.kind);
        }
    }
}

// ---------- gateway_keymap 软件路由（P1 隧道融合） ----------

static void test_keymap_route_default_passthrough(void) {
    printf("[用例] 路由：默认全部直通，translate 行为与 Phase 1 一致\n");
    gateway_keymap_reset_routes();
    gateway_key_action_t a = gateway_keymap_translate(0x00F1);  // back
    CHECK(a.kind == GATEWAY_KEY_CONSUMER, "back 默认 Consumer 直通");
    a = gateway_keymap_translate(0x0028);  // ok
    CHECK(a.kind == GATEWAY_KEY_KEYBOARD, "ok 默认键盘直通");
    a = gateway_keymap_translate(0x0066);  // power
    CHECK(a.kind == GATEWAY_KEY_INTERCEPT, "power 默认截留");
}

static void test_keymap_route_software_override(void) {
    printf("[用例] 路由：直通键设软件路由后 translate 返回 SOFTWARE（value=usage）\n");
    gateway_keymap_reset_routes();
    CHECK(gateway_keymap_set_route(0x00F1, GATEWAY_ROUTE_SOFTWARE) == 0, "back 设软件路由");
    gateway_key_action_t a = gateway_keymap_translate(0x00F1);
    CHECK(a.kind == GATEWAY_KEY_SOFTWARE, "back 软件路由");
    CHECK(a.value == 0x00F1, "SOFTWARE value 携带原 usage");
    CHECK(gateway_keymap_get_route(0x00F1) == GATEWAY_ROUTE_SOFTWARE, "get_route 一致");

    printf("[用例] 路由：截留键 power/tv 也可软件路由\n");
    CHECK(gateway_keymap_set_route(0x0066, GATEWAY_ROUTE_SOFTWARE) == 0, "power 设软件路由");
    a = gateway_keymap_translate(0x0066);
    CHECK(a.kind == GATEWAY_KEY_SOFTWARE, "power 软件路由");
    CHECK(gateway_keymap_set_route(0x0035, GATEWAY_ROUTE_SOFTWARE) == 0, "tv 设软件路由");
    a = gateway_keymap_translate(0x0035);
    CHECK(a.kind == GATEWAY_KEY_SOFTWARE, "tv 软件路由");

    printf("[用例] 路由：恢复直通后回到原动作\n");
    CHECK(gateway_keymap_set_route(0x00F1, GATEWAY_ROUTE_PASSTHROUGH) == 0, "back 恢复直通");
    a = gateway_keymap_translate(0x00F1);
    CHECK(a.kind == GATEWAY_KEY_CONSUMER, "back 回 Consumer 直通");
}

static void test_keymap_route_voice_key_not_routable(void) {
    printf("[用例] 路由：语音键不可软件路由（固定 ATVV 会话语义）\n");
    gateway_keymap_reset_routes();
    CHECK(gateway_keymap_set_route(0x003E, GATEWAY_ROUTE_SOFTWARE) != 0, "语音键设路由被拒");
    CHECK(gateway_keymap_get_route(0x003E) == GATEWAY_ROUTE_PASSTHROUGH, "语音键路由恒直通");
    gateway_key_action_t a = gateway_keymap_translate(0x003E);
    CHECK(a.kind == GATEWAY_KEY_INTERCEPT, "语音键仍截留（ATVV 驱动）");

    printf("[用例] 路由：未知 usage 设路由被拒\n");
    CHECK(gateway_keymap_set_route(0x0299, GATEWAY_ROUTE_SOFTWARE) != 0, "未知 usage 被拒");
}

static void test_keymap_route_reset(void) {
    printf("[用例] 路由：reset_routes 恢复全部默认\n");
    gateway_keymap_set_route(0x0080, GATEWAY_ROUTE_SOFTWARE);
    gateway_keymap_set_route(0x0066, GATEWAY_ROUTE_SOFTWARE);
    gateway_keymap_reset_routes();
    CHECK(gateway_keymap_get_route(0x0080) == GATEWAY_ROUTE_PASSTHROUGH, "volume_up 复位");
    CHECK(gateway_keymap_get_route(0x0066) == GATEWAY_ROUTE_PASSTHROUGH, "power 复位");
}

static void test_keymap_key_names(void) {
    printf("[用例] 键名：13 键 usage→协议 key 字符串，语音键无键名\n");
    struct { uint16_t usage; const char *name; } table[] = {
        {0x0028, "ok"},       {0x004F, "right"},   {0x0050, "left"},
        {0x0051, "down"},     {0x0052, "up"},      {0x0065, "menu"},
        {0x004A, "home"},     {0x00F1, "back"},    {0x0080, "volume_up"},
        {0x0081, "volume_down"}, {0x007F, "volume_mute"}, {0x0066, "power"},
        {0x0035, "tv"},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const char *name = gateway_keymap_key_name(table[i].usage);
        s_total++;
        if (name == NULL || strcmp(name, table[i].name) != 0) {
            s_failed++;
            printf("  [失败] usage 0x%04x 键名应为 %s，得到 %s\n", table[i].usage,
                   table[i].name, name ? name : "NULL");
        }
    }
    CHECK(gateway_keymap_key_name(0x003E) == NULL, "语音键无键名（不进软件路由协议）");
    CHECK(gateway_keymap_key_name(0x0299) == NULL, "未知 usage 无键名");
    // 可路由键名集合（NVS/协议枚举用）
    CHECK(gateway_keymap_routable_key_count() == 13, "可路由键 13 个（含语音键外的全部）");
}


// ---------- gateway_hogp_report（HOGP Report Map 与 Report 报文） ----------
//
// 真机背景（Phase 1）：Windows 11 与安卓配对后 HID 节点 Code 10——两处报文级缺陷：
//   1. 键盘段 6 键槽的 Usage Maximum 笔误写成 0x2A（=Usage Minimum），usage 范围反向；
//   2. Report 特征值多带 Report ID 前缀，违反 HOGP"报文不含 Report ID"。
// 下面的描述符解析器把这两条不变量固化成回归位。

// 极简 HID 描述符解析器：只覆盖本 Report Map 用到的 item。
// 断言两条不变量：usage 范围成对且 min ≤ max；各 Report ID 的位宽 = 报文长度。
typedef struct {
    uint16_t report_id;
    uint32_t report_size;
    uint32_t report_count;
    uint32_t usage_min;
    uint32_t usage_max;
    bool have_min;
    bool have_max;
    bool bad_range;        // usage min > usage max
    bool min_without_max;  // 数组项缺少成对的 usage min/max
    uint32_t bits[4];      // 下标 = Report ID
    bool seen_id[4];
} map_scan_t;

static void map_scan(const uint8_t *desc, size_t len, map_scan_t *s) {
    memset(s, 0, sizeof(*s));
    size_t i = 0;
    while (i < len) {
        uint8_t prefix = desc[i++];
        size_t bsize = (size_t)(prefix & 0x03);
        if (bsize == 3) {
            bsize = 4;
        }
        uint8_t btype = (uint8_t)((prefix >> 2) & 0x03);
        uint8_t btag = (uint8_t)((prefix >> 4) & 0x0F);
        if (i + bsize > len) {
            s->bad_range = true;  // 描述符截断
            return;
        }
        uint32_t val = 0;
        for (size_t k = 0; k < bsize; k++) {
            val |= (uint32_t)desc[i + k] << (8 * k);
        }
        i += bsize;

        if (btype == 1) {  // Global
            switch (btag) {
                case 7: s->report_size = val; break;
                case 8:
                    s->report_id = (uint16_t)val;
                    if (val < 4u) {
                        s->seen_id[val] = true;
                    }
                    break;
                case 9: s->report_count = val; break;
                default: break;
            }
        } else if (btype == 2) {  // Local
            if (btag == 1) {
                s->usage_min = val;
                s->have_min = true;
            } else if (btag == 2) {
                s->usage_max = val;
                s->have_max = true;
            }
        } else if (btype == 0) {  // Main
            if (btag == 8 || btag == 9 || btag == 11) {  // Input / Output / Feature
                bool is_const = (val & 0x01u) != 0u;     // Constant 位（保留位，无 usage）
                bool is_var = (val & 0x02u) != 0u;       // Variable 位（位图；否则数组）
                if (s->have_min || s->have_max) {
                    if (!(s->have_min && s->have_max)) {
                        s->min_without_max = true;
                    } else if (s->usage_min > s->usage_max) {
                        s->bad_range = true;
                    }
                } else if (!is_const && !is_var) {
                    // 数据数组项必须自带 usage 范围（缺范围时主机无法解析键码）
                    s->min_without_max = true;
                }
                if (s->report_id < 4u) {
                    s->bits[s->report_id] += s->report_size * s->report_count;
                }
                s->have_min = false;
                s->have_max = false;
            } else {  // Collection / End Collection：局部项清零
                s->have_min = false;
                s->have_max = false;
            }
        }
    }
}

static void test_hogp_report_map(void) {
    printf("[用例] Report Map：usage 范围合法 + 各 Report ID 位宽与报文长度一致\n");
    size_t len = 0;
    const uint8_t *map = gateway_hogp_report_map(&len);
    CHECK(map != NULL && len > 0, "Report Map 非空");
    map_scan_t s;
    map_scan(map, len, &s);
    CHECK(!s.bad_range, "usage min 不得大于 usage max（0x2A/0x29 笔误回归位）");
    CHECK(!s.min_without_max, "数组项必须成对声明 usage min/max");
    CHECK(s.seen_id[GATEWAY_HOGP_REPORT_ID_CONSUMER], "Report ID 1（Consumer）已声明");
    CHECK(s.seen_id[GATEWAY_HOGP_REPORT_ID_KEYBOARD], "Report ID 2（Keyboard）已声明");
    CHECK(s.bits[GATEWAY_HOGP_REPORT_ID_CONSUMER] == GATEWAY_HOGP_CONSUMER_REPORT_LEN * 8u,
          "Consumer 位宽 = 报文长度（1 字节）");
    CHECK(s.bits[GATEWAY_HOGP_REPORT_ID_KEYBOARD] == GATEWAY_HOGP_KEYBOARD_REPORT_LEN * 8u,
          "键盘位宽 = 报文长度（8 字节：modifier + 保留 + 6 键槽）");
}

static void test_hogp_consumer_report(void) {
    printf("[用例] Consumer 报文：位图映射/释放清位/未声明 usage 拒绝\n");
    gateway_hogp_reports_t r;
    gateway_hogp_reports_reset(&r);
    CHECK(r.consumer[0] == 0x00, "初值全零");

    CHECK(gateway_hogp_reports_set_consumer(&r, 0x00E9, true) == 0, "Vol+ 已声明");
    CHECK(r.consumer[0] == 0x02, "Vol+ 置 bit1");
    CHECK(gateway_hogp_reports_set_consumer(&r, 0x00EA, true) == 0, "Vol- 已声明");
    CHECK(r.consumer[0] == 0x06, "Vol- 置 bit2（位图累加，不互斥）");
    CHECK(gateway_hogp_reports_set_consumer(&r, 0x0224, true) == 0, "AC Back 已声明");
    CHECK(r.consumer[0] == 0x0E, "AC Back 置 bit3");
    CHECK(gateway_hogp_reports_set_consumer(&r, 0x00E2, true) == 0, "Mute 已声明");
    CHECK(r.consumer[0] == 0x0F, "Mute 置 bit0");

    CHECK(gateway_hogp_reports_set_consumer(&r, 0x00E9, false) == 0, "Vol+ 释放");
    CHECK(r.consumer[0] == 0x0D, "Vol+ 清 bit1，其余位保留");
    CHECK(gateway_hogp_reports_set_consumer(&r, 0x00EA, false) == 0, "Vol- 释放");
    CHECK(gateway_hogp_reports_set_consumer(&r, 0x0224, false) == 0, "AC Back 释放");
    CHECK(gateway_hogp_reports_set_consumer(&r, 0x00E2, false) == 0, "Mute 释放");
    CHECK(r.consumer[0] == 0x00, "全部释放后回全零");

    CHECK(gateway_hogp_reports_set_consumer(&r, 0x0299, true) == -1, "未声明 usage 被拒绝");
    CHECK(r.consumer[0] == 0x00, "拒绝的 usage 不改报文");
    CHECK(gateway_hogp_consumer_usage_bit(0x0299) == -1, "未声明 usage 无 bit");
}

static void test_hogp_keyboard_report(void) {
    printf("[用例] 键盘报文：无 Report ID 前缀 + 键槽位置 + 释放全零\n");
    gateway_hogp_reports_t r;
    gateway_hogp_reports_reset(&r);

    gateway_hogp_reports_set_keyboard(&r, 0x4F, true);  // 方向右
    CHECK(r.keyboard[0] == 0x00, "首字节是 modifier 而非 Report ID（HOGP 报文不含 ID）");
    CHECK(r.keyboard[1] == 0x00, "第 2 字节为保留");
    CHECK(r.keyboard[2] == 0x4F, "键码进 6 键槽首槽");
    for (size_t i = 3; i < GATEWAY_HOGP_KEYBOARD_REPORT_LEN; i++) {
        CHECK(r.keyboard[i] == 0x00, "其余键槽为空");
    }

    gateway_hogp_reports_set_keyboard(&r, 0x4F, false);
    for (size_t i = 0; i < GATEWAY_HOGP_KEYBOARD_REPORT_LEN; i++) {
        CHECK(r.keyboard[i] == 0x00, "释放帧全零");
    }
}

int main(void) {
    test_parser_initial_empty_report();
    test_parser_single_press();
    test_parser_multi_key();
    test_parser_header_ignored();
    test_parser_duplicate_usage_dedup();
    test_parser_invalid_length();
    test_keymap_consumer_keys();
    test_keymap_keyboard_keys();
    test_keymap_intercept_keys();
    test_keymap_route_default_passthrough();
    test_keymap_route_software_override();
    test_keymap_route_voice_key_not_routable();
    test_keymap_route_reset();
    test_keymap_key_names();
    test_hogp_report_map();
    test_hogp_consumer_report();
    test_hogp_keyboard_report();

    printf("\n结果：%d/%d 通过\n", s_total - s_failed, s_total);
    if (s_failed == 0) {
        printf("全部通过\n");
        return 0;
    }
    return 1;
}
