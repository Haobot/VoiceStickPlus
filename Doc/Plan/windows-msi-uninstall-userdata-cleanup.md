# Windows MSI 卸载时清理用户本地数据

## 背景与根因

用户卸载 VoiceStick MSI 后重装，发现配对设备信息（`paired_device_ids`、`paired_devices`）仍存在。根因有二：

1. **MSI 包内置了带配对设备的 `config.template.toml`**：`build-msi.bat` 在 `VOICESTICK_CONFIG_TEMPLATE` 环境变量设置时，把一份真实 `config.toml` 复制成 `config.template.toml` 打进 MSI。程序首启（`AppConfig::Load`，`app_config.cc:542`）非便携模式下把该模板种子到 `%APPDATA%\VoiceStick\config.toml`（不覆盖已有）。重装即重新种子，配对设备回来。
2. **MSI 卸载不删用户数据**：WiX 只卸载 File 表记录的文件（`Program Files\VoiceStick\` 下的 exe/dll/template + 快捷方式），运行时生成的 `%APPDATA%\VoiceStick\`、`%LOCALAPPDATA%\VoiceStick\` 和 `HKCU` 注册表项全部残留。

本方案解决问题 2：在卸载流程加一个勾选框，让用户选择一并删除所有本地用户数据。

## 目标

- MSI 卸载对话框新增「删除用户数据」勾选项，**默认勾选**。
- 勾选时删除当前用户的：`%APPDATA%\VoiceStick\`、`%LOCALAPPDATA%\VoiceStick\`、`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\VoiceStick`（开机自启）、`HKCU\Software\TenClass\VoiceStick`（快捷方式 keypath 残留）。
- 交互式卸载与静默卸载（`msiexec /x /quiet`，Property 保持默认值）均生效。
- 清理失败不阻断卸载（`Return="ignore"`）。

## 技术决策

### CA 执行载体：WixQuietExec64 + PowerShell

- 用 WiX util 扩展内置的 `WixQuietExec64`（`BinaryRef="Wix4UtilCA_X64"`，与项目已用的 `WixShellExec`/`Wix4UtilCA_X86` 同源，64 位变体）执行一条 PowerShell 命令。
- 命令经本地验证（见下），幂等且退出码恒为 0。

### 执行模式：deferred + Impersonate="yes"

perMachine MSI 的 `InstallExecuteSequence` 由 MSI 服务（LocalSystem）执行。immediate CA 的 HKCU/%APPDATA% 可能指向 SYSTEM 而非交互用户。**必须用 deferred + `Impersonate="yes"`** 显式模拟原用户，才能访问其 `%APPDATA%` 与 `HKCU`（FireGiant 文档确认）。

deferred CA 无法直接读普通 Property，需用 `SetProperty` 模式：一个 immediate CA 设置名为 deferred CA Id 的 Property，MSI 自动将其值作为 `CustomActionData` 传给 deferred CA，`WixQuietExec64` 读 `CustomActionData` 作为命令行。

### 默认勾选

`<Property Id="REMOVE_USER_DATA" Value="1"/>` 设初始值 1。CheckBox 默认勾选；静默卸载无 UI，Property 保持 1，同样删除。

## 已验证的 PowerShell 命令

经本地真机验证（测试数据 + 二次运行幂等性）：

```
"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command "Remove-Item -LiteralPath 'C:\Users\<user>\AppData\Roaming\VoiceStick','C:\Users\<user>\AppData\Local\VoiceStick' -Recurse -Force -ErrorAction SilentlyContinue; Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name VoiceStick -ErrorAction SilentlyContinue; Remove-Item 'HKCU:\Software\TenClass\VoiceStick' -Recurse -ErrorAction SilentlyContinue; exit 0"
```

验证结论：
- 有数据时：三项目标全删，退出码 0。
- 无数据时（二次运行）：`-ErrorAction SilentlyContinue` 抑制错误，但退出码仍为 1 → **末尾加 `; exit 0` 强制为 0**，二次运行退出码 0。
- WiX 中用 `[AppDataFolder]`、`[LocalAppDataFolder]`、`[System64Folder]` 三个内置 Property 替换硬编码路径，MSI 在 CA 执行前格式化替换。

## 具体改动

### 文件 1：`desktop/windows/installer/VoiceStick.wxs`

在 `<Package>` 内、`<Feature>` 之前新增以下片段。

**(1) 默认勾选 Property**：
```xml
<Property Id="REMOVE_USER_DATA" Value="1" />
```

**(2) 自定义动作**：
```xml
<!-- deferred: 模拟原用户执行 PowerShell 删除命令 -->
<CustomAction Id="RemoveUserDataExec"
              BinaryRef="Wix4UtilCA_X64"
              DllEntry="WixQuietExec64"
              Execute="deferred"
              Impersonate="yes"
              Return="ignore" />

