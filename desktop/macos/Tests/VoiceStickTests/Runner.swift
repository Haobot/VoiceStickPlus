import Foundation

/// 极简无框架断言（本机无 Xcode，XCTest/swift-testing 不可用）：
/// 每条断言计数，失败立即打印并记入失败清单；main.swift 末尾汇总 PASSED x/y，
/// 有失败 exit(1)。断言名沿用旧 XCTest 用例名 + 语义短句，便于逐条对照。
var totalChecks = 0
var failedChecks = 0
var failedNames: [String] = []

func check(_ condition: Bool, _ name: String, file: String = #fileID, line: Int = #line) {
    totalChecks += 1
    if !condition {
        failedChecks += 1
        failedNames.append(name)
        print("FAIL \(name) (\(file):\(line))")
    }
}

func checkEqual<T: Equatable>(_ actual: T, _ expected: T, _ name: String,
                              file: String = #fileID, line: Int = #line) {
    let ok = actual == expected
    check(ok, name, file: file, line: line)
    if !ok {
        print("  expected: \(expected)")
        print("  actual:   \(actual)")
    }
}

func checkNil(_ value: Any?, _ name: String, file: String = #fileID, line: Int = #line) {
    check(value == nil, name, file: file, line: line)
}

func checkNotNil(_ value: Any?, _ name: String, file: String = #fileID, line: Int = #line) {
    check(value != nil, name, file: file, line: line)
}

/// XCTUnwrap 等价物：失败记一条断言并返回 nil，调用方 guard let 提前 return。
func unwrap<T>(_ value: T?, _ name: String, file: String = #fileID, line: Int = #line) -> T? {
    check(value != nil, name, file: file, line: line)
    return value
}
