> **注意：**
> 

> 此接口为实时语音识别接口，在参数风格、错误码等方面有区别于云 API 接口，请知悉。
> 


## 接口描述

本接口服务采用 WebSocket 协议，对实时音频流进行识别，同步返回识别结果，达到“边说边出文字”的效果。

在使用该接口前，需要 [开通语音识别服务](https://cloud.tencent.com/document/product/1093/54362)，并进入 [API 密钥管理页面](https://console.cloud.tencent.com/cam/capi) 新建密钥，生成 AppID、SecretID 和 SecretKey，用于 API 调用时生成签名，签名将用来进行接口鉴权。

## 接口要求

集成实时语音识别 API 时，需按照以下要求。
<table>
<tr>
<td rowspan="1" colSpan="1" >内容</td>

<td rowspan="1" colSpan="1" >说明</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >语言种类</td>

<td rowspan="1" colSpan="1" >支持中文普通话、粤语、英语、韩语、日语、泰语、印度尼西亚语、越南语、马来语、菲律宾语、葡萄牙语、土耳其语、阿拉伯语、西班牙语、印地语、法语、德语、上海话、四川话、武汉话、贵阳话、昆明话、西安话、郑州话、太原话、兰州话、银川话、西宁话、南京话、合肥话、南昌话、长沙话、苏州话、杭州话、济南话、天津话、石家庄话、黑龙江话、吉林话、辽宁话、闽南语、客家话、南宁话、潮汕话、宁波话、无锡话、吴语。可通过接口参数 engine_model_type 设置对应语言类型。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >支持行业</td>

<td rowspan="1" colSpan="1" >通用、金融、游戏、教育、医疗。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >音频属性</td>

<td rowspan="1" colSpan="1" >- 采样率：16000Hz 或8000Hz<br>- 采样精度：16bits<br>- 声道：单声道（mono）</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >音频格式</td>

<td rowspan="1" colSpan="1" >pcm、wav、opus、speex、silk、mp3、m4a、aac</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >请求协议</td>

<td rowspan="1" colSpan="1" >wss 协议</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >请求地址</td>

<td rowspan="1" colSpan="1" ><code>wss://asr.cloud.tencent.com/asr/v2/<appid>?{请求参数} </code></td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >接口鉴权</td>

<td rowspan="1" colSpan="1" >签名鉴权机制，详情请参见 <a href="https://cloud.tencent.com/document/product/1093/48982#signature">签名生成</a>。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >响应格式</td>

<td rowspan="1" colSpan="1" >统一采用 JSON 格式。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >数据发送</td>

<td rowspan="1" colSpan="1" >- 建议200ms 发送200ms 时长（即1:1实时率）的数据包，对应 PCM 大小为：8k 采样率3200字节，16k 采样率6400字节。<br>- 音频发送速率过快超过1:1实时率或者音频数据包之间发送间隔超过6秒，可能导致引擎出错，后台将返回错误并主动断开连接。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >并发限制</td>

<td rowspan="1" colSpan="1" >默认单账号限制并发数为200路，如您有提高并发限制的需求，请 <a href="https://buy.cloud.tencent.com/asr">前往购买</a>。</td>
</tr>
</table>


### 业务流程图

![](https://qcloudimg.tencent-cloud.cn/image/document/4c425a8405a088aaab5e423286871cc7.png)


## 接口调用流程

接口调用流程分为两个阶段：握手阶段和识别阶段。两阶段后台均返回 text message，内容为 JSON 序列化字符串，以下是格式说明：
<table>
<tr>
<td rowspan="1" colSpan="1" >字段名</td>

<td rowspan="1" colSpan="1" >类型</td>

<td rowspan="1" colSpan="1" >描述</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >code</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >状态码，0代表正常，非0值表示发生错误。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >message</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >错误说明，发生错误时显示这个错误发生的具体原因，随着业务发展或体验优化，此文本可能会经常保持变更或更新。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >voice_id</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >音频流全局唯一标识，一个 WebSocket 连接对应一个，用户自己生成（推荐使用 UUID），最长128位。<br><br><strong>注意：</strong>每次建立 WebSocket 连接都必须重新生成 voice_id，任何情况下中断了识别或者 WebSocket 连接(如连接失败、正常结束、识别返回错误码等)，voice_id 作废，重新发起识别必须使用新的 voice_id。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >message_id</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >本 message 唯一 ID。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >result</td>

<td rowspan="1" colSpan="1" >Result</td>

<td rowspan="1" colSpan="1" >最新语音识别结果。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >final</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >该字段返回1时表示音频流全部识别结束。</td>
</tr>
</table>


其中识别结果 Result 结构体格式为：
<table>
<tr>
<td rowspan="1" colSpan="1" >字段名</td>

<td rowspan="1" colSpan="1" >类型</td>

<td rowspan="1" colSpan="1" >描述</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >slice_type</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >识别结果类型：<br>- 0：一段话开始识别。<br>- 1：一段话识别中，voice_text_str 为非稳态结果（该段识别结果还可能变化）。<br>- 2：一段话识别结束，voice_text_str 为稳态结果（该段识别结果不再变化）。<br>根据发送的音频情况，识别过程中可能返回的 slice_type 序列有：<br>- 0-1-2：一段话开始识别、识别中（可能有多次1返回）、识别结束。<br>- 0-2：一段话开始识别、识别结束。<br>- 2：直接返回一段话完整的识别结果。<br><br><strong>注意：</strong>如果需要0和2配对返回，需要设置 filter_empty_result=0（slice_type=0时，识别结果可能为空，默认是不返回空识别结果的）。一般在外呼场景需要配对返回，通过 slice_type=0 来判断是否有人声出现。<br>示例值：0</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >index</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >当前一段话结果在整个音频流中的序号，从0开始逐句递增。<br>示例值：0</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >start_time</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >当前一段话结果在整个音频流中的起始时间。<br>示例值：60</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >end_time</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >当前一段话结果在整个音频流中的结束时间。<br>示例值：2700</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >voice_text_str</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >当前一段话文本结果，编码为 UTF8。<br>示例值：ASR 语音识别结果。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >word_size</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >当前一段话的词结果个数。<br>示例值：1</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >word_list</td>

<td rowspan="1" colSpan="1" >Word Array</td>

<td rowspan="1" colSpan="1" >当前一段话的词列表，Word 结构体格式为：<br>- word：String 类型，该词的内容。<br>- start_time：Integer 类型，该词在整个音频流中的起始时间。<br>- end_time：Integer 类型，该词在整个音频流中的结束时间。<br>- stable_flag：Integer 类型，该词的稳态结果，0表示该词在后续识别中可能发生变化，1表示该词在后续识别过程中不会变化。<br>- 示例值： [{"word":"我","start_time":380,"end_time":680,"stable_flag":1}]</td>
</tr>
</table>


### 握手阶段

#### 请求格式

握手阶段，客户端主动发起 WebSocket 连接请求，请求 URL 格式为：
``` bash
wss://asr.cloud.tencent.com/asr/v2/<appid>?{请求参数}
```

其中 <appid> **需替换为腾讯云注册账号的** appid，可通过 [API 密钥管理页面](https://console.cloud.tencent.com/cam/capi) 获取，{请求参数}格式为：
``` bash
key1=value2&key2=value2...
```

参数说明：
<table>
<tr>
<td rowspan="1" colSpan="1" >参数名称</td>

<td rowspan="1" colSpan="1" >必填</td>

<td rowspan="1" colSpan="1" >类型</td>

<td rowspan="1" colSpan="1" >描述</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >secretid</td>

<td rowspan="1" colSpan="1" >是</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >腾讯云注册账号的密钥 secretid，可通过 <a href="https://console.cloud.tencent.com/cam/capi">API 密钥管理页面</a> 获取。<br>示例值：*****Qq1zhZMN8dv0******</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >timestamp</td>

<td rowspan="1" colSpan="1" >是</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >当前 UNIX 时间戳，单位为秒。如果与当前时间相差过大，会引起签名过期错误。<br>示例值：1745932688</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >expired</td>

<td rowspan="1" colSpan="1" >是</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >签名的有效期截止时间 UNIX 时间戳，单位为秒。expired 必须大于 timestamp 且 expired - timestamp 小于90天。<br>示例值：1746019088</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >nonce</td>

<td rowspan="1" colSpan="1" >是</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >随机正整数。用户需自行生成，最长10位。<br>示例值：8743357</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >engine_model_type</td>

<td rowspan="1" colSpan="1" >是</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >引擎模型类型<br>识别引擎采用分级计费方案，标记为“大模型版”的引擎适用大模型计费方案，请参见 <a href="https://cloud.tencent.com/document/product/1093/35686">计费概述（在线版）</a>。<br>电话场景：<br>- 8k_zh：中文电话通用。<br>- 8k_en：英文电话通用。<br>- <strong>8k_zh_large</strong>：中文电话场景专用大模型引擎【大模型版】。当前模型同时支持中文、上海话、四川话、武汉话、贵阳话、昆明话、西安话、郑州话、太原话、兰州话、银川话、西宁话、南京话、合肥话、南昌话、长沙话、苏州话、杭州话、济南话、天津话、石家庄话、黑龙江话、吉林话、辽宁话、闽南语、客家话、粤语、南宁话方言识别，通过显著提升模型参数规模与语言建模能力，实现对电话音频中复杂场景（如口音干扰、背景噪声）的高精度识别，识别准确率较常规版本大幅提升。<br><br><strong>注意</strong>：如您有电话通讯场景识别需求，但发现需求语种仅支持 16k，可将 input_sample_rate 参数设置为8000，使用下方 16k 引擎，亦能获取识别结果。但 16k 引擎并非基于电话通讯数据训练，无法承诺此种调用方式在电话场景下的识别效果，需由您自行验证识别结果是否可用。<br><br>非电话场景：<br>- <strong>16k_zh_en </strong>：普方英大模型引擎【大模型版】。当前模型同时支持中文、英文和 <a href="https://cloud.tencent.com/document/product/1093/35682">31种方言</a> 的识别，模型参数量极大，语言模型性能增强，针对噪声大、回音大、人声小、人声远等低质量音频的识别准确率极大提升;<br>- <strong>16k_multi_lang</strong>：多语种大模型引擎【大模型版】。当前模型同时支持英语、日语、韩语、阿拉伯语、菲律宾语、法语、印地语、印尼语、马来语、葡萄牙语、西班牙语、泰语、土耳其语、越南语、德语的识别，可实现15个语种的自动识别（句子/段落级别）；<br>- <strong>16k_en_large：</strong>英文大模型引擎【大模型版】支持英语；<br>- 16k_zh：中文通用；<br>- 16k_zh-TW：中文繁体；<br>- 16k_zh_edu：中文教育；<br>- 16k_zh_medical：中文医疗；<br>- 16k_zh_court：中文法庭；<br>- 16k_yue：粤语；<br>- 16k_en：英文通用；<br>- 16k_en_game：英文游戏；<br>- 16k_en_edu：英文教育；<br>- 16k_ko：韩语；<br>- 16k_ja：日语；<br>- 16k_th：泰语；<br>- 16k_id：印度尼西亚语；<br>- 16k_vi：越南语；<br>- 16k_ms: 马来语；<br>- 16k_fil: 菲律宾语；<br>- 16k_pt：葡萄牙语；<br>- 16k_tr：土耳其语；<br>- 16k_ar：阿拉伯语；<br>- 16k_es: 西班牙语；<br>- 16k_hi: 印地语；<br>- 16k_fr：法语；<br>- 16k_de：德语；<br>示例值：16k_zh</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >voice_id</td>

<td rowspan="1" colSpan="1" >是</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >音频流全局唯一标识，一个 WebSocket 连接对应一个，用户自己生成（推荐使用 UUID），最长128位。<br><br><strong>注意：</strong>每次建立 WebSocket 连接都必须重新生成 voice_id，任何情况下中断了识别或者 WebSocket 连接（如连接失败、正常结束、识别返回错误码等），voice_id 作废，重新发起识别必须使用新的 voice_id<br>示例值：TBgVFCsxdn3i2DGt</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >voice_format</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Int</td>

<td rowspan="1" colSpan="1" >语音编码方式，可选，默认值为4。1：pcm；4：speex(sp)；6：silk；8：mp3；10：opus（<a href="https://cloud.tencent.com/document/product/1093/48982#0ae3d1ce-bd85-48a9-9fbc-51979f9b4c49">opus 格式音频流封装说明</a>）；12：wav；14：m4a（每个分片须是一个完整的 m4a 音频）；16：aac<br>示例值：1</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >needvad</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >0：关闭 vad，1：开启 vad，默认为0。<br>为保证识别效果，如果语音分片长度超过60秒，会强制在60s 断一次，建议客户音频超过60s 时，开启 vad（人声检测切分功能），提升切分效果。<br>示例值：1</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >hotword_id</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >热词表 id。如不设置该参数，自动生效默认热词表；如果设置了该参数，那么将生效对应的热词表。<br>示例值：da3f5f5555cf11eda6da525400aec391</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >customization_id</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >自学习模型 id。如设置了该参数，将生效对应的自学习模型。<br>示例值：39bc8e96504511edbd76446a2eb5fd98</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >filter_dirty</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >是否过滤脏词（目前支持中文普通话引擎）。默认为0。0：不过滤脏词；1：过滤脏词；2：将脏词替换为“ * ”<br>示例值：0</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >filter_modal</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >是否过滤语气词（目前支持中文普通话引擎）。默认为0。0：不过滤语气词；1：部分过滤；2：严格过滤。<br>示例值：0</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >filter_punc</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >是否过滤句末的句号（目前支持中文普通话引擎）。默认为0。0：不过滤句末的句号；1：过滤句末的句号。<br>示例值：0</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >filter_empty_result</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >是否回调识别空结果，默认为1。0：回调空结果；1：不回调空结果。<br><br><strong>注意：</strong>如果需要 slice_type=0 和 slice_type=2 配对回调，需要设置 filter_empty_result=0。一般在外呼场景需要配对返回，通过 slice_type=0 来判断是否有人声出现。<br>示例值：1</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >convert_num_mode</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >是否进行阿拉伯数字智能转换（目前支持中文普通话引擎）。0：不转换，直接输出中文数字，1：根据场景智能转换为阿拉伯数字，3: 打开数学相关数字转换。默认值为1<br>示例值：1</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >word_info</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Int</td>

<td rowspan="1" colSpan="1" >是否显示词级别时间戳。0：不显示；1：显示，不包含标点时间戳；2：显示，包含标点时间戳；支持引擎 8k_en、8k_zh、8k_zh_finance、16k_zh_en、16k_zh、16k_en、16k_ca、16k_zh-TW、16k_ja、8k_zh_large，默认为0<br>100：显示字/词级别的时间戳，仅适用于16k_zh_en<br>示例值：0</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >vad_silence_time</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >语音断句检测阈值，静音时长超过该阈值会被认为断句（多用在智能客服场景，需配合 needvad = 1 使用），取值范围：500-2000（默认1000），单位 ms，此参数建议不要随意调整，可能会影响识别效果，目前支持 8k_zh、8k_zh_finance、16k_zh、16k_zh_en、8k_zh_large、16k_en、16k_en_large 引擎<br>示例值：1000</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >max_speak_time</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >强制断句功能，取值范围 5000-90000（单位:毫秒），默认值60000。 在连续说话不间断情况下，该参数将实现强制断句（此时结果变成稳态，slice_type=2）。如：游戏解说场景，解说员持续不间断解说，无法断句的情况下，将此参数设置为10000，则将在每10秒收到 slice_type=2 的回调。 （目前仅支持8k_zh、16k_zh、16k_zh_en、8k_zh_large 引擎）<br>示例值：60000</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >noise_threshold</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Float</td>

<td rowspan="1" colSpan="1" >噪音参数阈值，默认为0，取值范围：[-2,2]，对于一些音频片段，取值越大，判定为噪音情况越大。取值越小，判定为人声情况越大。<br><strong>慎用：可能影响识别效果</strong><br>示例值：0</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >signature</td>

<td rowspan="1" colSpan="1" >是</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >接口签名参数<br>示例值：*****g1JfeBi%2FYnTjyjekxfDA%3D</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >hotword_list</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >临时热词表：该参数用于提升识别准确率。<br>- 单个热词限制："热词\|权重"，单个热词不超过30个字符（最多10个汉字），权重[1-11]或者100，如：“腾讯云\|5” 或 “ASR\|11”。<br>- 临时热词表限制：多个热词用英文逗号分割，最多支持128个热词，如：“腾讯云\|10,语音识别\|5,ASR\|11”。<br>- 参数 hotword_id（热词表） 与 hotword_list（临时热词表） 区别：<br>  - hotword_id：热词表。需要先在控制台或接口创建热词表，获得对应 hotword_id 传入参数来使用热词功能。<br>  - hotword_list：临时热词表。每次请求时直接传入临时热词表来使用热词功能，云端不保留临时热词表。适用于有极大量热词需求的用户。<br><strong>注意：</strong><br>- 如果同时传入了 hotword_id 和 hotword_list，只有 hotword_list 生效。<br>- 热词权重设置为11时，当前热词将升级为超级热词，建议仅将重要且必须生效的热词设置到11，设置过多权重为11的热词将影响整体字准率。<br>- 热词权重设置为100时，当前热词开启热词增强同音同调替换功能（仅支持 8k_zh,16k_zh），举例：热词配置“蜜制\|100”时，与“蜜制”同拼音（mizhi）的“秘制”的识别结果会被强制替换成“蜜制”。因此建议客户根据自己的实际情况开启该功能。建议仅将重要且必须生效的热词设置到100，设置过多权重为100的热词将影响整体字准率。<br>- 热词不能包含空格，如：ASR 腾讯云<br>示例值：语音助理\|10</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >input_sample_rate</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" >支持 PCM 格式的 8k 音频在与引擎采样率不匹配的情况下升采样到 16k 后识别，能有效提升识别准确率。仅支持：8000。如：传入 8000 ，则 PCM 音频采样率为8k，当引擎选用16k_zh， 那么该8k 采样率的 PCM 音频可以在16k_zh 引擎下正常识别。<br><br><strong>注意：</strong>此参数仅适用于 PCM 格式音频，不传入值将维持默认状态，即默认调用的引擎采样率等于 PCM 音频采样率。<br>示例值：8000</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >emotion_recognition</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >Integer</td>

<td rowspan="1" colSpan="1" ><strong>增值付费功能</strong>情绪识别能力（目前仅支持 16k_zh , 16k_zh_en , 8k_zh , 8k_zh_large）<br>0：不开启。<br>1：开启情绪识别，但不在文本展示情绪标签。<br>2：开启情绪识别，并且在文本展示情绪标签。<br>默认值为0<br>支持的情绪分类为：高兴、伤心、愤怒。<br><br><strong>注意：</strong><br>1. 本功能为增值服务，需将参数设置为1或2时方可按对应方式生效。<br>2. 如果传入参数值1或2，需确保账号已购买情绪识别资源包，或账号开启后付费；若当前账号已开启后付费功能，并传入参数值1或2，将自动计费。<br>3. 参数设置为0时，无需购买资源包，也不会消耗情绪识别对应资源。<br>4. 使用情绪识别需要开启 vad 即 needvad 参数需要设置为1。<br>示例值：0</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >replace_text_id</td>

<td rowspan="1" colSpan="1" >否</td>

<td rowspan="1" colSpan="1" >String</td>

<td rowspan="1" colSpan="1" >替换词汇表 ID,  适用于热词和自学习场景也无法解决的极端 case 词组，会对识别结果强制替换。具体可参考（<a href="https://console.cloud.tencent.com/asr/replaceword">词汇替换</a>） ；强制替换功能可能会影响正常识别结果，请谨慎使用。<br><br><strong>注意：</strong><br>本功能配置完成后，预计在10分钟后生效。</td>
</tr>
</table>




**signature 签名生成**
5. 对除 signature 之外的所有参数按字典序进行排序，拼接请求 URL （不包含协议部分：wss://）作为签名原文，这里以 `appid=125922***`，`secretid=*****Qq1zhZMN8dv0******` 为例拼接签名原文，则拼接的签名原文为：

   ``` bash
   asr.cloud.tencent.com/asr/v2/125922***?engine_model_type=16k_zh&expired=1673494772&needvad=1&nonce=1673408372&secretid=*****Qq1zhZMN8dv0******&timestamp=1673408372&voice_format=1&voice_id=c64385ee-3e5c-4fc5-bbfd-7c71addb35b0
   ```
6. 对签名原文使用 SecretKey 进行 HMAC-SHA1 加密，之后再进行 base64 编码。例如对上一步的签名原文， `secretkey=*****SkqpeHgqmSz*****`，使用 HMAC-SHA1 算法进行加密并做 base64 编码处理：

   ``` bash
   Base64Encode(HmacSha1("asr.cloud.tencent.com/asr/v2/125922***?engine_model_type=16k_zh&expired=1673494772&needvad=1&nonce=1673408372&secretid=*****Qq1zhZMN8dv0******&timestamp=1673408372&voice_format=1&voice_id=c64385ee-3e5c-4fc5-bbfd-7c71addb35b0", "*****SkqpeHgqmSz*****"))
   ```

   得到 signature 签名值为：

   ``` bash
   G8jDQBRg1JfeBi/YnTjyjekxfDA=
   ```
7. 将 signature 值进行 **urlencode（必须进行 URL 编码，编码函数必须要支持对+、=等特殊字符的编码，否则将导致鉴权失败偶发）**后拼接得到最终请求 URL 为：

   ``` bash
   wss://asr.cloud.tencent.com/asr/v2/125922****?engine_model_type=16k_zh&expired=1592380492&filter_dirty=1&filter_modal=1&filter_punc=1&needvad=1&nonce=1592294092123&secretid=A*********************************0r&timestamp=1592294092&voice_format=1&voice_id=c64385ee-3e5c-4fc5-bbfd-7c71addb35b0&signature=G8jDQBRg1JfeBi%2FYnTjyjekxfDA%3D
   ```

#### Opus 音频流封装说明  

压缩 FrameSize 固定640，即一次压缩640 short，否则解压会失败。传到服务端可以是多帧的拼接组合，每一帧需满足下面格式。

每一帧压缩数据封装如下：

|OpusHead（4字节）|帧数据长度（2字节）|Opus 一帧压缩数据|
|---------|---------|---------|
|opus|长度 len|对应 len 长的 Opus encode data|


#### 请求响应

客户端发起连接请求后，后台建立连接并进行签名校验，校验成功则返回 code 值为0的确认消息表示握手成功；如果校验失败，后台返回 code 为非0值的消息并断开连接。
``` bash
 {"code":0,"message":"success","voice_id":"RnKu9FODFHK5FPpsrN"}
```

### 识别阶段

握手成功之后，进入识别阶段，客户端上传语音数据并接收识别结果消息。

#### 上传数据

在识别过程中，客户端持续上传 binary message 到后台，内容为音频流二进制数据。建议每 200ms 发送 200ms 时长（即1:1实时率）的数据包，对应 PCM 大小为：8k 采样率3200字节，16k 采样率6400字节。音频发送速率过快超过1:1实时率或者音频数据包之间发送间隔超过6秒，可能导致引擎出错，后台将返回错误并主动断开连接。

音频流上传完成之后，客户端需发送以下内容的 text message，通知后台结束识别。
``` bash
{"type": "end"}
```

#### 接收消息

客户端上传数据的过程中，需要同步接收后台返回的实时识别结果，结果示例：
``` bash
 {"code":0,"message":"success","voice_id":"RnKu9FODFHK5FPpsrN","message_id":"RnKu9FODFHK5FPpsrN_11_0","result":{"slice_type":0,"index":0,"start_time":0,"end_time":1240,"voice_text_str":"实时","word_size":0,"word_list":[]}}
```
``` bash
{"code":0,"message":"success","voice_id":"RnKu9FODFHK5FPpsrN","message_id":"RnKu9FODFHK5FPpsrN_33_0","result":{"slice_type":2,"index":0,"start_time":0,"end_time":2840,"voice_text_str":"实时语音识别","word_size":0,"word_list":[]}}
```

后台识别完所有上传的语音数据之后，最终返回 final 值为1的消息并断开连接。
``` bash
{"code":0,"message":"success","voice_id":"CzhjnqBkv8lk5pRUxhpX","message_id":"CzhjnqBkv8lk5pRUxhpX_241","final":1}
```

识别过程中如果出现错误，后台返回 code 为非0值的消息并断开连接。
``` bash
{"code":4008,"message":"后台识别服务器音频分片等待超时","voice_id":"CzhjnqBkv8lk5pRUxhpX","message_id":"CzhjnqBkv8lk5pRUxhpX_241"}
```

## 开发者资源

### SDK
- Tencent Cloud Speech SDK for Go:[GitHub](https://github.com/TencentCloud/tencentcloud-speech-sdk-go),[CNB](https://cnb.cool/tencent/cloud/speech/tencentcloud-speech-sdk-go)

- Tencent Cloud Speech SDK for Java:[GitHub](https://github.com/TencentCloud/tencentcloud-speech-sdk-java),[CNB](https://cnb.cool/tencent/cloud/speech/tencentcloud-speech-sdk-java)

- Tencent Cloud Speech SDK for C++:[GitHub](https://github.com/TencentCloud/tencentcloud-speech-sdk-cpp),[CNB](https://cnb.cool/tencent/cloud/speech/tencentcloud-speech-sdk-cpp)

- Tencent Cloud Speech SDK for Python:[GitHub](https://github.com/TencentCloud/tencentcloud-speech-sdk-python),[CNB](https://cnb.cool/tencent/cloud/speech/tencentcloud-speech-sdk-python)

- Tencent Cloud Speech SDK for JS:[GitHub](https://github.com/TencentCloud/tencentcloud-speech-sdk-js),[CNB](https://cnb.cool/tencent/cloud/speech/tencentcloud-speech-sdk-js)

- Tencent Cloud Speech SDK for C#:[GitHub](https://github.com/TencentCloud/tencentcloud-speech-sdk-dotnet),[CNB](https://cnb.cool/tencent/cloud/speech/tencentcloud-speech-sdk-dotnet)


### SDK 调用示例
- [Golang 示例](https://github.com/TencentCloud/tencentcloud-speech-sdk-go/blob/master/examples/asrexample/asrexample.go)

- [Java 示例](https://github.com/TencentCloud/tencentcloud-speech-sdk-java-example/blob/main/src/main/java/com/tencentcloud/asrv2/RtDemo.java)

- [C++ 示例](https://github.com/TencentCloud/tencentcloud-speech-sdk-cpp/blob/master/examples/asr_example.cpp)

- [Python 示例](https://github.com/TencentCloud/tencentcloud-speech-sdk-python/blob/master/examples/asr/asrexample.py)

- [JS 示例](https://github.com/TencentCloud/tencentcloud-speech-sdk-js/tree/main/asr/examples)

- [C# 示例](https://github.com/TencentCloud/tencentcloud-speech-sdk-dotnet/tree/main/examples/realtime-asr)


## 错误码
<table>
<tr>
<td rowspan="1" colSpan="1" >数值</td>

<td rowspan="1" colSpan="1" >说明</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4000</td>

<td rowspan="1" colSpan="1" >音频数据发送过多，请1秒内最多发送3秒音频数据。<br><strong>注意：</strong>实时识别的效果是“边说边出文字”，1秒内发送的音频数据总时长应为1秒。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4001</td>

<td rowspan="1" colSpan="1" >参数不合法，具体详情参考 message。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4002</td>

<td rowspan="1" colSpan="1" >鉴权失败。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4003</td>

<td rowspan="1" colSpan="1" >AppID 服务未开通，请在控制台开通服务。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4004</td>

<td rowspan="1" colSpan="1" >资源包耗尽，请开通后付费或者购买资源包。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4005</td>

<td rowspan="1" colSpan="1" >账户欠费停止服务，请及时充值。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4006</td>

<td rowspan="1" colSpan="1" >账号当前调用并发超限。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4007</td>

<td rowspan="1" colSpan="1" >音频解码失败，请检查上传音频数据格式与调用参数一致。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4008</td>

<td rowspan="1" colSpan="1" >客户端超过15秒未发送音频数据。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4009</td>

<td rowspan="1" colSpan="1" >客户端连接断开。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >4010</td>

<td rowspan="1" colSpan="1" >客户端上传未知文本消息。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >5000</td>

<td rowspan="1" colSpan="1" >因机器负载过高、网络抖动等导致失败，请重新发起新识别。<br><strong>注意：</strong>该问题通常为偶发，少量出现可忽略，发起新识别即可。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >5001</td>

<td rowspan="1" colSpan="1" >因机器负载过高、网络抖动等导致失败，请重新发起新识别。<br><strong>注意：</strong>该问题通常为偶发，少量出现可忽略，发起新识别即可。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >5002</td>

<td rowspan="1" colSpan="1" >因机器负载过高、网络抖动等导致失败，请重新发起新识别。<br><strong>注意：</strong>该问题通常为偶发，少量出现可忽略，发起新识别即可。</td>
</tr>

<tr>
<td rowspan="1" colSpan="1" >6001</td>

<td rowspan="1" colSpan="1" >境外调用请前往 <a href="https://www.tencentcloud.com/zh/products/asr">腾讯云国际站</a> 开通服务。国内站用户请检查是否使用境外代理，如果使用请关闭。</td>
</tr>
</table>
