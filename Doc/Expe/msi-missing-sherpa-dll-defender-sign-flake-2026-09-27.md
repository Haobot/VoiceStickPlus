# v2.4.1 全新安装启动即报「找不到 sherpa-onnx-c-api.dll」——MSI 打包清单整链漏文件 + Defender 签名瞬态失败

- 日期：2026-09-27
- 相关文件：`desktop/windows/installer/VoiceStick.wxs`、`scripts/build-msi.bat`、`scripts/verify_msi_contents.ps1`（新增）、`desktop/windows/CMakeLists.txt`
- 相关 commit：`7cdf457e`（修复）、缺陷引入可追溯至 `ceb2550a`（本机 ASR 迭代一引入 sherpa 链接时 MSI 就没打这两个 DLL）

## 症状

用户全新机器安装 v2.4.1 Windows MSI 后，启动 VoiceStick.exe 弹系统 loader 对话框：

> 由于找不到 sherpa-onnx-c-api.dll，无法继续执行代码。重新安装程序可能会解决此问题。

重装无效（安装包里本来就没有）。

## 日志/取证判据

- 用 PowerShell COM（`WindowsInstaller.Installer`）枚举 MSI 的 `File` 表：**v2.4.0 与 v2.4.1 的 `sherpa`/`onnx` 命中数均为 0**——证明是既有缺陷而非 v2.4.1 新引入。注意 `File` 表的 `FileName` 列是「短名|长名」格式（如 `-0dulbsm.dll|VoiceStickHidTap.dll`），匹配时必须两段都查。
- PE 导入表（pefile 解析 `VoiceStick.exe`）：隐式导入的第三方 DLL 只有两个非系统项——`WinSparkle.dll`（已打包）与 `sherpa-onnx-c-api.dll`（未打包）；`onnxruntime.dll` 是前者的二级依赖，exe 导入表里看不到，但 loader 递归解析时同样必须找到。
- 判「这个 DLL 该不该进 MSI」的通用方法：对发布产物跑一遍导入表，凡非系统 DLL 必须与 WiX 组件清单对得上。

## 根因

**根因一（主）：打包清单手工维护，第三方运行时 DLL 整条链无人认领。**

`sherpa-onnx-c-api.dll`/`onnxruntime.dll` 由 CMake `file(COPY)`（configure 期）从 `third_party/sherpa-onnx/lib/` 复制到构建目录，开发机直接跑构建目录没问题；但 MSI 打包链的三道关口都没有它们：

1. `VoiceStick.wxs` 的 `Main` Feature 组件清单只有 exe/WinSparkle/config.template/HidTap/TapInject；
2. `build-msi.bat` 的存在性检查与签名步骤同样只覆盖上述文件；
3. `release.ps1` 的门禁只有「MSI 文件存在 + P0-4 凭据扫描」，不验证 MSI 内容。

v2.4.0 之所以没暴露：MSI 卸载（major upgrade 的 RemoveFiles）只删**组件登记**的文件，旧安装目录里手工散落的 DLL 会残留，掩盖了缺失。**判据：凡是「老机器正常、全新安装必现」的缺文件问题，先怀疑历史产物同样有缺、被环境残留掩盖，而不是「新版本引入」。**

**根因二（修复过程中暴露）：Defender 实时扫描导致 signtool 间歇性失败。**

重建 MSI 时签名阶段随机在某个文件上报：

```
Error information: "Error: SignerSign() failed." (-2146881278/0x80093102)   ← CRYPT_E_BAD_ENCODE
```

三次构建分别死在 `onnxruntime.dll`、`WinSparkle.dll`（此前明明签成功过）。二分定位：同内容副本改名后能签、原文件稳定失败、无 NTFS 备用数据流、无进程占用、PE 结构合法——**触发因素是文件对象而非内容或文件名**，即 Defender 实时扫描在 SignerSign 写证书表（追加到文件尾）时钩住文件的瞬态竞态。手动重签同一文件立即成功。

## 修复（commit 7cdf457e）

1. **`VoiceStick.wxs`**：新增 `SherpaOnnxCApiDll`/`OnnxRuntimeDll` 两组件（`Guid="*"`、`Bitness="always64"`）并入 `Main` Feature。
2. **`build-msi.bat`**：
   - 两个 DLL 纳入 Step 1 存在性检查；
   - 纳入 Step 2 签名（对齐 WinSparkle.dll 先例，保证包内所有二进制发布者一致）；
   - 7 处重复的「signtool sign + Get-AuthenticodeSignature 验证」抽成 `:SignAndVerify` 子过程，内含**最多 3 次尝试、间隔 2s 的重试**（`ping -n 3 127.0.0.1 >nul` 延时，不用 `timeout`——cmd /c 无 stdin 时 timeout 会报错），重试仍败才 fail；
   - Step 4b（新增）：对两个语言 MSI 各调一次内容校验，失败即构建失败。
3. **`scripts/verify_msi_contents.ps1`（新增）**：COM 枚举 MSI `File` 表，断言 8 个顶层必需文件（exe/WinSparkle/config.template/VoiceStickFlash/HidTap/TapInject/sherpa/onnxruntime）在位。**顶层新增打包文件时必须同步它的 `-RequiredFiles` 清单。**

## 验证

- TDD：校验器先对既有缺陷版 `VoiceStick_2.4.1_zh-CN.msi` 跑出红灯（精确报出两个缺失文件，退出码 1）；修复后重建绿灯（8/8 在位）。
- 独立复核：`msiexec /a <msi> /qn TARGETDIR=...` 管理安装解包，四个关键文件落盘且 `Get-AuthenticodeSignature` 全部 `Valid`（含 17.4MB 的 onnxruntime.dll）。
- 真机：待用户在全新安装环境复验（本报告验证止于解包与签名层，未做全新机器实装）。

## 长期技术记忆

- **MSI `File` 表取证**：PowerShell `WindowsInstaller.Installer` COM 可直接读任意 MSI 的表，不用真装；`FileName` 列是「短名|长名」双段格式。
- **msiexec `/a` 管理安装**是无副作用解包 MSI 的标准手法（不跑 InstallExecuteSequence 的 CA），适合验证包内容。
- **signtool 0x80093102 随机命中文件 = Defender 签名竞态**，重试即恢复；签名脚本必须有重试，否则构建链随机炸。
- bat 子过程里延时用 `ping -n N 127.0.0.1 >nul`；`timeout /nobreak` 在重定向 stdin 的调用环境（`cmd /c` 经 powershell 包装）下会失败。
- `build-msi.bat` 在 GBK 控制台跑时，既有中文注释行会冒一条 `'X' 不是内部或外部命令` 噪音（UTF-8 注释字节被 GBK 误读），不阻断流程——看到它别当成新故障。

## 遗留/观察项

- 已发布渠道（COS + GitHub Release）上的 v2.4.0/v2.4.1 MSI 都是缺 DLL 的版本，**发布侧修复策略待定**（撤换 2.4.1 还是升 2.4.2 重发），本地已把缺陷版 v2.4.1 MSI 备份到 `desktop/windows/build-msi-x64/defective-2.4.1-shipped/`（gitignored）。
- 既有用户的安装目录里若残留旧 DLL，升级到含 DLL 的修复版后内容会被组件接管，无冲突。
