import Foundation
import SQLite3

// 会话历史数据底座（用户数据飞轮 P0-3，Typeless 方案 P0-1 的 history_v2 裁剪版；
// 逆向还原 schema 见 Doc/Plan/typeless-lessons-and-user-data-flywheel.md §1.1）：
// 每次识别会话落一行（raw/refined/injected + 设备/Provider/时长/下发热词），
// 为编辑追踪（P0-2）、dialog_ctx 接线（P1-6）、个人评测语料（P2-8）供数。
//
// 与 Typeless history_v2 的差异：本地优先定位，砍掉云同步字段（user_id/sync_status/
// server_updated_at 等）与音频 blob 列（保留 audio_local_path 挂接 debug_audio_cache
// 目录约定）；edited_* 三件套字段预留（编辑追踪分支接入）。
// 存储用系统 libsqlite3（`import SQLite3`，无新 SwiftPM 依赖；方案原文的 GRDB 建议
// 在本机 CLT-only 环境换用系统库直连，语义等价）。打开/迁移失败一律降级停用
//（init 返回 nil），历史记录绝不阻塞识别主链路。

/// edited_text 状态机初始态（Typeless 同名枚举）。
public let kSessionHistoryEditedStatusDefault = "NOT_EXTRACTED"

public struct SessionHistoryRecord: Equatable {
    public var id: String                 // 会话 UUID
    public var createdAt: Int64           // epoch 秒
    public var deviceID: String?          // 触发设备（VS-XXXX / 小米遥控）
    public var asrProvider: String?       // volcengine / tencent / voicestick_cloud
    public var mode: String?              // original / translate
    public var rawAsr: String?            // ASR 原文
    public var refinedText: String?       // 精修后文本
    public var injectedText: String?      // 实际注入内容（与 refined 可能不同，如翻译）
    public var editedText: String?        // 用户注入后修改的结果（编辑追踪产出）
    public var editedTextStatus: String   // NOT_EXTRACTED/PENDING/EXTRACTED/TIMEOUT/SKIPPED
    public var editedTextAttempts: Int
    public var audioLocalPath: String?    // 复用 debug_audio_cache 目录约定
    public var duration: Double?          // 录音时长秒
    public var focusedAppBundleID: String? // 仅 bundle id，不读窗口内容（隐私红线）
    public var hotwordsSent: String?      // JSON 数组：本会话 ASR 下发的热词集
    public var micDevice: String?
    public var clientMetadata: String?    // JSON：app 版本等

    public init(
        id: String, createdAt: Int64, deviceID: String? = nil, asrProvider: String? = nil,
        mode: String? = nil, rawAsr: String? = nil, refinedText: String? = nil,
        injectedText: String? = nil, editedText: String? = nil,
        editedTextStatus: String = kSessionHistoryEditedStatusDefault,
        editedTextAttempts: Int = 0, audioLocalPath: String? = nil, duration: Double? = nil,
        focusedAppBundleID: String? = nil, hotwordsSent: String? = nil, micDevice: String? = nil,
        clientMetadata: String? = nil
    ) {
        self.id = id
        self.createdAt = createdAt
        self.deviceID = deviceID
        self.asrProvider = asrProvider
        self.mode = mode
        self.rawAsr = rawAsr
        self.refinedText = refinedText
        self.injectedText = injectedText
        self.editedText = editedText
        self.editedTextStatus = editedTextStatus
        self.editedTextAttempts = editedTextAttempts
        self.audioLocalPath = audioLocalPath
        self.duration = duration
        self.focusedAppBundleID = focusedAppBundleID
        self.hotwordsSent = hotwordsSent
        self.micDevice = micDevice
        self.clientMetadata = clientMetadata
    }
}

public final class SessionHistoryStore {
    /// 默认库路径（与 config.toml 同目录）。
    public static var defaultURL: URL {
        AppConfig.configDirectory.appendingPathComponent("session_history.db")
    }

    /// 迁移 SQL（版本号 = 数组下标 + 1）。仿 Typeless drizzle 迁移：按序应用、
    /// schema_migrations 记版本，schema 演进只追加不回填。
    private static let migrations: [[String]] = [
        [
            """
            CREATE TABLE IF NOT EXISTS history (
              id TEXT PRIMARY KEY,
              created_at INTEGER NOT NULL,
              device_id TEXT,
              asr_provider TEXT,
              mode TEXT,
              raw_asr TEXT,
              refined_text TEXT,
              injected_text TEXT,
              edited_text TEXT,
              edited_text_status TEXT NOT NULL DEFAULT 'NOT_EXTRACTED',
              edited_text_attempts INTEGER NOT NULL DEFAULT 0,
              audio_local_path TEXT,
              duration REAL,
              focused_app_bundle_id TEXT,
              hotwords_sent TEXT,
              mic_device TEXT,
              client_metadata TEXT
            )
            """
        ]
    ]

    private static let SQLITE_TRANSIENT = unsafeBitCast(
        -1, to: sqlite3_destructor_type.self
    )

