# Windows COM 口烧录工具（VoiceStickFlash）实施与串口枚举修复（2026-08-07）

## 需求与成果

按 `Doc/Plan/windows-com-flash-tool.md` 实施 Windows COM 口烧录工具（BLE OTA 之外的用户级兜底链路），随后又修了一个串口枚举 bug 和引导页阻断问题：

1. **core 层（voicestick_core，4 个新模块，全部可单测）**：`com_port_selector`（SetupAPI 枚举+关键字评分，与 `scripts/idf_cli.yaml` 一致）、`esptool_flash_command`（命令序列构建）、`esptool_progress`（输出解析为 FlashEvent）、`voice_stick_flash_tool`（IFlashProcessRunner 抽象 + EsptoolSubprocessRunner + FlashTool 编排 + SHA256）。
2. **shell 层**：`flash_tool_dialog`（声明式布局表，范式同 §3.6，子进程事件 `PostMessageW(kMsgFlashEvent)` 回 UI 线程）、`flash_tool_main.cc`、`flash_tool.rc`（图标路径必须正斜杠）、`flash_tool.manifest`（POST_BUILD mt.exe 嵌入，同 §3.7 清单坑）。CMake 新增 `VoiceStickFlash` WIN32 target，core 链 `setupapi`。
3. **入口**：托盘「固件烧录工具…」（`kMenuFlashTool`）+ 固件更新对话框「高级… COM 口烧录」按钮，`LaunchFlashToolExe` 找 exe 同级 `VoiceStickFlash.exe` 后 ShellExecuteW。
4. **MSI 打包**：`scripts/prepare_flash_payload.ps1`（python-embed + pip 装 esptool，幂等）+ 扫描生成 `flash_payload.wxs` 片段，主 wxs `ComponentGroupRef` 引用。MSI 验证：23.4MB / 2012 文件 / 含 VoiceStickFlash.exe 与 1 个 python.exe。
5. **串口枚举修复**：烧录工具下拉框刷不出任何串口（见教训 1）。
6. **引导页放行**：设备步允许不配对直接进下一步（见教训 6）。

验证：构建 + CTest 全绿（含新增 4 组单测：端口评分/命令构建/进度解析/完整流程）；payload 冒烟 `esptool v5.2.0`；probe 程序实测枚举出 5 个串口并自动选中 COM17（VID_303A）。真机烧录（需求书 §7.2）未执行。

## 关键教训

1. **`SetupDiGetClassDevsW` 传设备接口类 GUID 必须带 `DIGCF_DEVICEINTERFACE`**（本次实际踩坑）。`GUID_DEVINTERFACE_COMPORT` 是接口类不是安装类；只传 `DIGCF_PRESENT` 时按安装类匹配，不存在该安装类，**枚举恒为空**——症状是"刷新多少次都没有串口"。单测只能覆盖评分/解析等纯函数，系统枚举必须用 probe 程序（cl 直接链接 `voicestick_core.lib` 调 `EnumerateComPorts()`，编译参数要 `/utf-8`（项目源含中文注释）+ `/MTd`（匹配 core 的静态调试运行时）+ `setupapi.lib advapi32.lib`）或真机验证。
2. **Edit/Write 工具写 .ps1 会丢 UTF-8 BOM**。`prepare_flash_payload.ps1` 含中文注释，PowerShell 5.1 对无 BOM 文件按 GBK 读会破坏解析（同 §3.4 便携包教训）。每次用工具编辑后必须 `sed -i '1s/^/\xef\xbb\xbf/'` 补回 BOM。
3. **Git Bash 调 cmd 的三个坑**：
   - 未加引号的 `\\` 被 bash 吞掉（`cmd //c scripts\\build.bat` → `scriptsbuild.bat` 找不到），路径用引号或正斜杠；
   - `cmd //c "start \"\" app.exe"` 拉 GUI 报"拒绝访问"（GBK 乱码 `ܾʡ`），改用 `powershell -NoProfile -Command "Start-Process ..."` 即可；
   - `taskkill //IM` 也可能 Access denied（输出是 GBK 乱码，先iconv或凭经验识别），用 `Stop-Process -Name VoiceStick -Force` 更稳。
4. **本机增量构建环境**：cmake 不在 PATH，在 `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`；vcvars64 在 BuildTools 同树；`INCLUDE` 必须 prepend `desktop/windows/generated_winrt`；ctest 用同 bin 目录的 ctest.exe 且 `--test-dir` 用正斜杠（反斜杠在 bash 里被吞）。链接前必须 `Stop-Process VoiceStick`，否则 LNK1104 锁文件（§3.1 的又一个实例）。
5. **wix 版本决定打包写法**：本机 wix v4.0.5 没有 v5 的 `<Files>` 元素，2008 个 payload 文件不能自动收编；方案是脚本扫描 payload 生成 `flash_payload.wxs` 片段（逐文件 Component，组件 ID 取相对路径 SHA1 前 32 位保证合法且稳定），主 wxs 只写 `<ComponentGroupRef>`，wix build 传两个源文件。pip 默认索引用官方 PyPI（清华镜像无 esptool），`VOICESTICK_PIP_INDEX_URL` 可覆盖。
6. **阻塞式引导不能锁死修复工具入口**：onboarding 取消即退出应用（`win32_app.cc` `ShowOnboardingIfNeeded` 返回 false → ShutdownAndQuit），设备步又硬性要求已配对——设备无固件/固件过旧时根本 BLE 配对不上，用户永远到不了能烧固件的入口，死锁。修复：未配对时点下一步弹确认框放行（`kOnboardingSkipDeviceConfirm`，提示托盘烧录工具路径）。原则：**引导里的硬性前置，必须想清楚"该前置本身不可达"时用户怎么自救**。
7. **msilib 验证 MSI 内容**：Python 3.13 的 msilib 里 `View.Execute()` 必须显式传 `None`（`v.Execute(None)`），查询 `SELECT FileName FROM File` 可快速核对文件数与关键文件。

## 验证状态

- 构建通过；CTest `voicestick_windows_tests` 全绿（多次，含本地化表完整性检查）。
- probe 程序真机枚举：COM4/5/6/7（蓝牙）+ COM17（USB Serial VID_303A）全部列出，`SelectBestComPort` 自动选中 COM17。
- 未签名 MSI 构建成功并用 msilib 验证内容；VoiceStick.exe 多次重启日志正常。
- 未验证：烧录工具 GUI 实际操作、真机烧录（需求书 §7.2，需人执行）、引导页确认框实际弹出效果（代码路径直，风险低）。
