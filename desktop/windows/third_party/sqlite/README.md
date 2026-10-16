# sqlite（vendored）

SQLite amalgamation 3.50.4（sqlite3.c / sqlite3.h），公有领域（public domain）。

- 来源：https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
- 用途：用户数据飞轮会话历史（`src/session_history.{h,cc}`，typeless 方案 P0-1
  Windows 接线；与 macOS `desktop/macos/Sources/VoiceStickCore/SessionHistory.swift`
  同 schema 同迁移）。
- 升级：整体替换 sqlite3.c/sqlite3.h 并更新本 README 版本号。
