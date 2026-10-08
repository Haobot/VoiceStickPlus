// N8：core_tests.cc 首刀拆分——**协议契约簇**（state/keymap/proto/ui_state 帧
// 解析与预算）迁至本文件；纯 BleProtocol、无 FakeBleCentral 依赖。与
// core_tests.cc 共用 test_suites.h 的套件声明（后续按域继续拆）。
// 拆分纪律：测试体逐字迁移、cluster 标记随套件走，注册处收敛为一次调用。
#include "ble_protocol.h"

#include "byte_utils.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void TestStateParsing() {
    const std::string json = "{\"event\":\"button_down\",\"button\":\"primary\",\"session_id\":42}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "button_down");
    assert(event->button == "primary");
    assert(event->session_id == 42);
}

// 网关按键事件（P1 隧道融合）：key/pressed 字段解析与命令 payload 构造。
// D10：网关路由回执解析——Windows 此前完全不解析被静默丢弃（路由 UI 无法回显）。
void TestGatewayKeymapReceiptParsing() {
    const std::string json =
        R"({"event":"gateway_keymap","routes":[{"key":"back","route":"software"},{"key":"menu","route":"passthrough"}]})";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "gateway_keymap");
    assert(event->keymap_routes.size() == 2);
    assert(event->keymap_routes[0].key == "back");
    assert(event->keymap_routes[0].route == "software");
    assert(event->keymap_routes[1].key == "menu");
    assert(event->keymap_routes[1].route == "passthrough");

    // 非回执事件不产生 routes。
    const std::string other = R"({"event":"gateway_status","mode":"gateway"})";
    ByteVector other_frame = {1, 0x10};
    AppendLe16(other_frame, static_cast<std::uint16_t>(other.size()));
    other_frame.insert(other_frame.end(), other.begin(), other.end());
    auto other_event = BleProtocol::ParseStateEvent(other_frame);
    assert(other_event.has_value());
    assert(other_event->keymap_routes.empty());

    // A15：分片回执——seq/more 解析与累计器（seq0 重启未完成累计、more=false 收口）。
    const std::string c0_json = R"({"event":"gateway_keymap","seq":0,"more":true,)"
                                R"("routes":[{"key":"back","route":"software"}]})";
    ByteVector c0_frame = {1, 0x10};
    AppendLe16(c0_frame, static_cast<std::uint16_t>(c0_json.size()));
    c0_frame.insert(c0_frame.end(), c0_json.begin(), c0_json.end());
    auto c0 = BleProtocol::ParseStateEvent(c0_frame);
    assert(c0.has_value() && c0->keymap_seq == 0 && c0->keymap_more);
    assert(c0->keymap_routes.size() == 1 && c0->keymap_routes[0].key == "back");

    const std::string c1_json = R"({"event":"gateway_keymap","seq":1,"more":false,)"
                                R"("routes":[{"key":"menu","route":"passthrough"}]})";
    ByteVector c1_frame = {1, 0x10};
    AppendLe16(c1_frame, static_cast<std::uint16_t>(c1_json.size()));
    c1_frame.insert(c1_frame.end(), c1_json.begin(), c1_json.end());
    auto c1 = BleProtocol::ParseStateEvent(c1_frame);
    assert(c1.has_value() && c1->keymap_seq == 1 && !c1->keymap_more);

    std::vector<StateEvent::KeyRoute> pending;
    assert(!BleProtocol::AccumulateKeymap(pending, *c0));   // more → 未完成
    assert(pending.size() == 1);
    assert(BleProtocol::AccumulateKeymap(pending, *c1));    // 收口 = 完整表
    assert(pending.size() == 2 && pending[0].key == "back" && pending[1].key == "menu");

    // seq0 重启未完成累计（帧丢失/新表自愈）。
    assert(!BleProtocol::AccumulateKeymap(pending, *c0));
    assert(pending.size() == 1);

    // 旧式单帧（无 seq/more 字段）→ seq0/false → 即到即完整（向后兼容）。
    assert(event->keymap_seq == 0 && !event->keymap_more);
    std::vector<StateEvent::KeyRoute> legacy_pending;
    assert(BleProtocol::AccumulateKeymap(legacy_pending, *event));
    assert(legacy_pending.size() == 2);
}

// A14：ui_state 帧预算（ATT MTU247-3=244B，protocol.md）——超长 text 按 UTF-8 边界
// 截断、state 恒完整；原发送端零校验 → 固件 512B 缓冲截出半截 JSON → 整帧丢（连
// state 一起丢，设备屏卡 thinking）。
void TestUiStateBudget() {
    // 正常帧不改。
    auto ok = BleProtocol::UiStatePayload("thinking", "ok");
    std::string ok_s(ok.begin(), ok.end());
    assert(ok_s.find("\"text\":\"ok\"") != std::string::npos);

    // ASCII 长文 → 帧 ≤244 且 state/截断内容俱在。
    auto long_ascii = BleProtocol::UiStatePayload("thinking", std::string(500, 'x'));
    assert(long_ascii.size() <= 244);
    std::string la(long_ascii.begin(), long_ascii.end());
    assert(la.find("\"state\":\"thinking\"") != std::string::npos);
    assert(la.find("xxx") != std::string::npos);
    assert(la.size() >= 2 && la[la.size() - 1] == '}' && la[la.size() - 2] == '"');

    // 中文长文（多字节）→ 帧 ≤244；每轮整体重建 JSON，不会切出半截转义/码点。
    std::string zh;
    for (int i = 0; i < 200; i++) zh += "\xE7\x83\xAD";  // 热 ×200
    auto long_zh = BleProtocol::UiStatePayload("recording", zh);
    assert(long_zh.size() <= 244);
    std::string zs(long_zh.begin(), long_zh.end());
    assert(zs.find("\"state\":\"recording\"") != std::string::npos);
    assert(zs.size() >= 2 && zs[zs.size() - 1] == '}' && zs[zs.size() - 2] == '"');
}

