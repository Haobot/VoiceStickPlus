# Windows 端 COM 口固件烧录小工具（VoiceStickFlash）开发需求书

> 文档定位：给另一位 AI 开发者/工程师的实施需求书（开发契约）。目标读者不熟悉本项目，本文自包含：先讲清现状与约束，再给出可执行的设计、构建、测试与验收标准。
> 状态：**已实施**（v1 全部范围落地，单测全绿，MSI 构建验证通过）。`Doc/Ref/release.md`、`CHANGELOG.md`、`CLAUDE.md`/`AGENTS.md`/`CODEBUDDY.md` 已同步。
>
> 实施偏差（与本文 §5 的差异，以代码为准）：
> 1. 本机 `wix.exe` 实际为 **v4.0.5**（无 v5 引入的 `<Files>` 收割元素），`flash_payload` 改由 `prepare_flash_payload.ps1` 扫描生成 `flash_payload.wxs` 片段（逐文件 Component/File，ID 取相对路径 SHA1 前 32 位保证跨构建稳定），主 wxs 用 `<ComponentGroupRef Id="FlashPayloadGroup" />` 引用，`build-msi.bat`/`build-msi-unsigned.bat` 把片段作为第二源文件传给 wix。
> 2. pip 索引默认官方 PyPI（本机配置的清华镜像实测找不到 esptool 包），可用 `VOICESTICK_PIP_INDEX_URL` 覆盖。
> 3. `prepare_flash_payload.ps1` 含中文注释，须保持 UTF-8 with BOM（PS 5.1 按 GBK 误读无 BOM UTF-8 会搅乱解析）。
> 4. 开发构建下工具回退查找 `exe 同级\flash_payload\python\python.exe`（MSI 布局为 `FlashTool\python\python.exe`）。

## 1. 背景与目标

固件升级主路径是 BLE OTA（桌面端托盘"更新固件" / 本地文件 OTA）。但 BLE OTA 只写 **app 分区**（`ota_0`/`ota_1` 二选一，写对端槽后重启切换），存在它无法覆盖的场景：

- **救砖**：app 刷坏 / 双 OTA 槽都不可用时，设备不跑固件，无法走 BLE OTA；
- **分区表变更**：`partitions_ota.csv` 布局改动（当前布局见 §3.5）；
- **Bootloader 更新**；
- **完整擦除恢复出厂**（`erase_flash`）。

现有 COM 烧录手段是 `scripts/idf_cli.py -u`，但它是**开发者工具**：依赖本机装 ESP-IDF 环境、探测 `C:\Espressif`、`idf.py flash` 全链路，普通用户/内测者没有这套环境。

**目标**：在 Windows 桌面端整合一个独立的小 GUI 工具 `VoiceStickFlash.exe`，打包进 MSI，用户插上 USB 即可通过 COM 口整包烧录固件（merged bin），作为 BLE OTA 之外的用户级兜底链路。**BLE OTA 仍是主路径，本工具只作回退手段。**

## 2. 范围

### 做（v1）
1. 独立 Win32 GUI exe `VoiceStickFlash.exe`（与 VoiceStickApp 同属一个 MSI）。
2. COM 口自动枚举 + 按项目评分规则自动选中（ESP32-S3 原生 USB 优先）。
3. 三种烧录模式：
   - 整包烧录（merged bin @ `0x0`，推荐，覆盖 bootloader + 分区表 + otadata + app）；
   - 仅应用分区（app bin @ `0x10000`）；
   - 先完全擦除再整包烧录（恢复出厂，`erase_flash` 后整包）。
4. 进度条 + 日志窗（解析 esptool 输出）；开始/取消；成功/失败提示（含"手动短按电源键重启"引导）。
5. 入口：托盘菜单"固件烧录工具…" + 固件更新对话框"高级… COM 口烧录"按钮。
6. 打包：`build-msi.bat` 准备并打进 MSI；内置自包含 Python + esptool 运行时（不依赖系统 Python）。

