// N8 cut10: xiaomi usage-tap batch extracted from core_tests.cc (8 tests).
// Pre-checks: col0 non-test heads = 0, cross-span refs = 0 (MakeTapReport
// pre-migrated to test_support.h in the same change).
#include "test_support.h"

void TestXiaomiUsageTapSessionEdges() {
    XiaomiUsageTapSession session;
    uint8_t report[9];

    // 空 → back+音量+：pressed 沿按 usage 升序（0x80 先于 0xF1）。
    MakeTapReport(report, 0x00F1, 0x0080, 0);
    auto edges = session.OnReport(report, 9);
    assert(edges.has_value());
    assert(edges->pressed.size() == 2);
    assert(edges->pressed[0] == "volume_up" && edges->pressed[1] == "back");
    assert(edges->released.empty() && edges->unknown_usages.empty());
    // 追加 home：只 pressed home。
    MakeTapReport(report, 0x00F1, 0x0080, 0x004A);
    edges = session.OnReport(report, 9);
    assert(edges->pressed.size() == 1 && edges->pressed[0] == "home");
    assert(edges->released.empty());
    // 松开 back：只 released back。
    MakeTapReport(report, 0x0080, 0x004A, 0);
    edges = session.OnReport(report, 9);
    assert(edges->pressed.empty());
    assert(edges->released.size() == 1 && edges->released[0] == "back");
    // 全松开。
    MakeTapReport(report, 0, 0, 0);
    edges = session.OnReport(report, 9);
    assert(edges->pressed.empty());
    assert(edges->released.size() == 2);
    assert(session.active_empty());
    // 报文无变化：无沿。
    edges = session.OnReport(report, 9);
    assert(edges.has_value());
    assert(edges->pressed.empty() && edges->released.empty());

    // 非法报文：nullopt 且状态不变（后续 diff 基于旧活跃集合）。
    MakeTapReport(report, 0x00F1, 0, 0);
    assert(session.OnReport(report, 9).has_value());
    assert(!session.active_empty());
    const uint8_t bogus[9] = {0x02, 0x00, 0x00, 0xF1, 0x00, 0, 0, 0, 0};
    assert(!session.OnReport(bogus, 9).has_value());
    assert(!session.active_empty());
    MakeTapReport(report, 0, 0, 0);
    edges = session.OnReport(report, 9);
    assert(edges->released.size() == 1 && edges->released[0] == "back");

    // 未知 usage：进 unknown_usages（诊断），不产生按钮沿、不影响已按住键。
    MakeTapReport(report, 0x00F1, 0, 0);
    edges = session.OnReport(report, 9);
    assert(edges->pressed.size() == 1 && edges->pressed[0] == "back");
    MakeTapReport(report, 0x00F1, 0x0099, 0);  // 按住中冒出未知 usage
    edges = session.OnReport(report, 9);
    assert(edges->pressed.empty() && edges->released.empty());
    assert(edges->unknown_usages == (std::vector<uint16_t>{0x0099}));
    MakeTapReport(report, 0x00F1, 0, 0);  // 未知 usage 消失：无按钮沿
    edges = session.OnReport(report, 9);
    assert(edges->released.empty());
    assert(edges->unknown_usages.empty());

    // 断连：活跃集合全部 released 并清空状态。
    MakeTapReport(report, 0x00F1, 0x0080, 0);
    session.OnReport(report, 9);
    auto disconnect = session.OnDisconnect();
    assert(disconnect.released.size() == 2);
    assert(disconnect.pressed.empty());
    assert(session.active_empty());
    // 再次断连：空沿。
    const auto disconnect_again = session.OnDisconnect();
    assert(disconnect_again.released.empty() &&
           disconnect_again.pressed.empty());
}

