# Windows MSI 内置密钥 + 向导免填 key + 替换 config 不破坏 ASR

## 背景

内测分发目标:MSI 自带腾讯云 ASR、火山引擎 ASR、DeepSeek(LLM)密钥,内测用户安装后自动具备语音识别、文本精修、热词精修全部功能;初始化向导不显示 API Key 配置、不强制填写即可完成。同时修复用户报告的"替换 config.toml 后 ASR 仍可能不工作"。

## 根因(已由代码调研证实)

### 1. MSI 内置密钥机制--已就绪,无需改代码
`VOICESTICK_CONFIG_TEMPLATE` 环境变量注入链路完整(`build-msi.bat:134-146` / `build-msi-unsigned.bat:70-82`),WiX 装模板到 Program Files(`VoiceStick.wxs:41-43`),首启 `SeedConfigFromTemplate`(`app_config.cc:298-310`)复制到 `%APPDATA%`(目标存在不覆盖)。只需分发时准备含密钥的 config 并设置环境变量构建。

### 2. 向导强制填 key--与目标矛盾
- `NeedsOnboarding()`(`onboarding_dialog.cc:83-85`)= `paired_device_ids.empty() || ActiveApiKey().empty()`;首启无配对设备仍弹向导。
- `GoNext()` kAsr 步门禁 `HasApiKey()`(`L499-507`)仅非空校验,**取消向导=退出程序**。
- 设置对话框 API key 框已隐藏(`settings_dialog.cc:578-590`,未进布局表),但向导仍强制填。
- 向导 `Show()` 会 `AppConfig::Load()` 重读文件(`L99`),`LoadConfigIntoControls` 把内置 key 预填到密码框(`L381-389`)--所以内置 key 不会被空输入覆盖,但用户仍被迫走 kAsr 步看到"请输入 API Key"界面。

### 3. 替换 config.toml 后 ASR 不工作--路径 A(最严重)
- 程序运行时**不重读 config.toml**(无文件监视,`win32_app.cc:322` 仅启动加载一次)。
- `AppConfig::Save()` 是**全量截断重写**(`app_config.cc:739-887`,逐字段写出全部 key)。
- 用户运行时替换 config.toml 后,任何 `config_.Save()`(改主题色/输出/编码器/热词等 10+ 处)用 `Win32App::config_` 旧内存值整体覆盖磁盘,真 key 被抹掉,**重启无效**。

次要路径:B(ASR client 持过期快照,需重启)、C(便携模式读错路径,exe 同级残留 `config.toml` 触发 `IsPortableMode`)、D(占位符 key 通过本地 `empty()` 校验,服务端鉴权失败)、E(腾讯缺 `secret_key`/`appid`)。

## 方案(用户已确认三项推荐选择)

### 改动 1:向导内置 key 时跳过 kAsr 步
`onboarding_dialog.cc` `GoNext()` 的 kDevice 步:配对设备后,若 `config_.ActiveApiKey()` 非空(内置 key)直接进 `kReady`,否则进 `kAsr`。公开版(无内置 key)仍走 kAsr 让用户填。

提取纯函数 `NeedsAsrStep(const AppConfig&) = config.ActiveApiKey().empty()` 便于单测。

### 改动 2:运行时 Save 保留磁盘凭据(修复路径 A)
- `app_config` 新增 `SavePreservingDiskCredentials()`:Load 磁盘最新 config,把 15 个 UI 隐藏的纯凭据/连接字段拷到副本,再 `Save`。
- `win32_app.cc` 运行时 Save 点(10+ 处:主题色/大小/位置、设备输出、编码器、体感调参、交互设置、划词热词、SaveInputOptions、热词提取等)改用此方法。
- `onboarding Finish`(`onboarding_dialog.cc:514`)、设置对话框保存(`settings_dialog.cc:1194`)仍用普通 `Save()`(正常写,含 key)。

**保留字段清单**(UI 全隐藏,用户只能手改文件):
`voicestick_api_key`、`voicestick_cloud_url`、`volcengine_api_key`、`volcengine_boosting_table_id`、`volcengine_correct_table_id`、`tencent_secret_id`、`tencent_secret_key`、`tencent_appid`、`tencent_engine_model_type`、`tencent_hotword_id`、`llm_base_url`、`llm_api_key`、`llm_model`、`refine_prompt`、`hotword_process_prompt`。

