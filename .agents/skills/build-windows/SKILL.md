---
name: build-windows
description: >-
  Windows 桌面端（VoiceStick.exe / VoiceStickCtl.exe）的构建与测试流程。当用户说"构建 Windows / 编译 Windows /
  build windows / 跑 Windows 测试 / Windows test" 等，或在改完 desktop/windows/ 下 C++ 源码后需要验证编译时使用。
  包含 CMake 配置、Ninja 编译、CTest 运行，以及 WinSparkle 下载失败的本地缓存回退方案。
---

# Windows 桌面端构建与测试

在 Visual Studio 2022 BuildTools + Ninja 环境下构建 Windows 端，并运行 CTest。

## ⚠️ 增量构建的两个坑（2026-09-20 真机踩坑，务必先读）

1. **中文环境下改 .h 不会触发重编**：MSVC 输出 `注意: 包含文件:` 而非 `Note: including file:`，
   CMake 解析不了这个前缀 ⇒ **Ninja 从不记录头文件依赖** ⇒ 改了头文件（加字段/虚函数）后旧的
   `.obj` 照样参与链接。症状极具迷惑性：编译"成功"，但测试/运行期出现**确定性假失败**
   （实测：给 UI 接口加一个纯虚函数后 `core_tests` 报 `ui.show_listening_count == 1` 断言失败，
   真实原因是旧 TU 的 vtable 错位）。
   **更糟的变体（2026-09-20 夜实测）**：依赖表一旦不可信，**改 .cc 也可能不重编**——新增
   `VoiceStickCoordinator::SendGatewayTargetInfo` 后构建"成功"却报 `LNK2019 无法解析的外部符号`，
   手工 touch 该 .cc 立刻通过；同一晚还出现过测试二进制未重编导致 `FakeBleCentral 抽象类` 报错
   在"成功"的构建里被掩盖。**结论：这套构建的依赖判定整体不可信，别做选择性重编。**
   **做法**：构建前 `set VSLANG=1033`，并且**每次构建前 touch 全部自有源码**（src + tests 的 *.cc/*.h）——
   ```powershell
   Get-ChildItem desktop\windows\src,desktop\windows\tests -Recurse -Include *.cc,*.h |
       ForEach-Object { $_.LastWriteTime = Get-Date }
   ```
   全量重编我们的 TU 约 3-5 分钟，依赖缓存（WinRT 投影、llama.cpp）不受影响；**比"省 3 分钟但
   交付一个没生效的二进制"便宜得多**——今晚为此浪费了 3 轮构建 + 一次差点漏掉的测试回归。
2. **链接失败会被误判为成功**：`LNK1104 无法打开 VoiceStick.exe` 的根因是 **VoiceStick.exe 正在运行**
   （文件被占用）。任何自建增量脚本都必须先 `taskkill /f /im VoiceStick.exe`，并且**校验 exe 时间戳**
   或 `%ERRORLEVEL%`——只看"没有报错输出"会把没生效的构建当成成功（实测连续两次改动都没进二进制）。

## 前置环境

- Visual Studio 2022（含 C++ 工作负载，或 BuildTools）
- CMake 3.24+（通过 VS 安装）
- Ninja（通过 VS CMake 组件安装）
- WinSparkle 0.9.2（CMake FetchContent 自动下载，失败时可走本地回退）

## 一键构建

推荐从仓库根目录执行 `build_win.bat`，它会自动结束残留进程、清理并重建 `desktop\windows\build-x64`：

```bat
build_win.bat
```

如果 `build_win.bat` 因网络问题无法下载 WinSparkle，手动指定本地缓存后重试：

```powershell
$env:VOICESTICK_WINSPARKLE_URL = "file:///C:/path/to/WinSparkle-0.9.2.zip"
.\build_win.bat
```

## 分步构建（手动 / 调试用）

当 `build_win.bat` 整体不可用时，分步执行。以下命令均在 VS 2022 x64 开发者环境下运行：

```powershell
# 方案 A：通过 vcvars64.bat 设置环境（推荐，含完整 SDK 路径）
cmd /c '@echo off && call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja && cmake --build desktop\windows\build-x64'
```

```powershell
# 方案 B：如果 WinSparkle 下载失败（网络不可达），使用本地缓存
# 先从 https://github.com/vslavik/winsparkle/releases/download/v0.9.2/WinSparkle-0.9.2.zip 手动下载
# 然后解压并存放在本地目录
cmd /c '@echo off && call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja -DFETCHCONTENT_SOURCE_DIR_WINSPARKLE=C:\path\to\WinSparkle-0.9.2 && cmake --build desktop\windows\build-x64'
```

### 构建产物

| 产物 | 路径 | 说明 |
|---|---|---|
| `VoiceStick.exe` | `desktop\windows\build-x64\VoiceStick.exe` | 主程序 |
| `VoiceStickCtl.exe` | `desktop\windows\build-x64\VoiceStickCtl.exe` | CLI OTA 工具 |
| `voicestick_windows_tests.exe` | `desktop\windows\build-x64\voicestick_windows_tests.exe` | 测试可执行文件 |

## 运行测试

```powershell
# 全部测试
ctest --test-dir desktop\windows\build-x64 --output-on-failure

# 按名称过滤（正则）
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
```

`voicestick_windows_tests` 是基于 `assert` 的单测可执行文件。新增核心测试时把 `Test...()` 函数加入 `desktop/windows/tests/core_tests.cc` 的 `main()`。

## 运行应用

```powershell
desktop\windows\build-x64\VoiceStick.exe
```

## WinSparkle 下载失败处理

当 CMake configure 时报错 `Build step for winsparkle failed` / `Each download failed!` / `status_code: 56`，说明 GitHub 不可达。两种解法：

1. **指定 FETCHCONTENT_SOURCE_DIR**（推荐）：手动下载并解压 WinSparkle，然后加 `-DFETCHCONTENT_SOURCE_DIR_WINSPARKLE=<解压目录>`
2. **设置环境变量指向本地 zip**：`$env:VOICESTICK_WINSPARKLE_URL = "file:///..."`（部分 CMake/curl 组合不支持 `file://`）

## 常见构建陷阱

| 现象 | 原因 | 处理 |
|---|---|---|
| `rc.exe` / `mt.exe` not found | 未进入完整 VS 开发者环境 | 调用 `vcvars64.bat` 或从 VS Developer Command Prompt 运行 |
| CMake configure 失败 | 未安装 CMake / Ninja | 通过 Visual Studio Installer 安装 CMake 组件 |
| 链接 LNK2019 无法解析 WinSparkle 符号 | WinSparkle 未下载 | 走本地缓存方案 |
| `.gitignore` 静默忽略源文件 | `desktop/windows/` 整体被 ignore | 提交时必须 `git add -f` |
| 旧 `build` 目录残留 | 混入错误 VS/SDK 缓存 | 统一使用 `build-x64`，删除旧 `build` 目录 |