void TestXiaomiTapDirectKeys() {
    const std::map<std::string, std::string> key_map = {
        {"back", "backspace"},
        {"volume_up", "ctrl+shift+right"},
        {"volume_down", "ctrl+down"},
        {"home", "ctrl+shift+h"}};
    constexpr std::int64_t kNow = 100000;

    // 单击：pressed 只登记不注入（对齐 10 键 keyup 后置的松手反馈体验），
    // released（重复延迟内）注入一次映射 down+up 对。
    {
        XiaomiTapDirectKeys keys;
        assert(!keys.OnPressed("back", kNow, key_map).has_value());
        assert(keys.HasHold("back"));
        const auto action = keys.OnReleased("back", kNow + 50, key_map);
        assert(action.has_value());
        assert(action->inject == (std::vector<UINT>{VK_BACK}));
        assert(action->inject_up == (std::vector<UINT>{VK_BACK}));
        assert(!keys.HasHold("back"));
    }
    // 组合键映射：down 修饰键序+主键，up 反序（同 interceptor 注入构造）。
    {
        XiaomiTapDirectKeys keys;
        keys.OnPressed("volume_up", kNow, key_map);
        const auto action = keys.OnReleased("volume_up", kNow + 100, key_map).value();
        assert(action.inject == (std::vector<UINT>{VK_CONTROL, VK_SHIFT, VK_RIGHT}));
        assert(action.inject_up ==
               (std::vector<UINT>{VK_RIGHT, VK_SHIFT, VK_CONTROL}));
    }
    // 无映射 / 空串显式取消 / 非直触发键：不登记，released 无动作。
    {
        XiaomiTapDirectKeys keys;
        const std::map<std::string, std::string> empty_map;
        const std::map<std::string, std::string> cancel_map{{"back", ""}};
        assert(!keys.OnPressed("back", kNow, empty_map).has_value());
        assert(!keys.HasHold("back"));
        assert(!keys.OnPressed("back", kNow, cancel_map).has_value());
        assert(!keys.HasHold("back"));
        // home 是系统可见键：tap 直触发不接管（RC003 事实，防与现有管线双触发）。
        assert(!keys.OnPressed("home", kNow, key_map).has_value());
        assert(!keys.HasHold("home"));
        assert(!keys.OnPressed("volume_mute", kNow, key_map).has_value());
        // released 无 hold：nullopt（残留沿，如 tap 会话中途恢复）。
        assert(!keys.OnReleased("back", kNow, key_map).has_value());
    }
    // 长按重复（back 节拍 280/40，MiVibe 真机值初值）：延迟后按间隔连发，
    // 重复已发过则 released 不再补发（松手不多一键）。
    {
        XiaomiTapDirectKeys keys;
        keys.OnPressed("back", kNow, key_map);
        assert(!keys.PollRepeat("back", kNow + 279, key_map).has_value());
        assert(keys.PollRepeat("back", kNow + 280, key_map).has_value());
        assert(!keys.PollRepeat("back", kNow + 319, key_map).has_value());
        assert(keys.PollRepeat("back", kNow + 320, key_map).has_value());
        assert(!keys.OnReleased("back", kNow + 500, key_map).has_value());
        assert(!keys.HasHold("back"));
    }
    // 音量节拍 400/120。
    {
        XiaomiTapDirectKeys keys;
        keys.OnPressed("volume_down", kNow, key_map);
        assert(!keys.PollRepeat("volume_down", kNow + 399, key_map).has_value());
        assert(keys.PollRepeat("volume_down", kNow + 400, key_map).has_value());
        assert(!keys.PollRepeat("volume_down", kNow + 519, key_map).has_value());
        assert(keys.PollRepeat("volume_down", kNow + 520, key_map).has_value());
        assert(!keys.OnReleased("volume_down", kNow + 600, key_map).has_value());
    }
    // 重复 pressed（报文抖动）：幂等，节拍仍按首次 pressed 计算。
    {
        XiaomiTapDirectKeys keys;
        keys.OnPressed("back", kNow, key_map);
        assert(!keys.OnPressed("back", kNow + 30, key_map).has_value());
        assert(!keys.PollRepeat("back", kNow + 279, key_map).has_value());
        assert(keys.PollRepeat("back", kNow + 280, key_map).has_value());
    }
    // 判定时映射已被取消（key_map 热更场景）：无注入，hold 清除（放行语义）。
    {
        XiaomiTapDirectKeys keys;
        keys.OnPressed("back", kNow, key_map);
        const std::map<std::string, std::string> empty_map;
        assert(!keys.OnReleased("back", kNow + 50, empty_map).has_value());
        assert(!keys.HasHold("back"));
    }
    // CancelHold：系统翻译到达（RC001 类固件该键可见）时现有管线接管，
    // 直触发让位，防双触发。
    {
        XiaomiTapDirectKeys keys;
        keys.OnPressed("back", kNow, key_map);
        keys.CancelHold("back", kNow + 5);
        assert(!keys.HasHold("back"));
        assert(!keys.OnReleased("back", kNow + 50, key_map).has_value());
        assert(!keys.PollRepeat("back", kNow + 300, key_map).has_value());
        keys.CancelHold("back", kNow + 10);  // 无 hold 时幂等
    }
    // 时钟乱序防御：LL keydown 先于 tap pressed 到达（CancelHold 在前），
    // 抑制窗内的 pressed 不登记（该键由现有管线接管，released 无残留注入）。
    {
        XiaomiTapDirectKeys keys;
        keys.CancelHold("back", kNow);
        assert(!keys.OnPressed("back", kNow + 100, key_map).has_value());
        assert(!keys.HasHold("back"));
        assert(!keys.OnReleased("back", kNow + 150, key_map).has_value());
        assert(!keys.PollRepeat("back", kNow + 400, key_map).has_value());
    }
    // 抑制窗外恢复登记（正常 tap 信号晚于一次历史取消，如断连重连场景）。
    {
        XiaomiTapDirectKeys keys;
        keys.CancelHold("back", kNow);
        assert(!keys.OnPressed("back", kNow + 200, key_map).has_value());
        assert(keys.HasHold("back"));
        assert(keys.OnReleased("back", kNow + 250, key_map).has_value());
    }
    // 抑制只作用于被取消的按钮：相邻直触发键不受影响。
    {
        XiaomiTapDirectKeys keys;
        keys.CancelHold("back", kNow);
        assert(!keys.OnPressed("volume_up", kNow + 50, key_map).has_value());
        assert(keys.HasHold("volume_up"));
    }
    // Reset：断连清全部（防按键状态卡死）。
    {
        XiaomiTapDirectKeys keys;
        keys.OnPressed("back", kNow, key_map);
        keys.OnPressed("volume_up", kNow, key_map);
        keys.Reset();
        assert(!keys.HasHold("back") && !keys.HasHold("volume_up"));
        assert(!keys.OnReleased("back", kNow + 50, key_map).has_value());
    }
    // 节拍参数表（§6.1 MiVibe 真机值）。
    assert(XiaomiTapRepeatTimingFor("back").delay_ms == 280);
    assert(XiaomiTapRepeatTimingFor("back").interval_ms == 40);
    assert(XiaomiTapRepeatTimingFor("volume_up").delay_ms == 400);
    assert(XiaomiTapRepeatTimingFor("volume_up").interval_ms == 120);
    assert(XiaomiTapRepeatTimingFor("volume_down").delay_ms == 400);
    assert(XiaomiTapRepeatTimingFor("volume_down").interval_ms == 120);
    // 非直触发键无节拍（0/0）。
    assert(XiaomiTapRepeatTimingFor("home").delay_ms == 0);
    assert(XiaomiTapRepeatTimingFor("home").interval_ms == 0);
    assert(XiaomiTapRepeatTimingFor("bogus").interval_ms == 0);
}

