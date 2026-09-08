# Windows 桌面端底层平台选型评估：维持 Win32，迁移则 .NET WPF 是唯一现实方向

- 日期：2026-09-08
- 评估对象：`desktop/windows/`（C++20 + Win32 + C++/WinRT + Direct2D，CMake + Ninja + MSVC 2022 x64）
- 候选平台：Win32（现状）、.NET（C# + WPF）、.NET MAUI、Electron、PWA
- 相关文档：`Doc/Agent/desktop-architecture.md`、`Doc/Ref/protocol.md`、`Doc/Ref/desktop-config.md`、`Doc/Expe/claude-memory-distilled.md`
- 结论时点：分支 `feat/local-asr` @ `7b2cd333`（纯评估，无代码改动）
- 时效性声明：能力清单中的实现描述为记录时点结论，引用前以当前源码为准。

## 一、结论

排序：**Win32（维持现状）≥ .NET WPF > .NET MAUI > Electron > PWA**

- **不换平台是技术最优解**。本程序的核心价值几乎全部押在 Windows 系统级集成上（见 §二），Win32/C++ 对这些能力全部是一等公民，且最难的坑（LL 钩子与 Raw Input 投递时序、剪贴板 owner 语义、C++/WinRT 参数陷阱等）已在真机上踩平并沉淀于代码与 `Doc/Expe/`。换平台的本质是把这笔已验证成本重新付一遍。
- **若因维护人力/迭代效率确需迁移，唯一现实的替代是 .NET（C# + WPF）**——它是唯一能完整覆盖能力清单的替代路径（P/Invoke + CsWinRT + sherpa-onnx 官方 C# 绑定）。
- **Electron、.NET MAUI、PWA 对本项目是负资产或不可行**（判定见 §三）。

## 二、评估依据：平台绑定能力清单

VoiceStick Windows 端不是普通桌面应用，选型评估的本质是下表覆盖率 + 已验证经验的沉没成本：

| 能力 | 现实现 |
|---|---|
| 全局热键录入 | 低级键盘钩子（LL Hook）+ 四态按键分类（ClassifyKey 纯函数） |
| 文本/按键注入 | `SendInput`（含 scan code 精细控制） |
| 剪贴板完整格式 vault/恢复 | `CF_DIB` 等多格式保存回放（vault 句柄跳过 ≠ 图像丢失） |
| 小米遥控器识别 | Raw Input 按接口路径 VID/PID 归属设备 |
| BLE（自研 GATT + Google ATVV） | C++/WinRT 调 WinRT 蓝牙栈 |
| 本机麦克风 + 虚拟麦克风渲染 | WASAPI 采集 + Opus 解码后渲染到虚拟设备端点 |
| 本地 ASR | sherpa-onnx 原生库（C/C++ 依赖） |
| 常驻托盘 + 自动更新 | Shell 托盘 + WinSparkle + MSI |

## 三、候选平台逐项判定

### Win32（现状）

上表全部是一等公民。代价只有开发效率与 UI 手写（Direct2D 自绘），而本应用 UI 面积小，UI 痛点本就偏低。

### .NET（C# + WPF，推荐 WPF 而非 WinUI 3）

- 钩子/`SendInput`/剪贴板/Raw Input 走 P/Invoke，成熟模式（注意点：`SetWindowsHookEx` 回调需钉住、钩子须在消息泵线程）。
- BLE 从 C# 调 WinRT 投影，文档与示例远多于 C++/WinRT。
- sherpa-onnx 有官方 C# 绑定；WebSocket（`System.Net.WebSockets`）、TOML/JSON 解析等 NuGet 即用。
- 排第二不是因为它比 Win32 更强，而是它是唯一"换得起"的方向——但迁移本身不划算（见 §五）。

### .NET MAUI

能力路径与 .NET 相同，但卖点（跨平台）对本项目是负资产：macOS 端已是成熟的独立 Swift/AppKit 实现，两端共享点在协议层（`Doc/Ref/protocol.md`）而非 UI 框架；ATVV/BLE 又深度绑定各 OS 蓝牙栈，跨平台 UI 统一是伪需求。叠加 WinUI 3 桌面成熟度与部署问题，相比 WPF 更重、更不稳，无增量收益。

### Electron

只能当 UI 壳：Node 主进程无 BLE API，LL 钩子、`SendInput`、剪贴板全格式、Raw Input、WASAPI、sherpa-onnx 全部要自写 N-API 原生模块——核心复杂度不降反升（Chromium + Node + C++ 三层、ABI 维护、node-gyp），且常驻后台工具难以接受其内存占用。负资产。

### PWA

直接排除：Windows 上 Chrome/Edge 不支持 Web Bluetooth；全局热键、键盘注入、任意格式剪贴板、Raw Input、虚拟音频、常驻托盘在 Web 沙箱内全部不可能。能力表覆盖度接近零。