<!-- immediate: 设置命令行，MSI 将其作为 CustomActionData 传给上面的 deferred CA -->
<CustomAction Id="RemoveUserDataExec.SetCmd"
              Property="RemoveUserDataExec"
              Value="&quot;[System64Folder]WindowsPowerShell\v1.0\powershell.exe&quot; -NoProfile -ExecutionPolicy Bypass -Command &quot;Remove-Item -LiteralPath '[AppDataFolder]VoiceStick','[LocalAppDataFolder]VoiceStick' -Recurse -Force -ErrorAction SilentlyContinue; Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name VoiceStick -ErrorAction SilentlyContinue; Remove-Item 'HKCU:\Software\TenClass\VoiceStick' -Recurse -ErrorAction SilentlyContinue; exit 0&quot;" />
```

**(3) InstallExecuteSequence**（新增元素，只列两个 Custom，标准 action 保持默认）：

注意 deferred CA 必须落在 `InstallInitialize`(1500) 与 `InstallFinalize`(6600) 之间才会执行；若 `After="RemoveUserDataExec.SetCmd"` 会紧跟 SetCmd 落在 1403（< 1500），deferred CA 不执行。故 deferred CA 用 `After="RemoveFiles"`(3500)，先卸载程序文件再清理用户数据。

```xml
<InstallExecuteSequence>
  <Custom Action="RemoveUserDataExec.SetCmd"
          Condition='REMOVE_USER_DATA=1 AND Installed AND REMOVE~="ALL"'
          After="InstallValidate" />
  <Custom Action="RemoveUserDataExec"
          Condition='REMOVE_USER_DATA=1 AND Installed AND REMOVE~="ALL"'
          After="RemoveFiles" />
</InstallExecuteSequence>
```

实测 Sequence：SetCmd=1402、RemoveUserDataExec=3501，均在正确区间。

**(4) UI：自定义卸载确认对话框 + 流程覆盖**：
```xml
<UI>
  <!-- 卸载确认对话框：CheckBox 勾选是否删除本地数据 -->
  <Dialog Id="RemoveUserDataDlg" Width="370" Height="270" Title="!(loc.RemoveUserDataTitle)">
    <Control Id="BannerBitmap" Type="Bitmap" X="0" Y="0" Width="370" Height="44" TabSkip="no" Text="WixUI_Bmp_Banner" />
    <Control Id="Title" Type="Text" X="15" Y="6" Width="340" Height="24" Transparent="yes" NoPrefix="yes" Text="!(loc.RemoveUserDataTitle)" />
    <Control Id="Description" Type="Text" X="25" Y="40" Width="320" Height="40" Transparent="yes" NoPrefix="yes" Text="!(loc.RemoveUserDataDescription)" />
    <Control Id="RemoveUserDataCheckbox" Type="CheckBox" X="25" Y="95" Width="320" Height="20" Property="REMOVE_USER_DATA" CheckBoxValue="1" Text="!(loc.RemoveUserDataCheckbox)" />
    <Control Id="ItemsList" Type="Text" X="35" Y="125" Width="310" Height="90" Transparent="yes" NoPrefix="yes" Text="!(loc.RemoveUserDataItems)" />
    <Control Id="Back" Type="PushButton" X="180" Y="243" Width="56" Height="17" Text="!(loc.WixUIBack)">
      <Publish Event="NewDialog" Value="MaintenanceTypeDlg" />
    </Control>
    <Control Id="Next" Type="PushButton" X="236" Y="243" Width="56" Height="17" Default="yes" Text="!(loc.WixUINext)">
      <Publish Event="NewDialog" Value="VerifyReadyDlg" />
    </Control>
    <Control Id="Cancel" Type="PushButton" X="304" Y="243" Width="56" Height="17" Cancel="yes" Text="!(loc.WixUICancel)">
      <Publish Event="SpawnDialog" Value="CancelDlg" />
    </Control>
  </Dialog>

  <!-- 覆盖内置 MaintenanceTypeDlg.RemoveButton -> VerifyReadyDlg，改为先进入 RemoveUserDataDlg -->
  <Publish Dialog="MaintenanceTypeDlg" Control="RemoveButton" Event="NewDialog" Value="RemoveUserDataDlg" Condition="1" Order="99" />
  <!-- 卸载路径下 VerifyReadyDlg.Back 返回 RemoveUserDataDlg 而非 MaintenanceTypeDlg -->
  <Publish Dialog="VerifyReadyDlg" Control="Back" Event="NewDialog" Value="RemoveUserDataDlg" Condition='WixUI_InstallMode="Remove"' Order="99" />