// 网关软件路由键长按连发（音量键同款手感）：按下沿由调用方注入 down 序（本
// 状态机只登记），延迟节拍后 PollRepeat 产出完整 down+up 对，松开沿清除。
void TestXiaomiGatewayKeyRepeater() {
    const std::map<std::string, std::string> key_map = {
        {"back", "backspace"},
        {"ok", "ctrl+alt+d"}};
    constexpr std::int64_t kNow = 100000;

    // 登记与节拍：延迟前不出对，到点出对，间隔内不重复。
    {
        XiaomiGatewayKeyRepeater rep;
        rep.OnPressed("back", kNow, key_map);
        assert(rep.HasHold());
        assert(!rep.PollRepeat("back", kNow + 399, key_map).has_value());
        const auto first = rep.PollRepeat("back", kNow + 400, key_map);
        assert(first.has_value());
        // 完整 down+up 对（Backspace 单键：down 序与 up 序同键）。
        assert(first->inject == (std::vector<UINT>{VK_BACK}));
        assert(first->inject_up == (std::vector<UINT>{VK_BACK}));
        assert(!rep.PollRepeat("back", kNow + 519, key_map).has_value());
        assert(rep.PollRepeat("back", kNow + 520, key_map).has_value());
    }
    // 松开沿清除：长按中途松手不再出对。
    {
        XiaomiGatewayKeyRepeater rep;
        rep.OnPressed("back", kNow, key_map);
        assert(rep.PollRepeat("back", kNow + 400, key_map).has_value());
        rep.OnReleased("back");
        assert(!rep.HasHold());
        assert(!rep.PollRepeat("back", kNow + 600, key_map).has_value());
    }
    // 无映射按键不登记（放行语义，连发无从谈起）。
    {
        XiaomiGatewayKeyRepeater rep;
        const std::map<std::string, std::string> empty_map;
        rep.OnPressed("home", kNow, empty_map);
        assert(!rep.HasHold());
        assert(!rep.PollRepeat("home", kNow + 400, empty_map).has_value());
    }
    // 组合键映射：重复对 = 修饰键序 down + 主键，up 反序（与单击注入同构）。
    {
        XiaomiGatewayKeyRepeater rep;
        rep.OnPressed("ok", kNow, key_map);
        const auto pair = rep.PollRepeat("ok", kNow + 400, key_map).value();
        assert(pair.inject == (std::vector<UINT>{VK_CONTROL, VK_MENU, 'D'}));
        assert(pair.inject_up == (std::vector<UINT>{'D', VK_MENU, VK_CONTROL}));
    }
    // 多键并存互不干扰；Reset 断连清全部。
    {
        XiaomiGatewayKeyRepeater rep;
        rep.OnPressed("back", kNow, key_map);
        rep.OnPressed("ok", kNow + 100, key_map);
        assert(rep.PollRepeat("back", kNow + 400, key_map).has_value());
        assert(!rep.PollRepeat("ok", kNow + 400, key_map).has_value());
        assert(rep.PollRepeat("ok", kNow + 500, key_map).has_value());
        rep.Reset();
        assert(!rep.HasHold());
        assert(!rep.PollRepeat("back", kNow + 800, key_map).has_value());
    }
}