**不保留**(UI 可改,用内存值):`asr_provider`(onboarding 可改)、`refine_enabled`/`hotword_process_enabled`/`hotword_mining_enabled`(开关)、`resource_id`(向导可改)、`asr_hotwords`(划词可改)。

### 改动 3:分发文档 + config 示例
更新 `Doc/Plan/windows-msi-config-template-seed.md`:附三 provider + DeepSeek 内置 config 示例(密钥占位符)、向导跳过 kAsr 说明、路径 B/C 注意事项(替换 config 前先退出程序、便携模式诊断看启动日志 `portable_mode`)。

`resources/config.template.toml` 保持占位脱敏不变(密钥不进仓库)。用户本机复制示例填真实密钥后 `set VOICESTICK_CONFIG_TEMPLATE=<路径>` && `build-msi-unsigned.bat`(本机)/`build-msi.bat`(签名机)。

#### 内置 config 示例(用户本机填密钥)
```toml
asr_provider = "volcengine"
volcengine_api_key = "<火山引擎 ASR key>"
volcengine_boosting_table_id = ""
volcengine_correct_table_id = ""
tencent_secret_id = "<腾讯云 SecretId>"
tencent_secret_key = "<腾讯云 SecretKey>"
tencent_appid = "<腾讯云 AppId>"
tencent_engine_model_type = "16k_zh"
tencent_hotword_id = ""
llm_base_url = "https://api.deepseek.com/v1"
llm_api_key = "<DeepSeek key>"
llm_model = "deepseek-chat"
refine_enabled = true
hotword_process_enabled = true
# 其余字段沿用 resources/config.template.toml 默认
```

## TDD

- `TestNeedsAsrStep`:有 key 返回 false(跳过 kAsr),无 key 返回 true(进 kAsr)。
- `TestSavePreservingDiskCredentials`:磁盘有 key、内存无 key(或不同),Save 后磁盘 key 保留;非凭据字段(如 `auto_enter`)用内存值写入。
- 现有 `TestConfigTemplateSeeding` 等不破坏。

## 改动清单

| 文件 | 改动 |
|---|---|
| `desktop/windows/src/onboarding_dialog.cc` | `GoNext` kDevice 步:有 key 跳过 kAsr;新增 `NeedsAsrStep` 纯函数 |
| `desktop/windows/src/app_config.h` | 声明 `SavePreservingDiskCredentials` |
| `desktop/windows/src/app_config.cc` | 实现 `SavePreservingDiskCredentials`(Load 磁盘 + 拷贝 15 凭据字段 + Save) |
| `desktop/windows/src/win32_app.cc` | 运行时 Save 点(10+ 处)改用 `SavePreservingDiskCredentials` |
| `desktop/windows/tests/core_tests.cc` | `TestNeedsAsrStep` + `TestSavePreservingDiskCredentials`,加入 `main()` |
| `Doc/Plan/windows-msi-config-template-seed.md` | 分发示例 + 向导行为 + 路径 B/C 注意 |

## 验证
- `ctest --test-dir desktop/windows/build-x64 --output-on-failure` 全绿(含新测试)。
- `build_win.bat` 构建通过,核对 exe 时间戳/体积。
- 真机(内测 MSI):
  1. 安装内置 key MSI -> 首启向导只配对设备、不显示 key 配置 -> 录音识别正常、文本精修/热词精修可用。
  2. 运行时替换 `%APPDATA%\VoiceStick\config.toml`(含新 key)-> 改主题色触发 Save -> 重启 -> 核对 config.toml 的 key 保留(SHA256)-> ASR 用新 key 正常。
  3. 便携模式诊断:启动日志 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log` 查 `portable_mode=`。

## 不在本次范围
- 路径 B(文件监视重载):复杂度高,路径 A 修复后重启可解决,暂不加,文档化"改 config 后重启生效"。
- 路径 D(占位符本地校验):内置真实 key 后非必需,暂不加。
- 公开版腾讯向导 secret_key/appid 残缺:内置 key 版跳过 kAsr 不受影响,公开版另议。
