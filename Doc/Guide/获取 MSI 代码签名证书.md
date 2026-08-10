# 获取 MSI 代码签名证书（Windows 公开发行）

本文面向需要把 VoiceStick Windows 安装包（`VoiceStick_<版本>_<culture>.msi`，即 zh-CN / en-US 两个语言版）从“内测自签”推进到“公开可分发”的签名机操作者。读完可独立完成：选型 → 购买 → 安装 → 接入现有 `build-msi.bat` → 验证 → 建立 SmartScreen 信誉。

## 1. 背景：为什么需要这张证书

当前 `scripts\build-msi.bat` 已经具备完整的 Authenticode 签名链路（详见附录 A），但它签名用的证书是本机的“VoiceStick Dev”自签证书。自签证书只适合这两种场景：

- 内测分发：接收方事先信任你的根证书。
- 本机调试：验证签名流程本身是否跑通。

**自签证书不能用于公开发行**，原因不是签名技术上不对，而是 Windows 不信任自签根：

- SmartScreen 会在用户首次运行安装包时弹“Windows 已保护你的电脑”拦截页。
- UAC 弹窗里发布者显示“未知发布者”。
- Edge / Chrome 下载时会标记“不常下载，可能有害”。

要消除这些，需要一张由受信任的证书颁发机构（CA）签发的**代码签名证书（Code Signing Certificate）**。这张证书就是本文所说的“MSI 认证”。

## 2. 名词速查

| 名词 | 含义 |
|---|---|
| Authenticode | Windows 的代码签名机制，用 signtool 把签名嵌入到 PE 文件（exe/dll）和 MSI 里。 |
| signtool | Windows SDK 自带的签名/验签命令行工具，`build-msi.bat` 已在用。 |
| 时间戳（RFC 3161） | 签名时附带的权威时间。**作用**：证书过期后，在该时间戳之前已签发的文件签名仍然有效，历史版本仍可正常下载安装。 |
| SmartScreen 信誉 | Windows 对“某个签名者”的信任度。新证书签发的文件默认没有信誉，会触发拦截；EV 证书可立即获得信誉。 |
| OV | Organization Validation，CA 验证组织身份后签发。 |
| EV | Extended Validation，更严格的身份验证，私钥强制硬件保护，**立即获得 SmartScreen 信誉**。 |

## 3. 第一步：选 OV 还是 EV

| 维度 | OV 代码签名证书 | EV 代码签名证书 |
|---|---|---|
| 身份验证 | 验证组织/个人身份 | 更严格，需公司电话核实、可能要 DUNS 等 |
| 私钥保护 | 2023 起同样必须硬件保护 | 强制硬件保护（USB token / HSM） |
| SmartScreen | **需积累下载量**才能消除拦截 | **立即获得信誉**，首次发布即无拦截 |
| 价格（2026 概况，以官网为准） | 约 $200–400/年 | 约 $300–700/年 |
| 适合谁 | 预算有限、能接受初期少量用户手动绕过 SmartScreen | 追求首发即静默、面向大量陌生用户 |

**选型建议**：

- 若 VoiceStick 面向公开陌生用户分发（GitHub Release + appcast 自动更新正是这种场景），**首选 EV**——首发就能安静通过 SmartScreen，不会因为拦截页劝退新用户。
- 若预算紧张、能接受首版被 SmartScreen 拦截（用户点“仍要运行”即可），且愿意通过下载量慢慢积累信誉，可选 OV。
- 两者现在**都不再下发纯软件 .pfx**（见第 5 节），交付形式都是硬件或云服务。

## 4. 第二步：选 CA 与购买

主流 CA（按对个人/小团队友好度与价格综合，2026 概况，**以官网实时报价为准**）：

| CA | 特点 | 个人可申请 | 价格区间（年） |
|---|---|---|---|
| Sectigo（原 Comodo） | 性价比高，最主流，reseller 多 | 部分 reseller 接受个人 | OV 低、EV 中 |
| DigiCert | 服务最好，企业首选，价格最高 | 主要面向组织 | 高 |
| GlobalSign | 项目已在用其时间戳服务器，体系成熟 | 主要面向组织 | 中 |
| SSL.com | 支持云签名 eSigner，无需物理 token | 支持 | 中 |
| Certum（波兰 ASSECO） | 价格低，对个人/小团队开放 | 支持 | 低 |

**购买渠道**：

- 直接在 CA 官网下单，或通过授权 reseller（价格常比官网低）。
- 个人开发者：优先看 Certum、SSL.com、Sectigo reseller 是否接受个人身份；多数 CA 的 OV/EV 默认要求组织（公司营业执照）。
- 组织开发者：备好营业执照、可接听的公司电话、DUNS 编号（EV 常要求）。