void TestXiaomiTapEvidenceTable() {
    XiaomiTapEvidenceTable table;
    constexpr std::int64_t kNow = 100000;
    // 空表：无佐证。
    assert(!table.HasRecentEdge("back", kNow));
    // 沿登记与窗口查询：now - last_edge <= window 命中。
    table.OnEdge("back", kNow);
    assert(table.HasRecentEdge("back", kNow));
    assert(table.HasRecentEdge("back", kNow + 250));
    assert(!table.HasRecentEdge("back", kNow + 251));
    // 不同按钮互不影响。
    assert(!table.HasRecentEdge("home", kNow));
    // 新沿覆盖旧沿（单槽最新）。
    table.OnEdge("back", kNow + 1000);
    assert(table.HasRecentEdge("back", kNow + 1000 + 200));
    assert(!table.HasRecentEdge("back", kNow + 1000 + 251));
    assert(!table.HasRecentEdge("back", kNow + 240));  // 旧沿已被覆盖
    // 自定义窗口。
    table.OnEdge("home", kNow + 2000);
    assert(table.HasRecentEdge("home", kNow + 2000 + 100, 100));
    assert(!table.HasRecentEdge("home", kNow + 2000 + 101, 100));
    // Reset。
    table.Reset();
    assert(!table.HasRecentEdge("back", kNow + 1000));
    assert(!table.HasRecentEdge("home", kNow + 2000));
}

// tap 管道帧解码器：DLL 侧变长帧字节流（1 字节心跳 0x01 / 10 字节数据
// 0x02+9 报文）→ 完整帧；支持任意分片/粘包/非法字节重同步。
void TestXiaomiTapFrameDecoder() {
    const std::vector<uint8_t> report = {0x01, 0x00, 0x00,
                                         0xF1, 0x00, 0x80, 0x00, 0x81, 0x00};
    // 单心跳帧。
    {
        XiaomiTapFrameDecoder decoder;
        const uint8_t heartbeat[] = {0x01};
        const auto frames = decoder.OnBytes(heartbeat, 1);
        assert(frames.size() == 1 && !frames[0].is_data);
    }
    // 单数据帧一次喂入：payload 原样。
    {
        XiaomiTapFrameDecoder decoder;
        std::vector<uint8_t> stream = {0x02};
        stream.insert(stream.end(), report.begin(), report.end());
        const auto frames = decoder.OnBytes(stream.data(), stream.size());
        assert(frames.size() == 1 && frames[0].is_data);
        assert(std::equal(frames[0].report, frames[0].report + 9,
                          report.begin()));
    }
    // 分片：数据帧拆 3 次喂，前两次无输出，第三次整帧产出。
    {
        XiaomiTapFrameDecoder decoder;
        std::vector<uint8_t> stream = {0x02};
        stream.insert(stream.end(), report.begin(), report.end());
        assert(decoder.OnBytes(stream.data(), 3).empty());
        assert(decoder.OnBytes(stream.data() + 3, 4).empty());
        const auto frames = decoder.OnBytes(stream.data() + 7, 3);
        assert(frames.size() == 1 && frames[0].is_data);
    }
    // 粘包：心跳+数据+心跳一次喂入，按序产出 3 帧。
    {
        XiaomiTapFrameDecoder decoder;
        std::vector<uint8_t> stream = {0x01, 0x02};
        stream.insert(stream.end(), report.begin(), report.end());
        stream.push_back(0x01);
        const auto frames = decoder.OnBytes(stream.data(), stream.size());
        assert(frames.size() == 3);
        assert(!frames[0].is_data && frames[1].is_data &&
               !frames[2].is_data);
    }
    // 非法前导字节：逐字节丢弃重同步，后续帧正常解出。
    {
        XiaomiTapFrameDecoder decoder;
        std::vector<uint8_t> stream = {0xFF, 0x00, 0x02};
        stream.insert(stream.end(), report.begin(), report.end());
        const auto frames = decoder.OnBytes(stream.data(), stream.size());
        assert(frames.size() == 1 && frames[0].is_data);
    }
    // 心跳可作数据帧 payload 首字节：0x02 后跟 0x01 开头的报文仍是数据帧。
    {
        XiaomiTapFrameDecoder decoder;
        const uint8_t stream[] = {0x02, 0x01, 0x00, 0x00, 0xF1,
                                  0x00, 0x81, 0x00, 0x00, 0x00};
        const auto frames =
            decoder.OnBytes(stream, sizeof(stream) / sizeof(stream[0]));
        assert(frames.size() == 1 && frames[0].is_data);
        assert(frames[0].report[0] == 0x01);
    }
    // Reset：半帧残留清空，重新从帧头解码。
    {
        XiaomiTapFrameDecoder decoder;
        const uint8_t partial[] = {0x02, 0x01, 0x00};
        decoder.OnBytes(partial, 3);
        decoder.Reset();
        std::vector<uint8_t> stream = {0x02};
        stream.insert(stream.end(), report.begin(), report.end());
        const auto frames = decoder.OnBytes(stream.data(), stream.size());
        assert(frames.size() == 1 && frames[0].is_data);
    }
}