## 四、WPF 相比 Win32 的优势详解（迁移动机清单）

优势不在运行能力（两者调到同一批系统 API），集中在三点：UI 产出效率、代码安全面、生态与迭代速度。

### 1. UI 开发效率（差距最大）

现设置界面（热键录入、模型目录浏览、有效性回显、热更）为 Direct2D 手绘：坐标、命中测试、焦点顺序、DPI 缩放全部自算。WPF 变为声明式 XAML + 布局容器自动排布：

```xml
<StackPanel Margin="16">
    <TextBlock Text="模型目录" />
    <TextBox Text="{Binding ModelDir, Mode=TwoWay}" />
    <Button Content="浏览..." Command="{Binding BrowseCommand}" />
    <TextBlock Text="{Binding ModelStatus}" Foreground="{Binding StatusBrush}" />
</StackPanel>
```

- **数据绑定（MVVM）**：状态变更界面自动更新，无需手写"改状态 → 重绘区域"胶水。
- **本地化/文案长度变化自适应**：布局自动重排，手绘坐标需逐个调整（对照 `windows-settings-label-width.md`、`windows-settings-dynamic-height.md` 这类调布局的工作）。
- **控件库现成**：TextBox/ComboBox/DataGrid/弹出层为成品；Direct2D 路线等于自建控件库。
- **XAML 热重载**：改 UI 不重启进程，真机调试循环从"编译→重启→重连 BLE→复现"缩短到秒级。

### 2. 代码安全面

- 托管内存消除整类 bug：use-after-free、缓冲区溢出、悬垂指针（注意：`IntPtr` 互操作边界仍需谨慎，但业务代码侧内存错误整类消失）。
- 字符串统一：协议侧 UTF-8、Win32 API 侧 UTF-16，C++ 里到处转换；C# 的 `string` 全程 UTF-16，仅协议边界转一次码。
- 异常模型对比 `HRESULT` 检查 + 错误码传播，错误路径不易被静默吞掉。

### 3. WinRT/BLE 互操作：C# 才是一等公民

Windows 官方文档与社区示例对 WinRT API 的第一语言是 C#：

- `FindAllAsync` 附加属性只收右值 vector、地址属性双格式兼容等坑，C# 侧要么不存在要么有现成答案。
- 投影头链路整体消失：`winrt/base.h` C1083 需 `cppwinrt.exe` 生成投影头并 prepend `INCLUDE` 的问题不复存在。
- async/await 原生对应 WinRT 异步，比 C++ 协程 + `IAsyncOperation` 顺畅。

### 4. 生态与工具链

- NuGet：NAudio、日志、MVVM Toolkit 等装包即用；sherpa-onnx 官方 C# 绑定。
- 单元测试：xUnit + mock 生态配合 TDD 红绿循环，比 gtest + ctest 写得快、跑得快；增量编译快（C++/WinRT 头文件编译慢）也是实质因素。
- 可维护性/交接：会 C# WPF 的人远多于会 Win32 + C++/WinRT + Direct2D 组合的人。

## 五、WPF 不覆盖的方面与迁移成本

- **运行时成本**：常驻内存从几十 MB 涨到约 100MB 上下；self-contained 分发增加几十 MB 安装体积（或依赖框架预装）。
- **系统集成的坑不会消失**：LL 钩子与 Raw Input 的投递时序、UIPI 提权、剪贴板 owner 语义是 OS 行为，换语言一样要踩，而经验全在现有 C++ 代码里。
- **性能无差别**：热路径本来就在原生库（Opus、sherpa-onnx）。
- **迁移是重写级**：协调器状态机（`voice_stick_coordinator.cc`）与钩子/BLE/音频管线深度交织，没有干净切分线。

## 六、若未来迁移：渐进路径

不建议整体迁移。可先行的一步（对当下测试性也有收益）：

1. 以协调器状态机为边界，把协议解析、状态转换、按键分类（ClassifyKey）等纯逻辑抽成可移植独立库（协议契约本以 `Doc/Ref/protocol.md` 为准），使 C++ 与 C# 都能消费。
2. UI、钩子、BLE、音频等不可移植层留在原地。
3. 满足以下任一条件时再评估逐层替换：
   - UI 需求持续膨胀（完整设置中心、统计面板、多语言界面）；
   - 维护人力成为瓶颈，C++ 迭代速度无法满足需求。

## 七、遗留/观察项

- 本评估未做 PoC 验证（属决策分析，非实验结论）；若启动渐进路径第 1 步，纯逻辑库的测试覆盖迁移是首个验证点。
- macOS 端不在本评估范围；跨平台统一 UI 的选项（MAUI/Electron）已被本项目双端原生实现 + 协议共享的架构事实否决。
