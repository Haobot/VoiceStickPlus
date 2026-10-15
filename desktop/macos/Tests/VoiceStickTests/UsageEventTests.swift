import Foundation
import VoiceStickCore

// 负信号采集单测（用户数据飞轮 P1-1，wetype 方案 G3）：JSONL 追加/读回往返、
// 损坏行跳过、隐私红线（事件只含长度与热词名单，无文本内容）。

func runUsageEventTests() {
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_usage_events_test.jsonl")
    try? FileManager.default.removeItem(at: url)

    // 追加三行（确认/取消/识别中途取消）。
    UsageEventLog.append(url: url, event: UsageEvent(
        kind: UsageEventKind.pasteAutoConfirmed, ts: 1000, deviceID: "AB12",
        textLength: 42, hotwords: ["语气词"], dwellSeconds: 0.4
    ))
    UsageEventLog.append(url: url, event: UsageEvent(
        kind: UsageEventKind.pasteCanceled, ts: 2000, deviceID: "AB12",
        textLength: 18, hotwords: [], dwellSeconds: 5.1
    ))
    UsageEventLog.append(url: url, event: UsageEvent(
        kind: UsageEventKind.recognitionCanceled, ts: 3000
    ))

    let events = UsageEventLog.load(url: url)
    checkEqual(events.count, 3, "events：JSONL 三行读回")
    checkEqual(events[0].kind, UsageEventKind.pasteAutoConfirmed, "events：自动确认 kind")
    checkEqual(events[0].dwellSeconds ?? 0, 0.4, "events：停留时长读回")
    checkEqual(events[1].kind, UsageEventKind.pasteCanceled, "events：取消 kind")
    checkEqual(events[1].hotwords, [] as [String], "events：空热词名单")
    checkEqual(events[2].textLength, 0, "events：识别中途取消无文本")
    checkNil(events[2].dwellSeconds, "events：识别中途取消无停留时长")

    // 序列化内容不含文本字段（隐私红线：只记长度与热词名单）。
    let raw = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
    check(!raw.contains("\"text\":"), "events：序列化无 text 字段（隐私红线）")
    check(raw.contains("\"text_length\":"), "events：序列化含 text_length")

    // 损坏行跳过。
    let line = raw.split(separator: "\n").first.map(String.init) ?? ""
    try? (line + "\n{broken json\n").write(to: url, atomically: true, encoding: .utf8)
    checkEqual(UsageEventLog.load(url: url).count, 1, "events：损坏行跳过")

    // 缺失文件返回空。
    try? FileManager.default.removeItem(at: url)
    check(UsageEventLog.load(url: url).isEmpty, "events：缺失文件返回空")
}