// WUDFDiagnosticInfo\HostPid 注册表值解析：真机（Win11 26200）为 REG_QWORD，
// 兼容 REG_DWORD；类型不符/字节数不足拒绝。
void TestXiaomiHostPidValueParsing() {
    // REG_QWORD（真机形态）：8 字节小端，取低 32 位。
    {
        const uint8_t data[8] = {0xa4, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        assert(ParseHostPidValue(REG_QWORD, data, sizeof(data)) == 0x14a4);
        // 高 32 位非零（不可能的 PID，防御性截断）也取低 32 位。
        const uint8_t big[8] = {0xff, 0xff, 0x00, 0x00, 0xde, 0xad, 0x00, 0x00};
        assert(ParseHostPidValue(REG_QWORD, big, sizeof(big)) == 0xffff);
    }
    // REG_DWORD（兼容形态）：4 字节。
    {
        const uint8_t data[4] = {0x84, 0x0c, 0x00, 0x00};
        assert(ParseHostPidValue(REG_DWORD, data, sizeof(data)) == 0x0c84);
    }
    // 非法：类型不符 / 字节数不足 / 空 pid。
    {
        const uint8_t data[8] = {0xa4, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        assert(!ParseHostPidValue(REG_SZ, data, sizeof(data)).has_value());
        assert(!ParseHostPidValue(REG_QWORD, data, 4).has_value());
        assert(!ParseHostPidValue(REG_DWORD, data, 2).has_value());
        const uint8_t zero[4] = {};
        assert(!ParseHostPidValue(REG_DWORD, zero, sizeof(zero)).has_value());
    }
}

// 按键映射消费端（Doc/Plan/xiaomi-keymap-consumer.md）：kbdhid 翻译特征识别、
// 佐证窗决策（吞+注入/放行）、按住闩锁与 keyup 关联、注入 VK 序列构造。
void TestXiaomiKeymapInterceptor() {
    // ---- 特征识别表：遥控器 12 键的 (VK, 扫描码) 特征 → 按钮候选 ----
    assert(XiaomiButtonFromVkScan(VK_BROWSER_BACK, 0) == "back");
    // VK_BACK/0x0E 不再识别为 back（2026-09-07 三轮探针 + MiVibe 研读定案：RC003
    // 固件上报 back usage 0xF1，但被微软 HidOverGatt WUDF 宿主在翻译层丢弃，系统
    // 输入链路全静默——此前日志中的 VK_BACK 事件全部为物理键盘 Backspace 污染。
    // 保留该特征会拖慢物理 Backspace 首按并存在误吞风险，故移除；VK_BROWSER_BACK
    // 为 RC001 固件特征，保留）。
    assert(XiaomiButtonFromVkScan(VK_BACK, 0x0E) == std::nullopt);
    assert(XiaomiButtonFromVkScan(VK_BACK, 0) == std::nullopt);
    assert(XiaomiButtonFromVkScan(VK_BROWSER_HOME, 0) == "home");
    assert(XiaomiButtonFromVkScan(VK_HOME, 0) == "home");
    assert(XiaomiButtonFromVkScan(VK_RETURN, 0) == "ok");
    assert(XiaomiButtonFromVkScan(VK_UP, 0) == "up");
    assert(XiaomiButtonFromVkScan(VK_DOWN, 0) == "down");
    assert(XiaomiButtonFromVkScan(VK_LEFT, 0) == "left");
    assert(XiaomiButtonFromVkScan(VK_RIGHT, 0) == "right");
    assert(XiaomiButtonFromVkScan(VK_APPS, 0) == "menu");
    // tv 与键盘 Grave 同 VK，靠扫描码 0x29 特征 + 佐证归属区分。
    assert(XiaomiButtonFromVkScan(VK_OEM_3, 0x29) == "tv");
    assert(XiaomiButtonFromVkScan(VK_OEM_3, 0x02) == std::nullopt);
    // power 三特征：VK_SLEEP / VK 0xFF（kbdhid 未知键）/ 扫描码 0x5E。
    assert(XiaomiButtonFromVkScan(VK_SLEEP, 0) == "power");
    assert(XiaomiButtonFromVkScan(0xFF, 0) == "power");
    assert(XiaomiButtonFromVkScan(0x41, 0x5E) == "power");
    assert(XiaomiButtonFromVkScan(VK_VOLUME_UP, 0) == "volume_up");
    assert(XiaomiButtonFromVkScan(VK_VOLUME_DOWN, 0) == "volume_down");
    // 非遥控器特征：普通字母/数字/功能键/空扫描码 power 特征不算。
    assert(XiaomiButtonFromVkScan(0x41, 0) == std::nullopt);
    assert(XiaomiButtonFromVkScan(VK_F5, 0) == std::nullopt);
    assert(XiaomiButtonFromVkScan(0, 0x5E) == std::nullopt);
    // 识别出的候选必在可映射集合内（与 xiaomi_buttons.h 一致）。
    for (UINT vk : {VK_BACK, VK_BROWSER_BACK, VK_BROWSER_HOME, VK_HOME, VK_RETURN,
                    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_APPS, VK_OEM_3,
                    VK_SLEEP, 0xFF, 0x41, VK_VOLUME_UP, VK_VOLUME_DOWN}) {
        for (UINT scan : {UINT{0}, UINT{0x0E}, UINT{0x29}, UINT{0x5E}, UINT{0x02}}) {
            const auto button = XiaomiButtonFromVkScan(vk, scan);
            if (button.has_value()) assert(IsXiaomiMappableButton(*button));
        }
    }

    // ---- Raw Input 设备接口路径识别：小米遥控器在 Raw Input 中是
    // RIM_TYPEKEYBOARD，RIDI_DEVICEINFO 只填 keyboard 联合体成员（hid.dwVendorId
    // 恒 0，2026-09-07 真机排查教训），VID/PID 必须从 RIDI_DEVICENAME 路径解析。
    // BTHLE 实测名：\\?\HID#{00001812-...}_Dev_VID&012717_PID&32b8_REV&00a4_c05d39...#...#...
    assert(XiaomiRawInputNameIsRemote(
        L"\\\\?\\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&012717_PID&32b8_"
        L"REV&00a4_c05d39c36459#b&19f8b1bc&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}"));
    // USB HID 接口名格式同样命中（容错未来有线连接场景）。
    assert(XiaomiRawInputNameIsRemote(L"\\\\?\\HID#VID_2717&PID_32B8&MI_00#6&2a3b#0000"));
    // 大小写不敏感。
    assert(XiaomiRawInputNameIsRemote(L"hid#vid&2717_pid&32b8#x"));
    // VID/PID 任一不匹配即非目标设备。
    assert(!XiaomiRawInputNameIsRemote(L"\\\\?\\HID#VID_260D&PID_1131&MI_01&Col01#8&1bddaf93"));
    assert(!XiaomiRawInputNameIsRemote(L"\\\\?\\HID#VID_2717&PID_9999&MI_00#6&2a3b"));
    // 无标记 / 仅一个标记 / 标记嵌在单词里（如 devid）不算。
    assert(!XiaomiRawInputNameIsRemote(L""));
    assert(!XiaomiRawInputNameIsRemote(L"\\\\?\\HID#VID_2717&MI_00#6&2a3b"));
    assert(!XiaomiRawInputNameIsRemote(L"prefix devid&012717 end pid&32b8"));
    // 前导零等价：VID&012717（BTHLE 6 位格式，前两位为 Source 前缀）与
    // VID&2717 / VID&002717 按低 16 位数值相同。
    assert(XiaomiRawInputNameIsRemote(L"x_VID&2717_PID&32b8_y"));
    assert(XiaomiRawInputNameIsRemote(L"x_VID&002717_PID&32b8_y"));

    // ---- 注入序列：down 修饰键序+主键；up 反序 ----
    const auto backspace = ParseKeySpec("backspace").value();
    assert((XiaomiKeymapInjectDownVks(backspace) ==
           std::vector<UINT>{VK_BACK}));
    assert((XiaomiKeymapInjectUpVks(backspace) == std::vector<UINT>{VK_BACK}));
    const auto combo = ParseKeySpec("ctrl+shift+v").value();
    assert((XiaomiKeymapInjectDownVks(combo) ==
           std::vector<UINT>{VK_CONTROL, VK_SHIFT, 'V'}));
    assert((XiaomiKeymapInjectUpVks(combo) ==
           std::vector<UINT>{'V', VK_SHIFT, VK_CONTROL}));
    const auto winCombo = ParseKeySpec("win+down").value();
    assert((XiaomiKeymapInjectDownVks(winCombo) ==
           std::vector<UINT>{VK_LWIN, VK_DOWN}));

    // ---- 决策状态机（keyup 后置决策版，2026-09-07 三次修复:LL 钩子吞掉的
    // 按键既无 MAKE raw 也无 BREAK raw——按键时刻在用户态拿不到任何设备证据,
    // 先验判定（全局信用热注入）必然存在物理键误映射率。改为:keydown 只吞
    // 不注入（零副作用）,keyup 放行让 BREAK 沿随投递（hDevice = 可靠证据）,
    // 归属判定后收尾——遥控器注入映射 down+up 对,物理键盘补偿原键对。 ----
    const std::map<std::string, std::string> key_map = {
        {"back", "backspace"}, {"home", "ctrl+shift+v"}, {"tv", "win+down"}};
    constexpr std::int64_t kNow = 500000;

    // 无映射按键：不吞不注入（key_map 未覆盖 ok/up/down 等）。
    {
        XiaomiKeymapInterceptor local;
        const auto a = local.OnKeyDown("ok", VK_RETURN, 0x1C, kNow, key_map);
        assert(!a.swallow && a.inject.empty() && !local.HasPending());
    }
    // 空串显式取消 / 非法 key_spec 串（配置层已过滤，防御）：放行。
    {
        XiaomiKeymapInterceptor local;
        const std::map<std::string, std::string> cancelled{{"back", ""}};
        const auto a = local.OnKeyDown("back", VK_BROWSER_BACK, 0, kNow, cancelled);
        assert(!a.swallow && a.inject.empty());
        XiaomiKeymapInterceptor local2;
        const std::map<std::string, std::string> bogus{{"back", "not a key"}};
        const auto b = local2.OnKeyDown("back", VK_BROWSER_BACK, 0, kNow, bogus);
        assert(!b.swallow && b.inject.empty());
    }
    // keydown 一律吞 + 登记 pending，不注入（副作用留到归属判定后）。
    {
        XiaomiKeymapInterceptor local;
        const auto a = local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        assert(a.swallow && a.inject.empty());
        assert(local.HasPending());
        assert(local.PendingAwaitingBreak().empty());      // 尚未松开
    }
    // pending 中自动重复 keydown：吞，不注入、不新建 pending。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        const auto repeat = local.OnKeyDown("home", VK_HOME, 0x71, kNow + 200,
                                            key_map);
        assert(repeat.swallow && repeat.inject.empty());
    }
    // keyup：pending 中放行（让 BREAK 沿投递提供设备证据），标记待判定。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        const auto up = local.OnKeyUp("home", kNow + 120, key_map);
        assert(!up.swallow && up.inject.empty());
        const auto awaiting = local.PendingAwaitingBreak();
        assert(awaiting.size() == 1 && awaiting[0].first == "home");
    }
    // 未归属 keyup（无 pending）：放行。
    {
        XiaomiKeymapInterceptor local;
        const auto up = local.OnKeyUp("back", kNow, key_map);
        assert(!up.swallow && up.inject.empty());
    }
    // BREAK 佐证=遥控器：注入映射 down+up 对，消费 pending。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        local.OnKeyUp("home", kNow + 120, key_map);
        const auto resolve = local.OnBreakEvidence("home", kNow + 123, true,
                                                   key_map);
        assert(resolve.has_value());
        assert((resolve->inject == std::vector<UINT>{VK_CONTROL, VK_SHIFT, 'V'}));
        assert((resolve->inject_up ==
                std::vector<UINT>{'V', VK_SHIFT, VK_CONTROL}));
        assert(!local.HasPending());
    }
    // BREAK 佐证=物理键盘：补偿注入原键 down+up 对，消费 pending。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        local.OnKeyUp("home", kNow + 120, key_map);
        const auto resolve = local.OnBreakEvidence("home", kNow + 123, false,
                                                   key_map);
        assert(resolve.has_value());
        assert((resolve->inject == std::vector<UINT>{VK_HOME}));
        assert((resolve->inject_up == std::vector<UINT>{VK_HOME}));
        assert(!local.HasPending());
    }
    // BREAK 佐证晚于兜底窗（超时已补偿）：丢弃，不注入（防双击）。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        local.OnKeyUp("home", kNow + 120, key_map);
        const auto bail = local.OnPendingTimeout("home", kNow + 400);
        assert(bail.has_value());
        const auto late = local.OnBreakEvidence("home", kNow + 450, true,
                                                key_map);
        assert(!late.has_value());
    }
    // 无 pending / 未松开的 BREAK：忽略（残留或按住中）。
    {
        XiaomiKeymapInterceptor local;
        assert(!local.OnBreakEvidence("tv", kNow, true, key_map).has_value());
        local.OnKeyDown("back", VK_BROWSER_BACK, 0, kNow, key_map);  // 按住中
        assert(!local.OnBreakEvidence("back", kNow + 50, true, key_map)
                    .has_value());
    }
    // 兜底超时（keyup 放行后 BREAK 迟迟不来，异常丢失保护）：补偿原键对。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        local.OnKeyUp("home", kNow + 120, key_map);
        // 窗内（kBreakEvidenceWindowMs）不算超时。
        assert(!local.OnPendingTimeout(
                    "home", kNow + 120 + XiaomiKeymapInterceptor::kBreakEvidenceWindowMs)
                    .has_value());
        const auto bail = local.OnPendingTimeout(
            "home", kNow + 120 + XiaomiKeymapInterceptor::kBreakEvidenceWindowMs + 1);
        assert(bail.has_value());
        assert((bail->inject == std::vector<UINT>{VK_HOME}));
        assert((bail->inject_up == std::vector<UINT>{VK_HOME}));
        assert(!local.HasPending());
    }
    // 按住中（keyup 未到）不触发兜底：按键长按是正常状态。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        assert(!local.OnPendingTimeout("home", kNow + 10000).has_value());
        assert(local.HasPending());
    }
    // 多按钮并发 pending（遥控器同报多键：home+tv 齐按）各自独立判定。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        local.OnKeyDown("tv", VK_OEM_3, 0x29, kNow + 5, key_map);
        local.OnKeyUp("home", kNow + 100, key_map);
        const auto home = local.OnBreakEvidence("home", kNow + 103, true,
                                                key_map);
        assert(home.has_value());
        assert(local.HasPending());                          // tv 仍待判定
        local.OnKeyUp("tv", kNow + 150, key_map);
        const auto tv = local.OnBreakEvidence("tv", kNow + 153, false, key_map);
        assert(tv.has_value());
        assert(!local.HasPending());
    }
    // 映射在判定前被取消（重配竞态）：不注入任何键（原键已放行等效原生）。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        local.OnKeyUp("home", kNow + 120, key_map);
        const std::map<std::string, std::string> empty_map;
        assert(!local.OnBreakEvidence("home", kNow + 123, true, empty_map)
                    .has_value());
        assert(!local.HasPending());
    }
    // Reset 清全部状态：pending 与按住记录。
    {
        XiaomiKeymapInterceptor local;
        local.OnKeyDown("home", VK_HOME, 0x71, kNow, key_map);
        local.Reset();
        assert(!local.HasPending());
        const auto after = local.OnKeyDown("home", VK_HOME, 0x71, kNow + 10,
                                           key_map);
        assert(after.swallow);                               // 重新走 pending
    }

}