### 不做（v1）
- 不联网下载固件（固件文件仅本地选择；下载 merged 为 P2）。
- 不做固件版本/SHA256 联网校验（本地文件直接烧，esptool 自带写前校验）。
- 不做 BLE OTA 功能、不做串口监控、不做双端并发协调。
- 不实现成 macOS 端。

## 3. 现状与硬约束（实现前必须知道）

### 3.1 本板烧录行为（来自项目经验，勿违反）
- StickS3 的 USB-C 直连 ESP32-S3 原生 USB（USB-Serial-JTAG，VID `303A`），**没有** CH340/CP2102 桥。Win10/11 免驱动。
- **进 Boot 可自动**：设备运行时 esptool 默认复位序列（USB JTAG 软复位）可自动进下载模式，无需长按。
- **出 Boot 必须手动短按电源键重启**：`esptool --after hard_reset` 在本板**无效**，不能用。烧完用 `--after no_reset`，并在 UI 里明确提示用户手动重启。
- 烧录会强制复位设备 → 若 VoiceStickApp 正连着该设备，BLE 连接会断开，可能触发 Windows 蓝牙 watcher 静默失效（项目已知问题）。**烧录前必须检测 VoiceStickApp 是否在运行并警告用户先退出**（复用 `idf_cli.py` 的 `is_voicestick_running` 守卫逻辑）。

### 3.2 esptool 可用性（已实证）
- 本机已装 `esptool 5.2.0` + `pyserial 3.5`；`import esptool; esptool.main(...)` 库入口存在。
- 但**发布版不能假设用户装了 esptool/python** → 必须随工具自带运行时（见 §5.3）。
- release 流水线已产出 merged bin（`esptool.py --chip esp32s3 merge_bin -o <merged>.bin @flash_args`），可直接作为整包烧录输入；本地开发时可用 `firmware/build/` 下的 bootloader/partition-table/voice_stick.bin 或现场生成 merged。

### 3.3 分区表（`firmware/partitions_ota.csv`）
```
nvs,      data, nvs,     0x9000,   0x4000,
otadata,  data, ota,     0xd000,   0x2000,
phy_init, data, phy,     0xf000,   0x1000,
ota_0,    app,  ota_0,   0x10000,  0x300000,
ota_1,    app,  ota_1,   0x310000, 0x300000,
storage,  data, spiffs,  0x610000, 0x1f0000,
```
- bootloader 在 `0x0`，分区表在 `0x8000`（默认位置，不在 csv 中列出）。
- merged bin 覆盖 `0x0` 起（bootloader+ptable+otadata+app）。esptool 只写镜像内含的地址段，因此**不会**触碰 `nvs`（0x9000）与 `storage`（0x610000，存 `power_log.bin`）。即：整包烧录不清除用户数据；真正清数据用"完全擦除"模式。

### 3.4 串口识别评分规则（与 `scripts/idf_cli.yaml` 保持一致）
- 描述关键字：`esp32 / usb serial / usb-serial / jtag / uart / cp210 / ch340 / ch343 / wch / ftdi`。
- 制造商关键字：`espressif / wch / silicon labs / ftdi`。
- HWID 关键字（VID:PID 前缀）：`303a:` / `1a86:` / `2bdf:` / `10c4:ea60` / `0403:6001`。
- 评分权重：desc +30、mfr +20、hwid +40、preferred_vid_pid +160；同分按 COM 名排序取最小。
- 烧录默认波特率 921600（可选 115200/460800/921600）。