    private var handle: OpaquePointer?

    /// 打开并按序应用迁移；任何失败返回 nil（调用方降级停用）。
    public init?(url: URL) {
        let path = url.path
        guard !path.isEmpty else { return nil }
        var db: OpaquePointer?
        guard sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nil) == SQLITE_OK, let db else {
            sqlite3_close(db)
            return nil
        }
        handle = db
        // WAL：协调器主线程单写 + 未来设置页/导出读不互堵。
        exec("PRAGMA journal_mode=WAL")
        guard migrate() else {
            close()
            return nil
        }
    }

    deinit {
        close()
    }

    public func close() {
        if let handle {
            sqlite3_close_v2(handle)
            self.handle = nil
        }
    }

    private func exec(_ sql: String) -> Bool {
        var error: UnsafeMutablePointer<CChar>?
        guard sqlite3_exec(handle, sql, nil, nil, &error) == SQLITE_OK else {
            if let error {
                NSLog("session history exec failed: %@", String(cString: error))
                sqlite3_free(error)
            }
            return false
        }
        return true
    }

    private func migrate() -> Bool {
        guard exec("""
            CREATE TABLE IF NOT EXISTS schema_migrations (
              version INTEGER PRIMARY KEY,
              applied_at INTEGER NOT NULL
            )
            """) else { return false }
        var applied = Set<Int64>()
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(handle, "SELECT version FROM schema_migrations", -1, &statement, nil) == SQLITE_OK else {
            return false
        }
        while sqlite3_step(statement) == SQLITE_ROW {
            applied.insert(sqlite3_column_int64(statement, 0))
        }
        sqlite3_finalize(statement)
        for (index, statements) in SessionHistoryStore.migrations.enumerated() {
            let version = Int64(index + 1)
            guard !applied.contains(version) else { continue }
            guard exec("BEGIN") else { return false }
            var ok = true
            for sql in statements where ok {
                ok = exec(sql)
            }
            if ok {
                var insert: OpaquePointer?
                ok = sqlite3_prepare_v2(handle, "INSERT INTO schema_migrations(version, applied_at) VALUES(?, ?)", -1, &insert, nil) == SQLITE_OK
                if ok, let insert {
                    sqlite3_bind_int64(insert, 1, version)
                    sqlite3_bind_int64(insert, 2, Int64(Date().timeIntervalSince1970))
                    ok = sqlite3_step(insert) == SQLITE_DONE
                }
                sqlite3_finalize(insert)
            }
            guard exec(ok ? "COMMIT" : "ROLLBACK"), ok else { return false }
        }
        return true
    }

    /// 插入一行（best-effort，失败记日志静默——历史记录不阻塞主链路）。
    public func insert(_ record: SessionHistoryRecord) {
        guard let handle else { return }
        let sql = """
            INSERT OR REPLACE INTO history(
              id, created_at, device_id, asr_provider, mode, raw_asr, refined_text,
              injected_text, edited_text, edited_text_status, edited_text_attempts,
              audio_local_path, duration, focused_app_bundle_id, hotwords_sent,
              mic_device, client_metadata
            ) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(handle, sql, -1, &statement, nil) == SQLITE_OK, let statement else {
            NSLog("session history insert prepare failed")
            return
        }
        defer { sqlite3_finalize(statement) }
        bindText(statement, 1, record.id)
        sqlite3_bind_int64(statement, 2, record.createdAt)
        bindText(statement, 3, record.deviceID)
        bindText(statement, 4, record.asrProvider)
        bindText(statement, 5, record.mode)
        bindText(statement, 6, record.rawAsr)
        bindText(statement, 7, record.refinedText)
        bindText(statement, 8, record.injectedText)
        bindText(statement, 9, record.editedText)
        bindText(statement, 10, record.editedTextStatus)
        sqlite3_bind_int(statement, 11, Int32(record.editedTextAttempts))
        bindText(statement, 12, record.audioLocalPath)
        if let duration = record.duration {
            sqlite3_bind_double(statement, 13, duration)
        }
        bindText(statement, 14, record.focusedAppBundleID)
        bindText(statement, 15, record.hotwordsSent)
        bindText(statement, 16, record.micDevice)
        bindText(statement, 17, record.clientMetadata)
        if sqlite3_step(statement) != SQLITE_DONE {
            NSLog("session history insert failed: %s", sqlite3_errmsg(handle))
        }
    }

    /// 更新实际注入文本（翻译异步完成后回填 injected_text）。
    public func updateInjectedText(id: String, text: String) {
        guard let handle else { return }
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(handle, "UPDATE history SET injected_text = ? WHERE id = ?", -1, &statement, nil) == SQLITE_OK,
              let statement else { return }
        defer { sqlite3_finalize(statement) }
        bindText(statement, 1, text)
        bindText(statement, 2, id)
        _ = sqlite3_step(statement)
    }

    /// 编辑追踪结果回填（Typeless edited_* 三件套语义：状态机 + 尝试计数）。
    public func updateEditedText(id: String, text: String?, status: String, attempts: Int) {
        guard let handle else { return }
        var statement: OpaquePointer?
        let sql = "UPDATE history SET edited_text = ?, edited_text_status = ?, edited_text_attempts = ? WHERE id = ?"
        guard sqlite3_prepare_v2(handle, sql, -1, &statement, nil) == SQLITE_OK,
              let statement else { return }
        defer { sqlite3_finalize(statement) }
        bindText(statement, 1, text)
        bindText(statement, 2, status)
        sqlite3_bind_int(statement, 3, Int32(attempts))
        bindText(statement, 4, id)
        _ = sqlite3_step(statement)
    }

    /// 读回某行的 edited_* 三件套（测试与设置页展示用）。
    public func editedText(id: String) -> (text: String?, status: String, attempts: Int)? {
        guard let handle else { return nil }
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(
            handle, "SELECT edited_text, edited_text_status, edited_text_attempts FROM history WHERE id = ?",
            -1, &statement, nil
        ) == SQLITE_OK, let statement else { return nil }
        defer { sqlite3_finalize(statement) }
        bindText(statement, 1, id)
        guard sqlite3_step(statement) == SQLITE_ROW else { return nil }
        var text: String?
        if let cString = sqlite3_column_text(statement, 0) {
            text = String(cString: cString)
        }
        let status = sqlite3_column_text(statement, 1).map { String(cString: $0) } ?? ""
        let attempts = Int(sqlite3_column_int64(statement, 2))
        return (text, status, attempts)
    }

    /// 行数（测试与导出用）。
    public func rowCount() -> Int {
        guard let handle else { return 0 }
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(handle, "SELECT COUNT(*) FROM history", -1, &statement, nil) == SQLITE_OK,
              let statement else { return 0 }
        defer { sqlite3_finalize(statement) }
        guard sqlite3_step(statement) == SQLITE_ROW else { return 0 }
        return Int(sqlite3_column_int64(statement, 0))
    }

    /// 最近 N 条精修后文本（旧→新；P1-6 dialog_ctx 接线的数据源预留）。
    public func latestRefinedTexts(limit: Int) -> [String] {
        guard let handle else { return [] }
        var statement: OpaquePointer?
        guard sqlite3_prepare_v2(handle, "SELECT refined_text FROM history WHERE refined_text IS NOT NULL ORDER BY created_at DESC LIMIT ?", -1, &statement, nil) == SQLITE_OK,
              let statement else { return [] }
        defer { sqlite3_finalize(statement) }
        sqlite3_bind_int(statement, 1, Int32(limit))
        var texts: [String] = []
        while sqlite3_step(statement) == SQLITE_ROW {
            if let cString = sqlite3_column_text(statement, 0) {
                texts.append(String(cString: cString))
            }
        }
        return texts.reversed()
    }

    private func bindText(_ statement: OpaquePointer, _ index: Int32, _ value: String?) {
        if let value {
            sqlite3_bind_text(statement, index, value, -1, Self.SQLITE_TRANSIENT)
        } else {
            sqlite3_bind_null(statement, index)
        }
    }
}

/// edited_text 状态机取值（Typeless edited_text_status 同名枚举裁剪）。
public enum EditTrackingStatus {
    public static let pending = "PENDING"
    public static let extracted = "EXTRACTED"
    public static let timeout = "TIMEOUT"
    public static let skipped = "SKIPPED"
}

/// 编辑追踪纯逻辑（Typeless P0-2 的 diff 口径）。
public enum EditTracking {
    /// 计算用户相对注入文本的改动区间：对焦点字段与注入文本做公共前后缀对齐，
    /// 归因闸要求公共前后缀合计覆盖注入文本至少一半（真实编辑保留原注入的大部；
    /// 焦点切走的无关字段公共前后缀趋近 0，不作归因）。返回（注入侧中段, 字段侧
    /// 中段）；字段侧中段为空串 = 纯删除形态；nil = 未编辑或无法归因。
    /// 隐私红线：只取改动区间，绝不返回/落库整个字段内容。
    public static func editedRegion(
        fieldText: String, injectedText: String
    ) -> (before: String, after: String)? {
        guard !injectedText.isEmpty, fieldText != injectedText else { return nil }
        let field = Array(fieldText.unicodeScalars)
        let injected = Array(injectedText.unicodeScalars)
        var prefix = 0
        while prefix < min(field.count, injected.count),
              field[prefix] == injected[prefix] {
            prefix += 1
        }
        var suffix = 0
        while suffix < min(field.count, injected.count) - prefix,
              field[field.count - 1 - suffix] == injected[injected.count - 1 - suffix] {
            suffix += 1
        }
        guard prefix + suffix >= injected.count / 2 else { return nil }
        let before = String(String.UnicodeScalarView(injected[prefix..<(injected.count - suffix)]))
        let after = String(String.UnicodeScalarView(field[prefix..<(field.count - suffix)]))
        return (before, after)
    }
}
