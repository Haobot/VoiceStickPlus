// 临时 bench CLI（/tmp）：批量生成对齐候选 + 与 App 同构的 ollama 请求体。
// 用法：align_cli <model_name> < stdin(JSONL: id/command/target/remaining) > stdout(JSONL)
import Foundation

struct CaseIn: Decodable { let id: String; let command: String; let target: String; let remaining: String }

func run() {
    let model = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "test-model"
    let data = FileHandle.standardInput.readDataToEndOfFile()
    let decoder = JSONDecoder()
    var out = ""
    for line in String(data: data, encoding: .utf8)!.split(separator: "\n") where !line.isEmpty {
        guard let c = try? decoder.decode(CaseIn.self, from: Data(line.utf8)) else { continue }
        let cands = VoiceTargetedAlign.candidates(asrTarget: c.target, remainingText: c.remaining)
        let texts = cands.map { $0.text }
        let body = VoiceTargetedAlign.chatRequestBody(
            model: model, command: c.command, asrTarget: c.target,
            remainingText: c.remaining, candidates: texts)
        let obj: [String: Any] = [
            "id": c.id,
            "candidates": cands.map { ["text": $0.text, "score": $0.score, "utf16": $0.utf16Start] },
            "request": body
        ]
        let j = try! JSONSerialization.data(withJSONObject: obj)
        out += String(data: j, encoding: .utf8)! + "\n"
    }
    FileHandle.standardOutput.write(Data(out.utf8))
}
run()