</UI>
```

说明：
- `WixUI_Bmp_Banner`、`!(loc.WixUIBack/Next/Cancel)`、`CancelDlg` 均由 `WixUI_InstallDir` 对话框集提供，可直接引用。
- `Order="99"` 确保覆盖内置同控件同事件的 Publish（内置通常 Order=1，WiX 取最后求值为真者生效）。

### 文件 2：`desktop/windows/installer/zh-CN.wxl`

追加本地化字符串：
```xml
<String Id="RemoveUserDataTitle" Value="删除用户数据" />
<String Id="RemoveUserDataDescription" Value="正在卸载 VoiceStick。是否一并删除您的本地用户数据？" />
<String Id="RemoveUserDataCheckbox" Value="删除所有本地用户数据（配置、配对设备、日志、调试音频）" />
<String Id="RemoveUserDataItems" Value="将删除：&#13;&#10;• 配置文件与配对设备信息（%APPDATA%\VoiceStick\）&#13;&#10;• 日志与调试音频（%LOCALAPPDATA%\VoiceStick\）&#13;&#10;• 开机自启注册表项&#13;&#10;API 密钥等凭据随配置一并删除，不可恢复。" />
```

## 构建与验证

### 构建

```bat
scripts\build-msi.bat
```

若 `Wix4UtilCA_X64` BinaryRef 报未找到（与已用的 `Wix4UtilCA_X86` 对称推断，预期存在），回退方案：改用 `BinaryRef="Wix4UtilCA_X86" DllEntry="WixQuietExec"`（32 位 CA 亦能 CreateProcess 启动 64 位 powershell.exe）。构建报错立即修正，不静默替换。

### 真机验证（必须，单测无法覆盖 MSI 卸载行为）

1. 安装带真实配置的 MSI（配对设备 53A8,580C），首启确认 `%APPDATA%\VoiceStick\config.toml` 生成。
2. 从「设置-应用」卸载，卸载对话框出现「删除用户数据」勾选框，**默认勾选**。
3. 保持勾选，点卸载完成。
4. 验证残留：
   - `Test-Path "$env:APPDATA\VoiceStick"` → False
   - `Test-Path "$env:LOCALAPPDATA\VoiceStick"` → False
   - `Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name VoiceStick` → 不存在
   - `Test-Path 'HKCU:\Software\TenClass\VoiceStick'` → False
5. 取消勾选卸载一次，验证数据保留（用户可选择保留）。
6. 静默卸载 `msiexec /x {ProductCode} /quiet`，验证默认勾选生效、数据被删。

## 已知限制

- **静默卸载无交互用户时**：deferred impersonate 模拟可能失败，`[AppDataFolder]` 可能指向 SYSTEM profile 而非原用户，导致删错（删 SYSTEM 的空目录，原用户数据未删）。`Return="ignore"` 保证不阻断卸载。交互式卸载（主流场景）不受影响。此限制可接受，后续若需彻底解决需改用 C++ CA DLL（`SHGetKnownFolderPath` + deferred impersonate）。
- **多用户**：perMachine 安装为多用户共享，本方案只删当前卸载用户的 `%APPDATA%`/`HKCU`，不删其他用户的。符合「卸载我的程序删我的数据」语义。

## 测试策略

MSI/WiX 无单元测试框架，按 `CLAUDE.md` 测试策略，验证方式为「构建通过 + 真机卸载测试」。PowerShell 删除命令的语法与幂等性已本地验证（见上）。UI 流程与 CA 时序在真机卸载时端到端验证。
