# 构建与测试命令（全平台细节）

本文承载各平台构建与测试的完整命令与注意事项，2026-08 由根目录 `AGENTS.md`/`CLAUDE.md` 的「构建与测试命令」「测试策略」章节迁入；根指南只保留各平台一行速查与指向本文的指针。

## 构建命令

### 固件（ESP-IDF v5.5.1，目标 `esp32s3`）

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

首次从旧单应用分区表升级时，需要擦除后重刷：

```sh
idf.py -p /dev/cu.usbmodemXXXX erase-flash flash monitor
```

Windows 上不便直接用 `idf.py` 时：

```bat
python scripts/idf_cli.py -cus -p COM17
```

`idf_cli.py` 常用参数：`-c` 编译、`-u` 上传、`-s` 串口监控、`-cus` 编译+上传+监控、`-p COMxx` 指定串口。固件没有自动化单元测试，验证方式为 `idf.py build` 编译通过和真机运行时测试。

### macOS 桌面端（SwiftPM）

```sh
cd desktop/macos
swift build
swift run VoiceStickApp
```

发布构建（在仓库根目录执行）：

```sh
SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
scripts/make-dmg.sh
```

macOS 端目前没有专用测试目标，无法运行单个测试；验证方式主要是 `swift build` 编译通过和运行时手动测试。

### Windows 桌面端（CMake + Ninja + MSVC 2022 x64）

推荐从仓库根目录使用：

```bat
build_win.bat
```

该脚本会自动查找 VS 2022、结束残留进程、删除并重建 `desktop\windows\build-x64`，只构建不运行 CTest。注意：`build_win.bat` 历史上曾出现链接失败仍报成功的情况，构建后应核对 `desktop\windows\build-x64\VoiceStick.exe` 的时间戳与体积。

手动构建（需先进入 VS 2022 x64 开发者环境）：

```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64
```

可选：configure 时传 `-D VOICESTICK_BUILTIN_API_KEY=...` 等 7 个 `VOICESTICK_BUILTIN_*` 变量把凭据编译进 exe（生成 `builtin_secrets.h`，见 `Doc/Agent/release-and-security.md`）。

运行全部 Windows 测试：

```powershell
ctest --test-dir desktop\windows\build-x64 --output-on-failure
```

按 CTest 名称正则过滤：

```powershell
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
```

`voicestick_windows_tests` 基于 `assert`，目前不支持按测试函数名过滤；新增核心测试时把 `Test...()` 函数加入 `desktop/windows/tests/core_tests.cc` 的 `main()`。`-R voicestick_integration_tests` 单独跑集成测试，需联网与 `volcengine_api_key`，无 key 时该测试返回 77 被 CTest 标记为 SKIP。

运行应用：

```powershell
desktop\windows\build-x64\VoiceStick.exe
```

发布打包（签名 MSI）：

```bat
scripts\build-msi.bat
```

该脚本在 WiX 构建前自动调用 `scripts\prepare_flash_payload.ps1` 准备 VoiceStickFlash 的自包含 esptool payload（`build-msi-x64\flash_payload\`，gitignored 构建产物），并随 MSI 安装到 `INSTALLFOLDER\FlashTool\`（exe 本体装到 `INSTALLFOLDER\VoiceStickFlash.exe`）。脚本还会用 `extract_builtin_key.ps1` 输出 7 项内置凭据环境变量供 cmake 注入，并调用 `generate_msi_config.ps1` 生成含 key 的 MSI config 产物。

注意：`build_native.bat`、`do_build.bat`、`desktop\windows\build.bat` 包含本机绝对路径或固定版本号，复用前必须先检查内容；根目录 `test.bat` 目前只是占位脚本，不运行 CTest。仓库根目录散落的 `*.log` 与 `%BUILD_LOG%` 等文件是历次本地构建的残留日志，不是源码。

### 网站（Vue 3 + Vite，Node 22）

```sh
cd website
npm install
npm run dev      # 本地开发服务器
npm run build    # 最小验证
npm run preview  # 预览生产构建
```

`website/package.json` 目前只定义了 `dev`、`build`、`preview`，没有 lint/test 脚本。修改网站后用 `npm run build` 作为最小验证。修改网站 UI 文案时，必须同步更新 `website/src/i18n/zh-CN.json` 和 `website/src/i18n/en-US.json`。

## 测试策略

- **Windows**：`desktop/windows/tests/core_tests.cc` 使用自定义 Fake/Mock 对 `voicestick_core` 中的状态机、配置解析、协议编解码、Ogg Opus mux 等进行单元测试（不联网）。`desktop/windows/tests/integration_tests.cc` 是 L1 ASR 链路集成测试，连真实火山 ASR，无 key 时返回 77 被 CTest 标记为 SKIP。运行命令：`ctest --test-dir desktop/windows/build-x64 --output-on-failure`。
- **macOS**：目前没有专用测试目标。验证方式主要是 `swift build` 编译通过和运行时手动测试。
- **固件**：没有自动化单元测试。验证方式是 `idf.py build` 编译通过和真机运行时测试。
- **网站**：没有自动化测试。验证方式是 `npm run build` 构建通过。
- **Python E2E 真机验证**：`scripts/e2e_test/` 是跨固件+Windows 端到端的半自动验证工具链（L0 语料、L3 固件回放、L4 微信输入法、ASR/热词离线评测、功耗记账导出），用真实 BLE 连接与真实 ASR/音频链路，不伪造结果。各工具用法与评测结论索引见 `Doc/Ref/e2e-test-toolchain.md`；依赖 `bleak` / `numpy` / `sounddevice`，**未列入根目录 `requirements.txt`**（该文件只含 `pyyaml` / `pyserial` / `Pillow`），运行前需另行 `pip install`；设计文档见 `Doc/Plan/windows-e2e-test-plan.md` 与 `Doc/Plan/windows-e2e-next-steps.md`。小米遥控器另有 ATVV 工具组：`atvv_capture.py`（真机 golden 采集）、`atvv_bench.py`（golden 会话离线 ASR 评测，裸 PCM 直送）、`atvv_probe.py`（会话延迟/尾包时延/长连接静置探针）；golden fixtures 接入 C++ 单测（`TestImaAdpcmDecoderGoldenFixtures` 扫描 `scripts/e2e_test/fixtures/xiaomi/**` 逐样本对拍，无 fixtures 打印 SKIP 不算失败）与集成测试回放，其中 `fixtures/xiaomi/demo_synthetic/` 入库作冒烟资产、真机采集目录 gitignore（详见 `Doc/Ref/e2e-test-toolchain.md`）。