### 3.5 项目工程约定（必须遵守）
- Windows 源码在 `desktop/windows/`，**整个目录被 `.gitignore` 忽略，提交必须 `git add -f`**。
- 可测试的核心逻辑放进 `voicestick_core` 库（测试在 `desktop/windows/tests/core_tests.cc`，基于 `assert`）；Win32 外壳只做 UI + 进程。
- 构建：`build_win.bat`（开发）+ `scripts/build-msi.bat`（MSI）。MSI 用 WiX v6（`wix.exe`），自定义动作走 `Wix4UtilCA_X64` + `WixQuietExec64`（deferred + Impersonate）。
- 代码风格：Google C++（snake_case 文件/变量、CapWords 类型、MixedCase 方法、4 空格缩进、C++20）。
- UI：Win32 对话框**用声明式布局表 + Relayout + WS_VSCROLL**（项目范式，见记忆 `windows-dialog-dynamic-layout-pattern`），不要手动累加 y 坐标。
- 中文注释/文案；manifest 为 asInvoker + DPI 感知（沿用 `VoiceStick.manifest` 模式）。

## 4. 总体架构

```
VoiceStickFlash.exe (Win32 GUI, C++20)
 ├─ voicestick_core（链接）  ← 可测试核心
 │    ├─ com_port_selector    串口枚举(SetupAPI) + 评分选中
 │    ├─ esptool_flash_command  esptool 命令行构造（纯字符串，无 I/O）
 │    ├─ esptool_progress      esptool stdout 解析 → 进度/阶段/错误事件
 │    └─ voice_stick_flash_tool 烧录编排：组装命令→拉起子进程→事件回调
 ├─ flash_tool_dialog         Win32 对话框（布局表/进度/日志/控件）
 ├─ flash_tool_main           WIN32 入口
 └─ payload/                  自包含运行时（python-embed + esptool）
```

**关键决策：esptool 以子进程方式运行**（`payload\python\python.exe -m esptool ...`），GUI 读其 stdout 解析进度。理由：跨版本稳定、崩溃隔离、易测试；避免 C++ 进程内嵌 Python 的复杂生命周期。子进程输出管道 + 后台线程读取 → `PostMessage` 回 UI 线程刷新。

## 5. 详细设计

### 5.1 目录与文件（新增/改动）

```
desktop/windows/src/
  com_port_selector.h/.cc        [core] 串口枚举+评分
  esptool_flash_command.h/.cc    [core] 命令构造
  esptool_progress.h/.cc         [core] 进度/阶段解析
  voice_stick_flash_tool.h/.cc   [core] 烧录编排（子进程 runner 可注入以便测试）
  flash_tool_dialog.h/.cc        [shell] 主对话框
  flash_tool_main.cc             [shell] wWinMain
  flash_tool.rc                  [shell] 图标/资源
  resources/flash_tool.manifest  [shell] asInvoker + DPI
desktop/windows/tests/core_tests.cc  追加 4 组单测（见 §7）
desktop/windows/CMakeLists.txt       新增 target VoiceStickFlash（链接 voicestick_core）
desktop/windows/installer/VoiceStick.wxs  新增 FlashTool 目录组件 + 托盘/固件更新入口不在此
desktop/windows/src/win32_app.cc/.h 托盘菜单加"固件烧录工具…"（ShellExecuteW 拉起 exe）
desktop/windows/src/firmware_update_dialog.cc/.h  加"高级… COM 口烧录"按钮
scripts/prepare_flash_payload.ps1    [新增] 下载 python-embed + pip 装 esptool 到 payload（幂等）
scripts/build-msi.bat                Step 3 前调用 prepare_flash_payload；WiX -d FlashPayloadDir
Doc/Plan/windows-com-flash-tool.md  本文
```

### 5.2 UI 规格（主对话框）

窗口标题：`VoiceStick 固件烧录工具`。尺寸约 480×560，对话框模板按声明式布局表实现，超高滚动。

顶部警告横幅（只读文本，黄底）：`烧录会覆盖设备固件。请先用 USB 线连接设备；若桌面端 VoiceStick 正在运行，建议先退出。`

