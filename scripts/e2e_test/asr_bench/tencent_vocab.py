"""腾讯 ASR 热词表管理 API（纯 stdlib，TC3-HMAC-SHA256 签名）。

复刻 desktop/windows/src/tencent_asr_vocab_client.cc：
CreateAsrVocab / UpdateAsrVocab / GetAsrVocabList，
endpoint asr.tencentcloudapi.com，Version 2019-06-14。
评测中用于把词库同步进热词表通道（hotword_id）做对照实验。
"""
from __future__ import annotations

import hashlib
import hmac
import http.client
import json
import time

HOST = "asr.tencentcloudapi.com"
SERVICE = "asr"
VERSION = "2019-06-14"
REGION = "ap-guangzhou"
CONTENT_TYPE = "application/json; charset=utf-8"


class TencentVocabError(RuntimeError):
    pass


def _sha256_hex(data: str | bytes) -> str:
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def _hmac_sha256(key: bytes, msg: str) -> bytes:
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def _tc3_authorization(secret_id: str, secret_key: str, action: str,
                       payload: str, timestamp: int) -> str:
    date = time.strftime("%Y-%m-%d", time.gmtime(timestamp))
    credential_scope = f"{date}/{SERVICE}/tc3_request"
    canonical_headers = (
        f"content-type:{CONTENT_TYPE}\n"
        f"host:{HOST}\n"
        f"x-tc-action:{action.lower()}\n"
    )
    signed_headers = "content-type;host;x-tc-action"
    canonical_request = (
        f"POST\n/\n\n{canonical_headers}\n{signed_headers}\n{_sha256_hex(payload)}"
    )
    string_to_sign = (
        f"TC3-HMAC-SHA256\n{timestamp}\n{credential_scope}\n"
        f"{_sha256_hex(canonical_request)}"
    )
    secret_date = _hmac_sha256(("TC3" + secret_key).encode("utf-8"), date)
    secret_service = _hmac_sha256(secret_date, SERVICE)
    secret_signing = _hmac_sha256(secret_service, "tc3_request")
    signature = _hmac_sha256(secret_signing, string_to_sign).hex()
    return (f"TC3-HMAC-SHA256 Credential={secret_id}/{credential_scope}, "
            f"SignedHeaders={signed_headers}, Signature={signature}")


def call_api(action: str, payload: dict, *, secret_id: str, secret_key: str,
             timeout: float = 15.0) -> dict:
    """调用腾讯 ASR 管理 API，返回 Response 字典；业务错误抛 TencentVocabError。"""
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    timestamp = int(time.time())
    conn = http.client.HTTPSConnection(HOST, timeout=timeout)
    try:
        conn.request("POST", "/", body=body.encode("utf-8"), headers={
            "Authorization": _tc3_authorization(secret_id, secret_key, action,
                                                body, timestamp),
            "Content-Type": CONTENT_TYPE,
            "Host": HOST,
            "X-TC-Action": action,
            "X-TC-Timestamp": str(timestamp),
            "X-TC-Version": VERSION,
            "X-TC-Region": REGION,
        })
        resp = conn.getresponse()
        raw = resp.read().decode("utf-8", "replace")
    finally:
        conn.close()
    try:
        data = json.loads(raw).get("Response", {})
    except json.JSONDecodeError:
        raise TencentVocabError(f"{action}: HTTP {resp.status} 非 JSON 响应: {raw[:200]}")
    if "Error" in data:
        err = data["Error"]
        raise TencentVocabError(f"{action}: {err.get('Code')} {err.get('Message')}")
    return data


def find_vocab_id(name: str, *, secret_id: str, secret_key: str) -> str:
    resp = call_api("GetAsrVocabList", {"Limit": 30},
                    secret_id=secret_id, secret_key=secret_key)
    for item in resp.get("VocabList", []):
        if item.get("Name") == name:
            return item.get("VocabId", "")
    return ""


def sync_vocab(name: str, words: list[str], *, secret_id: str, secret_key: str,
               weight: int = 10, description: str = "") -> str:
    """把词表同步到指定名字的热词表（存在则更新，否则新建），返回 VocabId。

    words 需已按优先级裁剪（每表 ≤1000 词，词 ≤10 汉字/30 字符）。"""
    entries = [{"Word": w, "Weight": weight} for w in words[:1000]]
    vocab_id = find_vocab_id(name, secret_id=secret_id, secret_key=secret_key)
    if vocab_id:
        call_api("UpdateAsrVocab", {"VocabId": vocab_id, "WordWeights": entries},
                 secret_id=secret_id, secret_key=secret_key)
        return vocab_id
    payload: dict = {"Name": name, "WordWeights": entries}
    if description:
        payload["Description"] = description
    resp = call_api("CreateAsrVocab", payload, secret_id=secret_id,
                    secret_key=secret_key)
    return resp.get("VocabId", "")