**国内 CA 注意**：CFCA、沃通 WoSign 等国内 CA 存在，但 WoSign 历史上被 Microsoft/Google 降信任处理过，新申请前务必确认其代码签名证书在 Windows 当前根信任列表内。**公开分发的桌面应用，优先选国际主流 CA**，省去信任域问题。

## 5. 第三步：完成身份验证并拿到私钥

### 5.1 验证流程

- **OV**：提交组织信息后，CA 做企业注册核实 + 电话回拨确认，通常 1–3 个工作日。
- **EV**：在 OV 基础上增加更严格的核实（公司电话、必要时 DUNS、实物寄送等），通常 3–7 个工作日。

### 5.2 私钥交付形式（重点）

自 2023 年 6 月 CA/B Forum 新规全面生效后，**所有新签发的代码签名证书私钥都必须保存在符合 FIPS 140-2 Level 2 或 Common Criteria EAL4+ 的硬件中**。也就是说，你**不会再收到可下载的 .pfx 软证书**，只会收到以下之一：

1. **物理 USB token**（最常见）：DigiCert、Sectigo 多用 SafeNet（Thales）USB token，私钥烧录在 token 内，不可导出。
2. **云签名服务**：如 DigiCert ONE、SSL.com eSigner、Azure Key Vault。私钥在云端 HSM，签名时通过 API/适配器调用。

两种形式 `build-msi.bat` 都能支持：USB token 直接走现有 signtool 命令；云签名需厂商的 signtool 适配器，见第 10 节。

## 6. 第四步：在签名机上安装并验证

以 USB token（SafeNet）为例：

1. **安装 token 驱动**：从 CA 发货邮件里下载 SafeNet Authentication Client，按默认选项安装。安装完成后系统托盘会出现 SafeNet 图标。
2. **插入 token**：首次插入会提示修改 PIN（初始 PIN 由 CA 提供）。改完 PIN 务必妥善保管，连续输错会锁死。
3. **确认证书已注册到本机**：在 PowerShell 里执行：

   ```powershell
   Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
       Format-List Subject, Thumbprint, NotAfter, NotBefore
   ```

   应能看到你购买证书的 Subject（含组织名），`Thumbprint` 是 40 位十六进制 SHA1 指纹。**记下这个 Thumbprint**，下一步要用。

4. （可选）查看证书是否带私钥且为 EV：

   ```powershell
   Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
       Select-Object Subject, Thumbprint, HasPrivateKey,
           @{N='IsEV';E={$_.Extensions.Oid.FriendlyName -match 'Certificate Policy'}}
   ```

## 7. 第五步：让 build-msi.bat 用上新证书

`build-msi.bat` 已经内置了三种证书指定方式，**无需改脚本**，按优先级从高到低任选其一：

### 方式 A（推荐）：写入 `scripts\.signing_sha1`

在签名机本地创建 `scripts\.signing_sha1`，内容只有一行——上一步记下的 Thumbprint（SHA1，可带空格也可不带，脚本会自动去空格）：

```text
A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4
```