| 区 | 控件（ID） | 说明 |
|---|---|---|
| 设备 | 串口下拉 `IDC_COMBO_PORT` + 刷新 `IDC_BTN_REFRESH` | 枚举全部 COM，按 §3.4 评分自动选中最优；无匹配时选第一个并标 `(自动猜测)` |
| 固件 | 路径编辑框 `IDC_EDIT_FW` + 浏览 `IDC_BTN_BROWSE` | `GetOpenFileNameW`，过滤 `*.bin`；选择后显示文件大小与 SHA256（只读标签 `IDC_LBL_FWINFO`） |
| 模式 | 单选 `IDC_RADIO_FULL`（整包，默认）/ `IDC_RADIO_APP`（仅应用）/ `IDC_RADIO_ERASE`（先擦除再整包） | 默认整包 |
| 高级 | 波特率下拉 `IDC_COMBO_BAUD` | 115200 / 460800 / **921600(默认)** |
| 操作 | 开始 `IDC_BTN_START`（默认）、取消 `IDC_BTN_CANCEL`、关闭 `IDC_BTN_CLOSE` | 运行中 Start 置灰、Cancel 可用 |
| 进度 | 进度条 `IDC_PROGRESS` + 状态文本 `IDC_LBL_STATUS` | 运行中显示阶段名 + 百分比 |
| 日志 | 多行只读编辑框 `IDC_EDIT_LOG`（可滚动） | 关键行（error/warning/阶段/成功）全量显示，普通行截断防刷屏 |

**交互流程**
1. 刷新/选择串口 → 浏览选择 bin → 选模式/波特率。
2. 点"开始烧录"：
   a. 校验：有串口、文件存在且后缀 `.bin`。
   b. **检测 VoiceStickApp 是否在运行**（`FindWindowW(L"VoiceStickWindow", NULL)`）。在运行 → MessageBox 警告"烧录将强制复位设备并断开当前蓝牙连接，建议先退出 VoiceStick。是否继续？"（是/否）。
   c. 若模式=整包或擦除：额外提示"整包烧录会覆盖 bootloader 与分区表；烧录完成后请手动短按电源键重启设备。"
   d. 启动子进程（§5.4），日志区打印 `esptool.py v5.2.0` 等首行。
3. 成功：状态"烧录完成"。MessageBox 提示：**"固件烧录完成。请手动短按设备电源键重启。重启后设备会自动恢复广播；若此前已配对，桌面端会重新连接。"** 提供"复制日志"按钮（把日志窗内容写剪贴板）。
4. 失败：状态"烧录失败"。日志区显示 esptool 错误（如 `Failed to connect`），MessageBox 给排查提示（见 §8）。
5. 取消：终止子进程，提示"已取消。若中断于写入中，设备可能停在下载模式，可再次烧录恢复。"（ROM bootloader 始终可进，安全）。

### 5.3 esptool 运行时打包（`scripts/prepare_flash_payload.ps1`）

目标：产出 `desktop/windows/build-msi-x64/flash_payload/`（gitignored，属构建产物），自包含、免系统 Python。

1. 下载 CPython **embeddable zip**（x64，3.12.x，如 `python-3.12.x-embed-amd64.zip`，官方 python.org）→ 解压到 `flash_payload/python/`。
2. 用本机 python 执行 `python -m pip install --target flash_payload/python/site-packages esptool==5.2.0 pyserial pyyaml`（pip 自动拉齐依赖，含 cryptography 二进制 wheel，随包携带即可）。
3. 改 `flash_payload/python/python312._pth`：
   ```
   python312.zip
   .
   Lib\site-packages
   import site
   ```
   （embeddable 默认不加载 site-packages，必须显式打开。）
4. 冒烟验证：`flash_payload\python\python.exe -m esptool version` 输出 `esptool.py v5.2.0`。
5. 幂等：payload 已存在且 `python.exe -m esptool version` 通过则跳过（加 `payload.version` 标记文件）。下载 URL 可用缓存镜像/本地 zip 覆盖（`VOICESTICK_PYTHON_EMBED_URL` 环境变量，仿 `VOICESTICK_WINSPARKLE_URL` 模式）。

