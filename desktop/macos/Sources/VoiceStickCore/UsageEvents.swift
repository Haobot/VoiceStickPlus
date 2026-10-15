import Foundation

// 负信号采集（用户数据飞轮 P1-1，wetype 方案 G3；对齐 WeType「完整动作历史：
// 上屏与回退都记录，正负样本都采集」）：待确认粘贴的确认/取消/识别中途取消/
// 确认停留时长全部落 JSONL 事件流水，供热词评分扩展负样本维度与「纠错对挖掘
// 只取用户实际放行样本」的消费方使用。
//
// 隐私红线：事件只记文本长度与热词命中名单（子串匹配的成员关系），绝不记
// 文本本身；纯本地追加，无遥测。

public struct UsageEvent: Equatable {
    /// 动作类型（UsageEventKind 常量）。
    public var kind: String
    public var ts: Int64              // epoch 秒
    public var deviceID: String?
    /// 本次结果文本长度（字符数；只记长度不记内容）。
    public var textLength: Int
    /// 结果文本命中的热词名单（成员关系，供评分负项归因）。
    public var hotwords: [String]
    /// 确认前停留秒数（确认/取消动作携带；识别中途取消为 nil）。
    public var dwellSeconds: Double?

    public init(kind: String, ts: Int64, deviceID: String? = nil, textLength: Int = 0,
                hotwords: [String] = [], dwellSeconds: Double? = nil) {
        self.kind = kind
        self.ts = ts
        self.deviceID = deviceID
        self.textLength = textLength
        self.hotwords = hotwords
        self.dwellSeconds = dwellSeconds
    }
}

public enum UsageEventKind {
    /// 待确认文本自动提交（倒计时走完/悬浮窗确认回调）。
    public static let pasteAutoConfirmed = "paste_auto_confirmed"
    /// 待确认文本手动确认（暂停后按前面板确认）。
    public static let pasteManuallyConfirmed = "paste_manually_confirmed"
    /// 待确认文本被取消（负信号）。
    public static let pasteCanceled = "paste_canceled"
    /// 识别进行中取消/短录音丢弃（负信号；无文本）。
    public static let recognitionCanceled = "recognition_canceled"
}

public enum UsageEventLog {
    /// JSONL 追加一行（文件不存在则创建；写失败静默 best-effort）。
    public static func append(url: URL, event: UsageEvent) {
        let object: [String: Any] = [
            "kind": event.kind,
            "ts": event.ts,
            "device": event.deviceID ?? "",
            "text_length": event.textLength,
            "hotwords": event.hotwords,
            "dwell_s": event.dwellSeconds as Any
        ]
        guard JSONSerialization.isValidJSONObject(object),
              let data = try? JSONSerialization.data(withJSONObject: object),
              let line = String(data: data, encoding: .utf8), let lineData = (line + "\n").data(using: .utf8) else {
            return
        }
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
        }
        guard let handle = try? FileHandle(forWritingTo: url) else { return }
        defer { try? handle.close() }
        _ = try? handle.seekToEnd()
        try? handle.write(contentsOf: lineData)
    }

    /// JSONL 读取（缺失/损坏行跳过；测试与后续评分消费用）。
    public static func load(url: URL) -> [UsageEvent] {
        guard let data = try? Data(contentsOf: url),
              let content = String(data: data, encoding: .utf8) else { return [] }
        var events: [UsageEvent] = []
        for line in content.split(separator: "\n") {
            guard let object = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
                  let kind = object["kind"] as? String,
                  let ts = object["ts"] as? Int64 else { continue }
            var event = UsageEvent(kind: kind, ts: ts)
            if let device = object["device"] as? String, !device.isEmpty { event.deviceID = device }
            if let length = object["text_length"] as? Int { event.textLength = length }
            if let hotwords = object["hotwords"] as? [String] { event.hotwords = hotwords }
            if let dwell = object["dwell_s"] as? Double { event.dwellSeconds = dwell }
            events.append(event)
        }
        return events
    }
}