- 这个文件**只放签名机本地**，不要提交到 git（虽然 Thumbprint 本身不是机密，但能避免暴露所用 CA 与证书实体）。
- `scripts\` 目前未被 `.gitignore` 单独忽略该文件，建议手动把 `.signing_sha1` 加入忽略。

### 方式 B：环境变量

在签名机设置系统环境变量：

```powershell
[Environment]::SetEnvironmentVariable('SIGNING_SHA1', 'A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4', 'User')
```

适合多项目复用同一证书、不想在每个项目里放文件的场景。

### 方式 C：自动发现（唯一证书时）

什么都不配。脚本会在 `Cert:\CurrentUser\My` 和 `Cert:\LocalMachine\My` 里查找所有 `-CodeSigningCert`、未过期、带私钥的证书，**若恰好只有一个**就用它。当你只在签名机装了这一张代码签名证书时最省事，但装了多张时会因不唯一而回退到 `/a /uw`，不建议在生产签名时依赖此方式。

### 时间戳服务器不用改

`build-msi.bat` 当前用的 RFC 3161 时间戳服务器是：

```text
http://rfc3161timestamp.globalsign.com/advanced
```

时间戳服务器与签名证书**不必同属一家 CA**，公共时间戳服务器对任何 CA 的证书都可用。换证书后**保持原样**即可，无需改动。

### signtool 与 USB token 的兼容性

`build-msi.bat` 的签名参数是：

```text
/v /fd sha256 /sha1 <thumbprint> /tr http://rfc3161timestamp.globalsign.com/advanced /td sha256
```

对 USB token 证书，signtool 会通过 `/sha1` 找到证书，再经由 token 的 CSP（SafeNet 已安装）完成签名。首次签名时 token 会弹 PIN 输入框，输入后 CSP 会缓存 PIN 一段时间。**现有脚本命令对 token 证书开箱即用，无需修改**。

## 8. 第六步：构建并验证签名

### 8.1 构建

在签名机执行（需已配置 WiX、VS 2022、Windows SDK，见 `build-msi.bat` 注释）：

```bat
scripts\build-msi.bat
```

输出（WiX 4.0 逐 culture 构建，产两个语言版）：

```text
desktop\windows\build-msi-x64\VoiceStick_<版本>_zh-CN.msi
desktop\windows\build-msi-x64\VoiceStick_<版本>_en-US.msi
```

### 8.2 验证签名

逐个核对 exe、dll、msi 是否都带签名且签名者正确：

```powershell
$files = @(
    'desktop\windows\build-msi-x64\VoiceStick.exe',
    'desktop\windows\build-msi-x64\WinSparkle.dll',
    'desktop\windows\build-msi-x64\VoiceStick_<版本>_zh-CN.msi',
    'desktop\windows\build-msi-x64\VoiceStick_<版本>_en-US.msi'
)
foreach ($f in $files) {
    $s = Get-AuthenticodeSignature -FilePath $f
    "{0,-40} Status={1} Signer={2}" -f (Split-Path $f -Leaf), $s.Status, $s.SignerCertificate.Subject
}
```

期望输出：三行 `Status=Valid`，且 `Signer=` 是你证书的 Subject（组织名）。`Status=NotSigned` 说明没签上，`Status=UnknownError` 多为 token 未插或 PIN 未解锁。

也可用 signtool 验签（更严格，走系统信任链）：

```powershell
signtool verify /pa /v "desktop\windows\build-msi-x64\VoiceStick_<版本>_zh-CN.msi"
```

### 8.3 UAC 发布者核对

在**另一台没装过你证书的干净 Windows** 上双击 MSI，UAC 弹窗的发布者应显示你的组织名而非“未知发布者”。这是最终用户视角的判据。

## 9. 第七步：建立 SmartScreen 信誉

### EV 证书

**自动获得**，无需任何额外操作。首发版本在陌生用户的 Windows 上即可静默通过 SmartScreen。这是 EV 最大的价值。

### OV 证书

需要积累信誉。新 OV 证书签的首个版本，陌生用户首次下载运行时**仍可能触发 SmartScreen 拦截**（用户点“仍要运行”可继续）。信誉随该签名者的下载量与使用量逐步建立，通常数周到数月后消失。加速方式：

1. **鼓励早期用户主动绕过**，积累真实下载与运行数据（Microsoft 通过遥测统计）。
2. **Microsoft Partner Center 文件提交**（原 Hardware Dashboard 的 file signing 提交）：把已用 OV 证书签名的文件提交给 Microsoft，由 Microsoft 附加签名重新签发，从而快速建立信誉。此功能入口与政策偶有调整，**以 Microsoft 官方当前文档为准**，需先用 EV 证书注册 Windows Hardware Developer 账号。

> 注：Microsoft 的“桌面应用认证（Desktop App Certification / Windows Logo）”新申请通道在 2023 年前后已关闭新提交，仅保留已认证开发者。若你此前未取得该认证，不要把希望寄托在“申请 Logo”上，走 EV 或 OV+信誉积累更现实。

## 10. CI 自动签名的局限与替代

`release.md` 把 Windows 包定为“本地签名机手动签名后上传”，原因之一就是 **USB token 无法上云**——GitHub Actions 等云端 CI 没有物理 USB 口。

若确实需要 CI 自动签名，改用**云签名服务**：

- SSL.com eSigner：提供 signtool 兼容的 Signtool 适配器，用 OAuth 凭据签名，无需 token。
- DigiCert ONE / CertCloud 等：类似机制。
- Azure Key Vault：把证书导入 Key Vault，用 `AzureSignTool`（开源）替代 signtool 签名。

云签名方案需要改造 `build-msi.bat` 的签名步骤（替换 `signtool` 调用或包装一层），属于较大改动，应单独立项评估，不在本指南范围内。

## 11. 续期与维护

- **续期**：代码签名证书有效期通常 1–3 年。**到期前 2–4 周**续期，避免证书空窗期导致发布受阻。续期流程比首次快，多数 CA 对同一实体有简化验证。
- **时间戳保证历史版本有效**：只要签名时带了 RFC 3161 时间戳，证书过期后**已签发的旧版本签名仍然有效**，历史下载不受影响。这正是 `build-msi.bat` 始终带 `/tr /td` 的意义，换证书后务必保持。
- **私钥保护**：USB token 妥善保管，PIN 不要与 token 同处存放。token 支持“签名”用途即可，不要启用不必要的导出权限。
- **token 丢失/损坏**：立即联系 CA 撤销（revoke）该证书，并签发新证书。已签发的带时间戳文件不受撤销影响（撤销只影响撤销时间之后的新校验场景，对历史签名的时间戳校验仍有效）。

## 12. 常见问题

**Q1：自签证书能不能先顶一下公开发行？**
不能。技术上 `build-msi.bat` 能用自签证书签出 MSI，但 Windows 不信任自签根，SmartScreen 与 UAC 都会拦截，等同于没签。公开发行必须用 CA 证书。

**Q2：一个证书能同时签 exe、dll、msi 吗？**
可以。`build-msi.bat` 已在 `[2/4]` 签 `VoiceStick.exe` 和 `WinSparkle.dll`，在 `[4/4]` 签 MSI，三处共用同一 `SIGN_ARGS`、同一证书，无需额外配置。

**Q3：signtool 报“找不到私钥 / Cannot find the certificate and private key”**
通常是 token 未插入、SafeNet 驱动未装、或 PIN 未解锁。先在 PowerShell 跑第 6 节的查询确认证书 `HasPrivateKey=True`；若证书在但签不上，确认 token 已插、SafeNet 托盘显示已连接，必要时拔插一次。

**Q4：换证书后，旧版本 MSI 还能被用户安装吗？**
能。只要旧版本签名时带了时间戳（项目一直带），证书过期或更换后，旧签名的有效性由时间戳保证，用户仍可正常安装。

**Q5：EV 和 OV 可以用同一个 token 吗？**
不能混用。每张证书的私钥独立烧录在各自 token 里。但一台签名机可以同时插多张 token、装多张证书，靠 Thumbprint 区分（这正是 `SIGNING_SHA1` 显式指定的意义，避免多证书时自动发现失效）。

**Q6：`/a /uw` 回退分支会不会被误触发？**
当 `SIGNING_SHA1` 和 `.signing_sha1` 都未配置、且本机不止一张代码签名证书时，脚本会落到 `/a /uw`（自动选证书 + Windows 系统组件验证）。`/uw` 只对 Windows 系统组件有效，普通应用证书会签失败。生产签名**务必用方式 A 或 B 显式指定 Thumbprint**，不依赖自动发现。

## 附录 A：当前 build-msi.bat 签名链路

```text
证书来源（优先级从高到低）:
  1. 环境变量 SIGNING_SHA1
  2. scripts\.signing_sha1 文件（一行 SHA1 thumbprint）
  3. 自动发现: Cert:\CurrentUser\My + Cert:\LocalMachine\My
     过滤 -CodeSigningCert / 未过期 / HasPrivateKey，且唯一时才采用

