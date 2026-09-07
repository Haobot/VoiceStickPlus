"""AI 改写器单测：规则版（本地必选）+ 混元适配器凭据/降级路径。

LLM 真调用是外部边界（不 mock 不伪造），单测只覆盖规则与降级逻辑。
"""
import pytest

from p1.rewrite.rewriter import (
    RewriteResult,
    RuleRewriter,
    build_rewriter,
)


# ---------- 规则版：去口癖 ----------

def test_去孤立口癖():
    r = RuleRewriter()
    out = r.rewrite("嗯，今天天气不错")
    assert out.text == "今天天气不错。"
    assert out.changed is True


def test_口癖句中独立出现也去除():
    out = RuleRewriter().rewrite("我们嗯，去开会")
    assert out.text == "我们去开会。"


def test_不误伤正常语义词():
    # 「那个」是正常指示词，不能因为口癖规则被删
    out = RuleRewriter().rewrite("那个方案不错")
    assert out.text == "那个方案不错。"
    assert out.changed is True  # 补句号也算 changed


def test_叠词口癖那个那个折叠():
    out = RuleRewriter().rewrite("我那个那个再补充一点")
    assert "那个那个" not in out.text
    assert out.text == "我那个再补充一点。"


# ---------- 规则版：重复折叠与标点 ----------

def test_叠字折叠():
    out = RuleRewriter().rewrite("我我我们开始吧")
    assert out.text == "我们开始吧。"


def test_句尾补句号():
    out = RuleRewriter().rewrite("今天天气不错")
    assert out.text == "今天天气不错。"


def test_已有句尾标点不重复():
    out = RuleRewriter().rewrite("已经结束了。")
    assert out.text == "已经结束了。"
    assert out.changed is False


def test_双标点去重():
    out = RuleRewriter().rewrite("好的。。")
    assert out.text == "好的。"


def test_空文本与纯标点原样():
    r = RuleRewriter()
    assert r.rewrite("").text == ""
    assert r.rewrite("。").text == "。"


def test_多规则叠加():
    out = RuleRewriter().rewrite("嗯，我我我们今天那个那个开会")
    assert out.text == "我们今天那个开会。"


# ---------- 工厂与降级 ----------

def test_工厂_rules直接返回规则版():
    r = build_rewriter(provider="rules")
    assert isinstance(r, RuleRewriter)


def test_工厂_混元无凭据自动降级规则版():
    r = build_rewriter(provider="tencent_hunyuan", credentials=None)
    assert isinstance(r, RuleRewriter)


def test_工厂_未知provider报错():
    with pytest.raises(ValueError, match="未知改写"):
        build_rewriter(provider="gpt999")


def test_RewriteResult_默认字段():
    res = RewriteResult(text="你好。", changed=False, engine="rules")
    assert res.corrections == []