// D9：协议版本协商——proto_info 解析 + proto_negotiate 构造 + 版本常量语义
//（protocol.md「Protocol version & negotiation」；三端常量同步演进）。
void TestProtoVersionNegotiation() {
    const std::string json = R"({"event":"proto_info","proto":1,"min_proto":1})";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "proto_info");
    assert(event->proto.value_or(-1) == BleProtocol::kProtocolVersion);
    assert(event->min_proto.value_or(-1) == BleProtocol::kProtocolMinVersion);

    // 桌面端上报帧：值取自同一常量（帧首字节与 JSON proto 同源）。
    auto payload = BleProtocol::ProtoNegotiatePayload();
    std::string sent(payload.begin(), payload.end());
    assert(sent.find(R"("event":"proto_negotiate")") != std::string::npos);
    assert(sent.find("\"proto\":" +
                    std::to_string(BleProtocol::kProtocolVersion)) != std::string::npos);

    // 非 proto_info 事件不带版本字段（缺省语义）。
    const std::string other = R"({"event":"gateway_status","mode":"gateway"})";
    ByteVector other_frame = {1, 0x10};
    AppendLe16(other_frame, static_cast<std::uint16_t>(other.size()));
    other_frame.insert(other_frame.end(), other.begin(), other.end());
    auto other_event = BleProtocol::ParseStateEvent(other_frame);
    assert(other_event.has_value() && !other_event->proto.has_value() &&
           !other_event->min_proto.has_value());
}

void TestGatewayKeyStateParsing() {
    const std::string json =
        "{\"event\":\"gateway_key\",\"key\":\"volume_up\",\"pressed\":true}";
    ByteVector frame = {1, 0x10};
    AppendLe16(frame, static_cast<std::uint16_t>(json.size()));
    frame.insert(frame.end(), json.begin(), json.end());
    auto event = BleProtocol::ParseStateEvent(frame);
    assert(event.has_value());
    assert(event->event == "gateway_key");
    assert(event->gateway_key == "volume_up");
    assert(event->gateway_pressed.has_value());
    assert(event->gateway_pressed.value());

    // 松开沿与缺字段容错
    const std::string up_json =
        "{\"event\":\"gateway_key\",\"key\":\"back\",\"pressed\":false}";
    ByteVector up_frame = {1, 0x10};
    AppendLe16(up_frame, static_cast<std::uint16_t>(up_json.size()));
    up_frame.insert(up_frame.end(), up_json.begin(), up_json.end());
    auto up_event = BleProtocol::ParseStateEvent(up_frame);
    assert(up_event.has_value());
    assert(up_event->gateway_key == "back");
    assert(up_event->gateway_pressed.has_value());
    assert(!up_event->gateway_pressed.value());

    // 非 gateway_key 事件不带网关字段
    const std::string other =
        "{\"event\":\"button_down\",\"button\":\"primary\",\"session_id\":7}";
    ByteVector other_frame = {1, 0x10};
    AppendLe16(other_frame, static_cast<std::uint16_t>(other.size()));
    other_frame.insert(other_frame.end(), other.begin(), other.end());
    auto other_event = BleProtocol::ParseStateEvent(other_frame);
    assert(other_event.has_value());
    assert(other_event->gateway_key.empty());
    assert(!other_event->gateway_pressed.has_value());

    // 路由命令 payload：软件路由与直通
    const auto software_payload = BleProtocol::GatewayKeymapSetPayload("back", true);
    const std::string software_json(software_payload.begin(), software_payload.end());
    assert(software_json.find("\"event\":\"gateway_keymap_set\"") != std::string::npos);
    assert(software_json.find("\"key\":\"back\"") != std::string::npos);
    assert(software_json.find("\"route\":\"software\"") != std::string::npos);
    const auto pass_payload = BleProtocol::GatewayKeymapSetPayload("ok", false);
    const std::string pass_json(pass_payload.begin(), pass_payload.end());
    assert(pass_json.find("\"route\":\"passthrough\"") != std::string::npos);

    // P1 切换器目标选择 payload：self 与 clear 两种形态（桌面端 --gateway-target）。
    const auto self_select = BleProtocol::GatewaySelectTargetPayload(true);
    const std::string self_json(self_select.begin(), self_select.end());
    assert(self_json == "{\"event\":\"gateway_select_target\",\"self\":true}");
    const auto clear_select = BleProtocol::GatewaySelectTargetPayload(false);
    const std::string clear_json(clear_select.begin(), clear_select.end());
    assert(clear_json == "{\"event\":\"gateway_select_target\",\"clear\":true}");
}


// N8：套件入口——core_tests.cc 的 main() 只调这里（域内顺序即原注册顺序）。
void RunProtocolContractTests() {
    TestStateParsing();
    printf(">> cluster: D10 gateway_keymap receipt parsing\n"); fflush(stdout);
    TestGatewayKeymapReceiptParsing();
    printf(">> cluster: D9 proto version negotiation\n"); fflush(stdout);
    TestProtoVersionNegotiation();
    printf(">> cluster: D14 ui_state frame budget\n"); fflush(stdout);
    TestUiStateBudget();
    TestGatewayKeyStateParsing();
}
