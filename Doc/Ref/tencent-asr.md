# 腾讯云 ASR 参考

Voice Stick Windows 桌面端支持腾讯云实时语音识别（ASR）作为 `asr_provider = "tencent"`。

## API 端点

- **WebSocket 实时识别**: `wss://asr.cloud.tencent.com/asr/v2/<appid>?<签名参数>`
- **热词表管理 REST API**: `https://asr.tencentcloudapi.com/`

## 鉴权

| 场景 | 方式 |
|------|------|
| WebSocket ASR | URL 查询参数 HMAC-SHA1 签名 |
| 热词表管理 | HTTP Header `Authorization: TC3-HMAC-SHA256 ...` |

## 引擎模型

| 值 | 说明 |
|----|------|
| `16k_zh_en` | **普方英大模型**（推荐）。普通话 + 粤语 + 英语 + 多种方言 |
| `16k_zh` | 中文普通话通用模型 |
| `8k_zh` | 电话 8k 中文模型 |
| `16k_multi_lang` | 多语种（15 种语言自动识别） |

## WebSocket 协议

参考 [腾讯云实时语音识别（WebSocket）API](https://cloud.tencent.com/document/product/1093/48982)。

1. 客户端连接 WebSocket（URL 已包含 `voice_format`、`needvad`、`hotword_list` 等参数）
2. 服务端握手确认: `{"code": 0, "message": "success", "voice_id": "..."}`
3. 客户端发送二进制帧: Opus 音频数据，每帧封装为 `Opus`(4B) + 大端长度(2B) + Opus 一帧压缩数据
4. 服务端发送文本帧: 识别结果 `{"code": 0, "result": {"slice_type": 0/1/2, "voice_text_str": "..."}}`
5. 音频发送完毕后，客户端发送文本帧: `{"type": "end"}`（小写，固定格式）
6. 服务端发送最终结果并关闭连接

注意：腾讯云实时 ASR **没有 START 消息**，所有配置通过 URL 查询参数传递；结束消息必须是 `"end"` 小写。

### slice_type

| 值 | 含义 | Voice Stick 映射 |
|----|------|-----------------|
| 0 | 开始识别，无文本 | 忽略 |
| 1 | 中间结果（非稳态） | → `on_partial` |
| 2 | 最终结果（稳态） | → `on_final` |

### 错误码

| 错误码 | 说明 |
|--------|------|
| 4000 | 音频数据发送过多 |
| 4001 | 参数不合法 |
| 4002 | 鉴权失败（检查 SecretId/SecretKey） |
| 4003 | AppID 未开通语音识别服务 |
| 4004 | 资源包耗尽 |
| 4005 | 账户欠费 |
| 4006 | 并发超限 |
| 4007 | 音频解码失败 |
| 4008 | 客户端超 15 秒未发送音频 |

## 热词表管理

### 自动管理

当 `tencent_hotword_id` 为空且 `asr_hotwords` 不为空时，程序自动调用 REST API：

1. 查找名为 `VoiceStick-Hotwords` 的热词表
2. 如存在则更新，否则新建
3. 将返回的 VocabId 用于本次 ASR 会话

### 手动管理

用户可在腾讯云控制台 [语音识别 > 热词表](https://console.cloud.tencent.com/asr/vocab) 创建热词表，
将 VocabId 填入配置 `tencent_hotword_id`。

### 限制

- 每账户最多 30 个热词表
- 每表最多 1000 词
- 每词 ≤10 汉字 / 30 英文字符
- 权重范围 [1, 11] 或 100

## 配置示例

```toml
asr_provider = "tencent"

tencent_secret_id = "AKID..."
tencent_secret_key = "..."
tencent_appid = "1234567890"
tencent_engine_model_type = "16k_zh_en"
tencent_hotword_id = ""              # 可选，留空自动管理
asr_hotwords = "小智,VoiceStick,语音输入"
```

## 实现文件

| 文件 | 说明 |
|------|------|
| `desktop/windows/src/asr_client_tencent.cc` | WebSocket ASR 客户端 |
| `desktop/windows/src/asr_client_tencent.h` | 客户端类声明 |
| `desktop/windows/src/tencent_asr_vocab_client.cc` | 热词表 REST API 客户端 |
| `desktop/windows/src/tencent_asr_vocab_client.h` | 热词表客户端声明 |
| `desktop/windows/src/app_config.h` | `AsrProvider::kTencent` + 配置字段 |
| `desktop/windows/tests/core_tests.cc` | `TestTencent*()` 单元测试 |

## 参考资料

- [腾讯云实时语音识别（WebSocket）API](https://cloud.tencent.com/document/api/1093/48982)
- [创建热词表 API](https://cloud.tencent.com/document/product/1093/41111)
- [更新热词表 API](https://cloud.tencent.com/document/api/1093/41108)
- [API 概览](https://cloud.tencent.com/document/product/1093/35637)