`build-msi.bat` 在 WiX 构建前调用 `prepare_flash_payload.ps1`（失败即报错退出），并把 `-d FlashPayloadDir=%BUILD_DIR%\flash_payload` 传给 wix。

### 5.4 esptool 命令构造（`esptool_flash_command.h/.cc`）

核心类型（可单测）：
```cpp
enum class FlashMode { FullMerged, AppOnly, EraseThenFull };

struct FlashOptions {
    std::wstring serial_port;      // COM5
    std::wstring firmware_path;    // 绝对路径（含中文，宽字符）
    int baud = 921600;
    FlashMode mode = FlashMode::FullMerged;
};

// 返回 argv（宽字符），0 号固定为 -m esptool 之前的 python.exe 占位
std::vector<std::wstring> BuildEsptoolArgv(const FlashOptions& opts,
                                           const std::filesystem::path& python_exe);
```

生成的命令形态：
```
<python>\python.exe -m esptool
  --chip esp32s3
  --port COM5
  --baud 921600
  --before default_reset
  --after no_reset          // 关键：本板 hard_reset 无效，烧完提示手动重启
  write_flash
  [--flash_mode keep --flash_freq keep --flash_size keep]   // 保留既有 flash 设置
  <addr> "<fw>"
```
- FullMerged：`write_flash 0x0 <fw>`。
- AppOnly：`write_flash 0x10000 <fw>`。
- EraseThenFull：先 `erase_flash`（单独一次调用），成功后再次整包 write_flash。编排层负责两次子进程。
- 说明：esptool 只写镜像内含地址段，整包**不清除 nvs/storage**；要清数据必须用 EraseThenFull。

### 5.5 进度解析（`esptool_progress.h/.cc`）

复用 `idf_cli.py` 的 `parse_flash_progress` 思路：
- 阶段识别：`Detected chip type` / `Chip ID` → "检测芯片"；`Erasing flash` → "擦除"；`Writing at 0x…` → "写入"；`Hash of data verified` → "校验"；`Stub running` → "连接中"。
- 进度：正则 `\((\d+(?:\.\d+)?)\s*%\)`（排除含 `Eras/Verif/Hash/Compress/Check/CRC/Leaving/Reset/Connecting` 的行，避免进度条闪跳）。
- 错误：`A fatal error occurred:` / `Failed to connect` / `Invalid` 等 → 失败事件 + 原文入日志。
- 事件回调接口：
```cpp
struct FlashEvent { enum class Kind { Stage, Progress, LogLine, Error, Finished }; ... };
// 进度归一到 0-100（整包含多段写入时按"当前写入阶段/总段数+段内百分比"估算）
```

### 5.6 烧录编排（`voice_stick_flash_tool.h/.cc`）

```cpp
class FlashTool {
 public:
  struct Runner {              // 可注入，测试用 FakeRunner
    virtual int Run(const std::vector<std::wstring>& argv,
                    const FlashEvent::Handler& on_event) = 0;
    virtual void Cancel() = 0; // TerminateProcess
  };
  FlashTool(FlashOptions opts, const std::filesystem::path& python_exe,
            std::unique_ptr<Runner> runner);
  void Start();                // 校验→(EraseThenFull 先擦除)→写→完成
};
```
- 子进程创建：`CreateProcessW`，`STARTUPINFOW.dwFlags = STARTF_USESTDHANDLES`，stdout/stderr 合并到同一管道；后台线程 `ReadFile` → 逐行调 `on_event` → UI 线程 `PostMessage(WM_APP_FLASH_EVENT)`。
- 退出码：0=成功；非 0 读日志给失败事件。`Cancel()` 调 `TerminateProcess` 并返回被取消。
- 宽字符路径：全部 `std::wstring`/`CreateProcessW`，规避中文路径 ACP/GBK 坑（记忆 `filesystem-path-acp-utf8-pitfall`）。

