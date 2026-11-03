---
name: install-macos-app
description: >-
  macOS 桌面端构建产物覆盖安装到 /Applications 的标准流程与验证方法。当用户说"安装 VoiceStick / 装到 /Applications /
  覆盖安装 / 更新本机程序 / 启动的是老版本 / 怎么还在跑旧包" 等，或 macOS 构建完成后需要装进 /Applications 验证功能时使用。
  核心坑：所有开发构建 CFBundleShortVersionString 恒为 2.4.10，版本号无法区分新旧，必须 md5 对拍验证。
---

# macOS 构建产物安装到 /Applications

把 `build/` 下的最新 VoiceStick.app 覆盖安装进 `/Applications`。**登录启动项与 Spotlight 都指向
`/Applications/VoiceStick.app`**——构建产物不装过去，用户启动的永远是旧包（2026-11-02 实例：/Applications
里是 10-29 包，10-31 两笔 targeted 修复等于没生效，复盘白跑一轮）。

## ⚠️ 两个核心坑

1. **版本号是哑的**：所有开发构建 `CFBundleShortVersionString` 恒为 `2.4.10`，新旧只能靠
   **二进制 md5 + 构建时间**区分，别信 Info.plist。
2. **可执行文件名是 `VoiceStickApp` 不是 `VoiceStick`**：md5 校验前先读 `CFBundleExecutable`，
   直接拼 `Contents/MacOS/VoiceStick` 会报 No such file or directory。

## 标准安装流程

```bash
SRC="/Users/hwp/Dev/DVG/VoiceStickPlus/build/VoiceStick-2.4.10.app"   # 以 build/ 下 mtime 最新的 .app 为准

# 1) 退出运行实例
pkill -f VoiceStickApp || true

# 2) 先删再装（ditto 直接覆盖会在旧 bundle 里残留新包没有的文件）
rm -rf /Applications/VoiceStick.app
ditto "$SRC" /Applications/VoiceStick.app

# 3) md5 对拍验证
EXE=$(defaults read /Applications/VoiceStick.app/Contents/Info.plist CFBundleExecutable)
md5 -q "$SRC/Contents/MacOS/$EXE"
md5 -q "/Applications/VoiceStick.app/Contents/MacOS/$EXE"   # 两者必须一致
```

装完即生效：登录启动项路径不变无需改动；TCC 授权因自签证书固定身份（CN `VoiceStick Local Code Signing`）
跨安装保留。

## 判断「某笔修复在不在包里」（版本号不可用时的三步字符串指纹法）

```bash
# 1) 从修复提交提取新增字面量（Swift 字符串插值里的词也是字面量，如 still_old）
git show <fix-commit> -U0 | grep '^+' | grep -oE '"[^"]+"' | sort -u
# 2) 对照父提交确认该字面量确实是新增的（0 = 新增）
git grep -c '"<字面量>"' '<fix-commit>^' -- desktop/macos
# 3) 在目标二进制里找（>0 = 包含该修复）
strings "<二进制路径>/Contents/MacOS/$EXE" | grep -c '<字面量>'
```

注意：**构建时间早于 commit 时间一两分钟不代表不含该修复**——常规流程就是改完 → 构建 → 提交，同分钟属正常。
以字符串指纹为准，不要凭时间戳判死（2026-11-02 曾误判 10-31 12:40 的包"早于 1a62870 提交不可信"，
指纹验证（`still_old`×1、`voice_targeted`×10）后确认它就含 afb0137+1a62870）。

## 历史副本清理（2026-11-02 已完成，再冒出来按同名清）

当时全机共存 5 份：

| 位置 | 构建时间 | 处置 |
|---|---|---|
| `/Applications/VoiceStick.app` | 10-29 | 覆盖为 build 最新包（登录启动项指向它） |
| `/Applications/VoiceStick-2.4.10.app` | 10-29（与上者二进制完全相同） | 已删 |
| `/Applications/VoiceStick-old-20261020.app` | 10-16 | 已删 |
| `build/install-backup-20261019-133746/VoiceStick.app` | 10-16 | 已删 |
| `build/VoiceStick-2.4.10.app` | 10-31（安装源） | 保留 |

清理原则：`/Applications` 只留 `VoiceStick.app` 一个名字；`build/install-backup-*/` 可删；
`build/` 里的版本号 `dmg/zip/signature` 是发布产物，与安装无关，不要顺手删。

## 启动验证

测试启动一律 `open /Applications/VoiceStick.app`（CLI 直接启动会因 TCC 归因失效静默失去蓝牙，见 Hub 高频坑）。
验证目标功能前可再复核一次：
`strings "/Applications/VoiceStick.app/Contents/MacOS/$EXE" | grep -c <修复特征字面量>`。