signtool 定位:
  SIGNTOOL_PATH 环境变量 → PATH 中的 signtool →
  Windows Kits\10\bin\<sdk>\x64\signtool.exe →
  Windows Kits\10\App Certification Kit\signtool.exe

签名参数:
  /v /fd sha256 /sha1 <thumbprint> /tr http://rfc3161timestamp.globalsign.com/advanced /td sha256
  (无 thumbprint 时回退: /v /fd sha256 /a /uw /tr ... /td sha256)

签名对象与校验（每步都 Get-AuthenticodeSignature 校验）:
  [2/4] VoiceStick.exe   → 校验 SignerCertificate 非空
  [2/4] WinSparkle.dll   → 校验 SignerCertificate 非空
  [4/4] VoiceStick_<ver>.msi → 校验 SignerCertificate 非空
```

## 附录 B：signtool 关键参数速查

| 参数 | 作用 |
|---|---|
| `/sha1 <thumbprint>` | 按证书 SHA1 指纹选择签名证书（项目用此方式） |
| `/fd sha256` | 文件摘要算法用 SHA256（必选，SHA1 已弃用） |
| `/tr <url>` | RFC 3161 时间戳服务器 URL |
| `/td sha256` | 时间戳摘要算法用 SHA256 |
| `/v` | 详细输出 |
| `/a` | 自动选择证书（回退用，生产不依赖） |
| `/csp <name>` | 指定加密服务提供程序（token 特殊场景才需） |
| `/kc <container>` | 指定私钥容器名（token 特殊场景才需） |

## 附录 C：参考入口

- Microsoft：Authenticode 与代码签名官方文档（`learn.microsoft.com` 搜索 “signtool” / “Authenticode”）。
- Microsoft Partner Center / Hardware Dashboard（`partner.microsoft.com`）。
- CA 官网：Sectigo、DigiCert、GlobalSign、SSL.com、Certum。
- CA/B Forum Code Signing Certificate Requirements（`cabforum.org`）。
- 项目内：`scripts\build-msi.bat`（签名实现）、`scripts\build-msi-unsigned.bat`（无证书机用）、`Doc\Ref\release.md`（发布流程，Windows 包在签名机手动签后上传）。
