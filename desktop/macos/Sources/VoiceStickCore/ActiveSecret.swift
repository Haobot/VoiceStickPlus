import Foundation

/// 内置凭据回退解析（对齐 Windows ResolveActiveString）：配置值优先（trim 后
/// 非空即用），否则回退编译期内置值；不反向写回配置、不落盘。
public func resolveActiveString(_ configured: String, builtin: String) -> String {
    let trimmed = configured.trimmingCharacters(in: .whitespacesAndNewlines)
    return trimmed.isEmpty ? builtin : trimmed
}