// F5 抑制谓词：enabled 且 last>0 且 0<=age<=80ms 时吞，其余一律放行。
void TestXiaomiF5SuppressPredicate() {
    constexpr std::int64_t kLast = 100000;
    // 窗内（含 0/80ms 边界）吞。
    assert(ShouldSuppressF5(kLast, kLast, true));
    assert(ShouldSuppressF5(kLast + 79, kLast, true));
    assert(ShouldSuppressF5(kLast + kF5SuppressWindowMs, kLast, true));
    // 窗外（81ms）放行。
    assert(!ShouldSuppressF5(kLast + kF5SuppressWindowMs + 1, kLast, true));
    // 开关关闭放行。
    assert(!ShouldSuppressF5(kLast, kLast, false));
    // 从未开麦（last=0）放行。
    assert(!ShouldSuppressF5(kLast, 0, true));
    // 未来时间戳（时钟回拨/乱序，age<0）放行。
    assert(!ShouldSuppressF5(kLast - 1, kLast, true));
}

// 协调器 × 小米能力门控（规格 §4.4/§5.2）：RC 设备不下发交互/编码器设置、不走
// VoiceStick 固件更新；对照组 StickS3 设备行为不变。小米身份走 config 种子兜底路径
//（paired_devices 条目带 hardware，IsXiaomiRemoteDevice 直接命中），不注入
// device_info 事件，因此完全不触达 SavePairedDeviceInfo 落盘真实 config.toml。

// Suite entry: core_tests.cc main() calls this once.
void RunXiaomiUsageTapBatchTests() {
    TestXiaomiUsageTapSessionEdges();
    TestXiaomiTapDirectKeys();
    TestXiaomiGatewayKeyRepeater();
    TestXiaomiTapEvidenceTable();
    TestXiaomiTapFrameDecoder();
    TestXiaomiHostPidValueParsing();
    TestXiaomiKeymapInterceptor();
    TestXiaomiF5SuppressPredicate();
}