### 5.7 入口整合

- **托盘**：`win32_app.cc` 托盘菜单新增"固件烧录工具…"（菜单 ID 新段 7000 起）。点击 → `ShellExecuteW(NULL, L"open", <exe同级>\VoiceStickFlash.exe, ...)`；exe 路径 = `GetModuleFileNameW` 取当前目录 + `VoiceStickFlash.exe`（开发构建在 build-x64，MSI 安装在 INSTALLFOLDER）。
- **固件更新对话框**：底部加"高级… COM 口烧录"小按钮 → 同上拉起。

### 5.8 WiX（`desktop/windows/installer/VoiceStick.wxs`）

新增组件（Feature Main 内）：
- `VoiceStickFlash` Component：`INSTALLFOLDER\VoiceStickFlash.exe`（`$(var.BuildDir)\VoiceStickFlash.exe`，KeyPath）。
- `FlashPayload` Component：`INSTALLFOLDER\FlashTool\` 目录下的 `flash_payload/` 全部文件（WiX v4 用 `<Files>` 收割目录，Source 为 `$(var.FlashPayloadDir)`；注意 python-embed 小文件多）。
- 可选：开始菜单/桌面快捷方式（与 VoiceStick 一致，Name="VoiceStick 固件烧录工具"）。

MSI 体积会增加约 20–25MB（python-embed ~10MB + esptool/deps ~10MB），属预期。

## 6. 构建与验证命令

```powershell
# 1) 核心库与工具编译（VS 2022 x64 开发者环境）
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64          # 产出 VoiceStickFlash.exe
# 2) 单元测试
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
# 3) 准备 payload 并冒烟
powershell -ExecutionPolicy Bypass -File scripts\prepare_flash_payload.ps1
desktop\windows\build-x64\flash_payload\python\python.exe -m esptool version   # 应输出 v5.2.0
# 4) MSI
scripts\build-msi.bat          # 内部会准备 payload 并打进 MSI
```

**注意**：`build_win.bat` 历史上曾链接失败仍报成功，构建后核对 `build-x64\VoiceStickFlash.exe` 时间戳与体积（记忆 `windows-build-fake-success`）。`desktop/windows` 提交用 `git add -f`。

## 7. 测试计划

### 7.1 单元测试（`core_tests.cc` 追加，基于 assert）
1. `TestComPortScoring`：给定模拟端口集合（含 303A、CH340、无关 USB 串口），评分排序正确；preferred_vid_pid 优先；无匹配兜底逻辑。
2. `TestEsptoolCommandBuilder`：三种模式生成正确的 argv（地址、`--after no_reset`、`--chip esp32s3`、波特率）；中文路径参数不被截断；EraseThenFull 返回两步命令。
3. `TestEsptoolProgressParser`：喂典型 esptool 输出片段 → 正确映射 阶段/百分比/错误；黑名单行不产生进度事件。
4. `TestFlashToolFlow`：FakeRunner 返回 0 → Finished(success)；返回非 0 → Finished(failure)；Cancel 后事件正确；EraseThenFull 先 erase 后 write 的顺序断言。

### 7.2 真机验收（需硬件，由人执行；开发 AI 交付后附操作清单）
1. `idf_cli.py -c` 编译固件，得到 `firmware/build/voice_stick.bin`；或直接用 release 的 merged bin。
2. 设备 USB 接本机，打开 VoiceStickFlash.exe：串口下拉应自动选中 `COMx`（303A）。
3. 选 merged bin → 整包烧录：进度条推进，日志出现 `Stub running`/`Writing at…`/`Hash of data verified`。
4. 成功后提示手动重启；短按电源键 → 设备重新广播 `VS-XXXX`；打开 VoiceStickApp 能重连。
5. 用"仅应用分区"烧 `voice_stick.bin`：同样成功且不影响 nvs/storage。
6. 用"完全擦除"模式：烧录后设备恢复出厂、重新配对。
7. 失败路径：拔掉设备点开始 → 报 `Failed to connect`，提示合理；VoiceStickApp 运行时点开始 → 出现退出警告。
8. `nvs` 数据保持验证（整包模式烧完，配对列表/上次成功串口不丢）。

## 8. 边界与异常（实现时逐条处理）

| 场景 | 处理 |
|---|---|
| 无串口 / 无匹配串口 | 下拉为空 + 状态提示；点开始禁用 |
| 文件不存在 / 非 .bin | 开始前校验，弹提示 |
| 端口被占用（如其他工具监控中） | esptool 报 `Access denied`/`Failed to connect`，提示"检查该 COM 口是否被其他程序占用" |
| 设备不在下载模式 / USB 线不良 | `Failed to connect ... No serial data received`，提示换线/换口/检查设备供电 |
| 烧录中设备拔掉 | 读管道 EOF + 退出码非 0 → 失败事件，提示重插后重试（ROM bootloader 可重进，安全） |
| 烧录中取消 | TerminateProcess；提示设备可能停在下载模式，可再次烧录恢复 |
| VoiceStickApp 正在运行 | 开始前警告并让用户确认（§5.2 步骤 2b） |
| 中文路径 | 全宽字符 API；esptool 子进程 argv 用 Unicode 传参 |
| 日志过长 | 日志框保留最近 N 行（如 1000），前面截断 |
| 重复点开始 | 运行中禁用开始按钮 |

## 9. 验收标准（可勾选）

- [ ] `cmake --build` 产出 `VoiceStickFlash.exe`，时间戳/体积正常。
- [ ] `ctest -R voicestick_windows_tests` 全绿（含新增 4 组）。
- [ ] `prepare_flash_payload.ps1` 幂等、冒烟 `esptool version` 通过；payload 不打进 git。
- [ ] `build-msi.bat` 产出含 FlashTool 的 MSI；安装后 `INSTALLFOLDER\VoiceStickFlash.exe` 与 `FlashTool\` payload 就位。
- [ ] 托盘菜单"固件烧录工具…"与固件更新对话框"高级…"均能拉起工具。
- [ ] 真机验收 §7.2 全部通过（由人执行，开发 AI 提供操作清单）。
- [ ] 文档同步：`Doc/Ref/release.md`（MSI 内容说明）、`CHANGELOG.md`（Unreleased 条目）、`CLAUDE.md`/`AGENTS.md`/`CODEBUDDY.md`（关键配置文件表 + 构建命令 + 提示节新增 FlashTool）。

## 10. 安全与合规

- 固件镜像不含凭据，工具无网络、不读写配置，无密钥泄露面。**不做**固件下载（避免供应链风险）；本地文件直烧，esptool 自带写前校验。
- 工具 manifest 为 asInvoker，不需提权（写串口无需管理员）。
- 不绕过任何签名/OTA 校验逻辑；本工具是独立的用户主动操作，与 BLE OTA 互不影响。

## 11. 参考（实现前建议快速过一遍）

- `scripts/idf_cli.py`：串口评分（`PortDetector`）、进度解析（`parse_flash_progress`）、`is_voicestick_running` 守卫、`dtr_rts_reset`。
- `scripts/idf_cli.yaml`：串口识别关键字与权重。
- `firmware/partitions_ota.csv`：分区布局（§3.3）。
- `desktop/windows/installer/VoiceStick.wxs`：现有组件与自定义动作模式（`RemoveUserDataExec` 作自定义动作参考）。
- `desktop/windows/src/firmware_update_dialog.cc`：对话框写法参考；`win32_app.cc` 托盘菜单结构。
- 记忆要点：`stick-s3-button-boot-control`（出 Boot 须手动重启）、`windows-build-fake-success`（核对 exe 时间戳）、`windows-dialog-dynamic-layout-pattern`（声明式布局表）、`filesystem-path-acp-utf8-pitfall`（中文路径全宽字符）。
