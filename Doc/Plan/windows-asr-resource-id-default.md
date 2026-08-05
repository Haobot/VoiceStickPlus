# Windows 首启 ASR 需切换供应商才可用 - resource_id 默认值修复

## 症状

MSI 安装后，首次启动直接用默认 ASR（volcengine）识别失败；进入设置切换一次 ASR 供应商（或任意改动保存）后，volcengine 与腾讯云两个 ASR 都能正常使用。

## 根因

`config.template.toml:41` 的 `resource_id = ""`（空）。

数据流：

1. MSI 安装把 `config.template.toml` 装到 `Program Files\VoiceStick\`，其中 `resource_id = ""`。
2. 首启 `AppConfig::Load()` 调 `SeedConfigFromTemplate` 把 template 复制到 `%APPDATA%\VoiceStick\config.toml`，然后 `Load(path)` 解析。`app_config.cc:586` 读到 `resource_id = ""`（空字符串，非 nullopt），**覆盖** `app_config.h:217` 的成员默认值 `volc.seedasr.sauc.duration`。`config_.resource_id = ""`。
3. 内置 volcengine key 非空 -> `NeedsAsrStep` 返回 false -> onboarding 跳过 kAsr 步直接 kReady。`OnboardingDialog::SaveControlsIntoConfig`（`onboarding_dialog.cc:398`）因 kReady 步未创建 `provider_combo_` 直接 return，不填 `resource_id`。
4. onboarding 完成 `UpdateConfig` 重建 `asr_ = AsrClientWin(config_)`，`config_.resource_id` 仍为空。
5. 录音时 `AsrClientWin::RunReusableWebSocket`（`asr_client_win.cc:248`）发 `X-Api-Resource-Id: `（空），`AsrProtocol::MakeStartSessionFrame`（`asr_protocol.cc:365`）写 JSON `"resource_id":""`。volcengine 服务端拒绝（`X-Api-Resource-Id` 是必需头，见 `Doc/Ref/volcengine-asr.md:28`）。**volcengine ASR 失败**。
6. 腾讯云 ASR 不需要 `resource_id`，始终能用。
7. 用户打开设置，`settings_dialog.cc:1027-1030` 加载 `resource_id`（空）-> `CB_FINDSTRINGEXACT` 找空返回 -1 -> `CB_SETCURSEL 0`（选中 `SupportedResourceIds()[0]` = `volc.seedasr.sauc.duration`）。保存时 `settings_dialog.cc:1150-1156` 把该有效值写入 `config_.resource_id`。`UpdateConfig` 重建 `asr_`，volcengine ASR 成功。**"切换一次后两个都能用"**。

关键差异：`resource_id` 是唯一"template 空、settings 保存填有效值"的字段（其他凭据字段空时 `Active*()` 回退内置，LLM 字段亦然）。

## 修复方案

### 1. `AppConfig::ActiveResourceId()` 回退默认（核心修复）

`resource_id` 空时回退 `SupportedResourceIds().front()`（`volc.seedasr.sauc.duration`）。覆盖所有场景：新安装、升级（config.toml 已存在 resource_id 空）、config 缺失。

- `app_config.h`：声明 `std::string ActiveResourceId() const;`（紧随 `ActiveWebsocketUrl()`）
- `app_config.cc`：实现
  ```cpp
  std::string AppConfig::ActiveResourceId() const {
      const auto& ids = SupportedResourceIds();
      return ResolveActiveString(resource_id, ids.empty() ? std::string{} : ids.front());
  }
  ```

### 2. 调用点改用 `ActiveResourceId()`

- `asr_client_win.cc:248`：`config_.resource_id` -> `config_.ActiveResourceId()`
- `asr_protocol.cc:366`：`config.resource_id` -> `config.ActiveResourceId()`

UI（settings/onboarding）与 Load/Save 仍读写原 `resource_id` 字段，不受影响。

### 3. `config.template.toml` 填默认值

`resource_id = ""` -> `resource_id = "volc.seedasr.sauc.duration"`。让新安装的 config.toml 有合理可见默认值（与成员默认一致），不依赖运行时回退。

### 4. 单测

`core_tests.cc` 加 `TestActiveResourceId`：空回退默认、非空用配置值。

## 不改动的部分

- `Active*()` 凭据回退机制（已正确）。
- onboarding 跳过 kAsr 逻辑（已正确）。
- settings 保存逻辑（已正确填 resource_id）。
- `ResolveActiveApiKey` 的 cloud 不回退设计（已正确）。

## 验证

- `ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests` 全绿。
- 重新构建 MSI，首启直接用 volcengine ASR 识别成功（不需切换）。
