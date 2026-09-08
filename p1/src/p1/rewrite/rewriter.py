"""AI 改写：规则版（本地必选，离线可跑）+ 腾讯混元 LLM 版（可选，无凭据降级）。

规则版的误伤边界（有意保守）：
- 叠字折叠只动 3 连以上（正常叠词「谢谢」最多 2 连不受影响）与
  虚词 2 连（的/了/是/在/有/就，正常中文无 2 连用法）；
- 双字词重复只折叠白名单口癖（那个/然后）——「研究研究」是正常强调；
- 语义级自修正（"不对说错了重新说"截断）属 LLM 版职责，规则版不做。
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

# 孤立口癖（后跟逗号才算停顿填充，句尾语气词不动）
_FILLER = re.compile(r"(?:嗯|呃|啊)[，,]")
# 任意字符 3 连折叠（口吃），正常叠词不受影响
_TRIPLE = re.compile(r"(.)\1{2,}")
# 虚词 2 连折叠（正常中文没有这些 2 连）
_FUNC_DOUBLE = re.compile(r"(的|了|是|在|有|就)\1+")
# 双字口癖叠用折叠白名单
_WORD_DOUBLE = re.compile(r"(那个|然后)\1+")
# 相同标点连打
_PUNCT_DOUBLE = re.compile(r"([。！？，、])\1+")
_SENT_END = "。！？.!?…"


@dataclass
class RewriteResult:
    """改写结果：changed 供悬浮条反馈，corrections 记录每条规则命中。"""

    text: str
    changed: bool
    engine: str
    corrections: list[str] = field(default_factory=list)


class RuleRewriter:
    """本地规则改写（去口癖 / 折叠 / 标点兜底）。"""

    engine = "rules"

    def rewrite(self, text: str) -> RewriteResult:
        corrections: list[str] = []

        def apply(pattern: re.Pattern, repl: str, label: str, src: str) -> str:
            out = pattern.sub(repl, src)
            if out != src:
                corrections.append(label)
            return out

        text = apply(_FILLER, "", "去口癖", text)
        text = apply(_TRIPLE, r"\1", "叠字折叠", text)
        text = apply(_FUNC_DOUBLE, r"\1", "虚词去重", text)
        text = apply(_WORD_DOUBLE, r"\1", "口癖叠用折叠", text)
        text = apply(_PUNCT_DOUBLE, r"\1", "标点去重", text)
        if text and text[-1] not in _SENT_END:
            text += "。"
            corrections.append("句尾补标点")
        return RewriteResult(text=text, changed=bool(corrections),
                             engine=self.engine, corrections=corrections)


class HunyuanRewriter:
    """腾讯混元 LLM 改写（去赘词 / 自修正 / 标点），凭据就绪才可用。

    真实调用走网络，不做单测 mock（仓库红线：不伪造链路），验收靠真机。
    """

    engine = "tencent_hunyuan"
    PROMPT = (
        "你是语音输入的文本清理器。去掉口癖填充词（嗯/呃/那个那个）、"
        "修正明显的口误重复、整理标点；不要增删实义内容，不要翻译，"
        "直接输出清理后的一句话，不要任何解释。\n原文："
    )

    def __init__(self, credentials):
        if not credentials:
            raise ValueError("混元改写需要凭据")
        self._credentials = credentials

    def rewrite(self, text: str) -> RewriteResult:
        from tencentcloud.common import credential
        from tencentcloud.hunyuan.v20230901 import hunyuan_client, models

        cred = credential.Credential(self._credentials.secret_id,
                                     self._credentials.secret_key)
        client = hunyuan_client.HunyuanClient(cred, self._credentials.region)
        req = models.ChatCompletionsRequest()
        req.Model = "hunyuan-turbos-latest"
        req.Messages = [{"Role": "user", "Content": self.PROMPT + text}]
        resp = client.ChatCompletions(req)
        cleaned = resp.Choices[0].Message.Content.strip()
        return RewriteResult(text=cleaned or text,
                             changed=cleaned != text, engine=self.engine)


def build_rewriter(provider: str, credentials=None):
    """工厂：rules 直接用；混元无凭据自动降级规则版（不静默伪造 LLM 结果）。"""
    if provider == "rules":
        return RuleRewriter()
    if provider == "tencent_hunyuan":
        if credentials is None:
            return RuleRewriter()  # 降级：离线/无凭据环境保持可用
        return HunyuanRewriter(credentials)
    raise ValueError(f"未知改写 provider: {provider}")
