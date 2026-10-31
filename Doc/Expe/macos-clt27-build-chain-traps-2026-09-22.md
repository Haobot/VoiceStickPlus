# macOS CLT（macOS 27 SDK）构建链三坑：x86_64 链接已死 / 产物布局变更 / 边构建边改代码竞态

- 日期：2026-09-22
- 相关文件：`scripts/build-macos.sh`、`Doc/Agent/build-and-test.md`、`desktop/macos/Package.swift`
- 相关 commit：本次改动未提交（工作区状态）；文中路径/行号为记录时点结论，引用前以当前源码为准。

## 坑 1：x86_64 链接报 `Undefined symbols __swift_FORCE_LOAD_$_swiftCompatibility56`

- 症状：universal 构建中 arm64 一切正常，x86_64 编译也过，死在链接期（app 与 VoiceStickTests 都中）。
- 判据：`ld: warning: ignoring file '.../libswiftCompatibility56.a': fat file missing arch 'x86_64', file has 'arm64,arm64e'`（`libswiftCompatibilityPacks.a` 同款）+ `Undefined symbols for architecture x86_64: "__swift_FORCE_LOAD_$_swiftCompatibility56"`；伴随 `The x86_64 architecture is deprecated for your deployment target (macOS 27.0)`。
- 根因：CLT-only 机器（无 Xcode）升到 macOS 27 后，`/Library/Developer/CommandLineTools/usr/lib/swift/macosx/` 下的 Swift 回填兼容静态库只剩 arm64/arm64e slice。部署目标 macOS 12 的 Swift 并发回填依赖这两个库 → **x86_64 Swift 可执行文件链接必失败**。对照：9/5 构建的 `VoiceStick-2.3.8.app` 是 universal，说明旧工具链没问题，是工具链升级引入的硬限制。
- 修复：`build-macos.sh` 增加 `VOICESTICK_ARCHS`（默认仍 `arm64 x86_64`，发布机构建不变）；本机构建固定 `VOICESTICK_ARCHS=arm64 scripts/build-macos.sh --debug`，单架构时 `cp` 取代 `lipo`。
- 验证：arm64-only debug 包构建通过、自签验签通过、用户真机测试通过（2026-09-22）。

## 坑 2：swift build 报 Build complete，下一步取产物 `No such file or directory`

- 判据：`swift build` 成功，脚本 cp/lipo 报 `.build-<arch>/<triple>/<config>/VoiceStickApp: No such file or directory`。
- 根因：新工具链产物布局从 `.build/<triple>/<config>/` 变为 **`.build/<arch>/out/Products/<Config>/`**（scratch 下出现 `out/Intermediates.noindex`、`manifest.pif` 等 Xcode 风格布局；`Sparkle.framework` 已被 SwiftPM 直接铺在 Products 目录里，与可执行文件并列）。
- 修复：脚本 `bin_dir_for_arch()` 双布局探测（旧布局 → 新布局 → 找不到显式报错退出）；`Sparkle.framework` 优先取 Products 目录那份（与链接所用必然一致），取不到再回退 artifacts 查找；`sign_update` 查找改按首选架构 scratch。
- 通用规则：**任何脚本取 SwiftPM 产物路径都先做存在性探测**，不要写死 `.build/<triple>/<config>`——工具链升级后老脚本第一个断点通常就是产物路径和链接库架构。

## 坑 3：arm64 编译过、x86_64 报 `cannot find type 'X' in scope`（X 明明在源码树里）

- 判据：universal 构建中只有单一架构报某类型找不到；类型定义在新文件里；两架构共用同一源码树，另一架构编译通过。
- 根因：**边构建边改代码竞态**。`build-macos.sh` 先串行编译各架构、后统一组装；若在某架构编译完成后、下一架构编译计划生成前落盘新文件/新引用，就出现「文件清单是旧的、源文件是新的」错位。本次实录：`GatewaySupport.swift`（定义 `GatewayKeymapRoute`）在 x86_64 计划生成后落盘，而 `BleProtocol.swift` 的新引用是编译时从磁盘现读的 → arm64 侥幸全过、x86_64 缺类型。
- 修复：无需代码修复，重跑即消失。判别顺序：先看报错类型定义文件的 mtime 与构建时间线，再考虑 SwiftPM target 成员/条件编译——不要一上来怀疑代码本身。

## 长期技术记忆 / 经验

- 本机（CLT-only + macOS 27 SDK）macOS 桌面端构建固定姿势：`export https_proxy=http://127.0.0.1:5782 http_proxy=http://127.0.0.1:5782`（GitHub 依赖，直连不通）+ `VOICESTICK_ARCHS=arm64`（x86_64 链接已死）。universal 发布包在发布机构建（有 Xcode 或旧 SDK）。
- 坑 3 与坑 1 的复合表象：「单架构缺类型」+「单架构链接失败」叠加时逐坑独立判据，别混为一个根因。
- 关联文档：ad-hoc 签名 .app 启动即崩（different Team IDs）见 `Doc/Expe/app-update-mechanism-and-fork-migration-2026-07-30.md`；自签证书固定 TCC 身份的完整排障链见 `Doc/Expe/xiaomi-remote-macos-hid-seize-not-permitted-2026-09-03.md` 追加节。

## 遗留 / 观察项

- `Doc/Expe/claude-memory-distilled.md` 末尾存在未解决合并冲突（`<<<<<<< HEAD` vs `>>>>>>> feat/add-MiRemote`，入侧为空），2026-09-22 新增条目追加在冲突标记之后，解决冲突时保留即可。
- x86_64 的未来：macOS 27 SDK 已 deprecated x86_64；未来 SDK 彻底移除后应评估 drop x86_64（发布 universal 的意义随 Intel Mac 淘汰递减）。
