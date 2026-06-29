## 语音流异步识别相关接口

| 接口名称 | 接口功能 | 频率限制（次/秒） |
|-|-|-|
| [CloseAsyncRecognitionTask](/document/api/1093/52130) | 语音流异步识别任务关闭 | 20 |
| [DescribeAsyncRecognitionTasks](/document/api/1093/52060) | 语音流异步识别任务列表 | 20 |
| [CreateAsyncRecognitionTask](/document/api/1093/52061) | 语音流异步识别任务创建 | 20 |

## 录音文件识别相关接口

| 接口名称 | 接口功能 | 频率限制（次/秒） |
|-|-|-|
| [DescribeTaskStatus](/document/api/1093/37822) | 录音文件识别结果查询 | 50 |
| [CreateRecTask](/document/api/1093/37823) | 录音文件识别请求 | 20 |

## 一句话识别相关接口

| 接口名称 | 接口功能 | 频率限制（次/秒） |
|-|-|-|
| [SentenceRecognition](/document/api/1093/35646) | 一句话识别 | - |

## 自学习相关接口

| 接口名称 | 接口功能 | 频率限制（次/秒） |
|-|-|-|
| [CreateCustomization](/document/api/1093/48601) | 创建自学习模型 | 20 |
| [GetCustomizationList](/document/api/1093/48498) | 查询自学习模型列表 | 20 |
| [ModifyCustomization](/document/api/1093/48598) | 更新自学习模型 | 20 |
| [DeleteCustomization](/document/api/1093/48600) | 删除自学习模型 | 20 |
| [ModifyCustomizationState](/document/api/1093/48597) | 修改自学习模型状态 | 20 |
| [DownloadCustomization](/document/api/1093/48599) | 下载自学习模型语料 | 20 |
| [GetModelInfo](/document/api/1093/90813) | 获取自学习模型信息 | 20 |

## 热词相关接口

| 接口名称 | 接口功能 | 频率限制（次/秒） |
|-|-|-|
| [CreateAsrVocab](/document/api/1093/41111) | 创建热词表 | 20 |
| [DeleteAsrVocab](/document/api/1093/41110) | 删除热词表 | 20 |
| [DownloadAsrVocab](/document/api/1093/41485) | 下载热词表 | 20 |
| [GetAsrVocab](/document/api/1093/41109) | 获取热词表 | 20 |
| [GetAsrVocabList](/document/api/1093/41484) | 列举热词表 | 20 |
| [SetVocabState](/document/api/1093/41483) | 设置热词表状态 | 20 |
| [UpdateAsrVocab](/document/api/1093/41108) | 更新热词表 | 20 |

## 说话人验证相关接口

| 接口名称 | 接口功能 | 频率限制（次/秒） |
|-|-|-|
| [VoicePrintEnroll](/document/api/1093/94483) | 说话人注册 | 20 |
| [VoicePrintVerify](/document/api/1093/94481) | 说话人认证 | 20 |
| [VoicePrintUpdate](/document/api/1093/94482) | 说话人更新 | 20 |
| [VoicePrintDelete](/document/api/1093/94484) | 说话人删除 | 20 |
| [VoicePrintCompare](/document/api/1093/103720) | 说话人比对 | 20 |
| [VoicePrintCount](/document/api/1093/96061) | 说话人注册数量统计 | 20 |
| [VoicePrintGroupVerify](/document/api/1093/107668) | 说话人验证1:N | 20 |

## 关键字检索相关接口

| 接口名称 | 接口功能 | 频率限制（次/秒） |
|-|-|-|
| [CreateAsrKeyWordLib](/document/api/1093/110576) | 创建关键词表 | 20 |
| [DeleteAsrKeyWordLib](/document/api/1093/110575) | 删除关键词表 | 20 |
| [GetAsrKeyWordLibList](/document/api/1093/110574) | 列举关键词表 | 20 |
| [UpdateAsrKeyWordLib](/document/api/1093/110573) | 更新关键词表 | 20 |

## 业务用量相关接口

| 接口名称 | 接口功能 | 频率限制（次/秒） |
|-|-|-|
| [GetUsageByDate](/document/api/1093/111167) | 通过日期查询用量 | 20 |

>! 以上给出的接口频率限制维度为`API + 接入地域 + 子账号`，有关限频更多说明参考：[API 频率限制说明](https://cloud.tencent.com/document/product/1278/109059)