"""把本地调试 ogg（Ogg Opus）按腾讯实时 ASR 协议回放，验证服务端识别行为。

用法：python replay_tencent_asr.py <file.ogg> [--realtime]

复刻 desktop/windows/src/asr_client_tencent.cc 的行为：
- 按 Ogg 段表（lacing）拆出单个 Opus 包（跳过 OpusHead/OpusTags），
  对 ffmpeg 生成的「一页多包」Ogg 也适用（复用 asr_bench.wsproto 的实现），封装为
  "opus"（4 字节）+ 长度（2 字节，小端，与桌面端 ExtractTencentOpusFrame 一致）+ Opus 帧；
- 签名 URL：HMAC-SHA1(secret_key, sign_str) -> base64 -> urlencode；
- 发送二进制帧后收 JSON；最后发 {"type":"end"}，等 final=1。
"""
import argparse
import base64
import hashlib
import hmac
import json
import random
import sys
import time
import urllib.parse
import uuid

import websocket

# 复用 asr_bench 的按段表（lacing）拆包实现；保证脚本目录在 sys.path 中
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from asr_bench.wsproto import demux_ogg_packets  # noqa: E402

SECRET_ID = "AKIDCIDwShqnaDgz86tWzGNPJ5h8MH6lFalw"
SECRET_KEY = "FLH1Zt31nMkLqAIDwT2OYXLyRAkDYiio"
APPID = "1259040144"
ENGINE = "16k_zh_en"


def build_signed_url(voice_id, needvad="1", voice_format="10", engine=ENGINE):
    params = {
        "secretid": SECRET_ID,
        "timestamp": str(int(time.time())),
        "expired": str(int(time.time()) + 86400),
        "nonce": str(random.randint(1, 2**31 - 1)),
        "engine_model_type": engine,
        "voice_format": voice_format,
        "needvad": needvad,
        "voice_id": voice_id,
    }
    query = "&".join(f"{k}={params[k]}" for k in sorted(params))
    sign_str = f"asr.cloud.tencent.com/asr/v2/{APPID}?{query}"
    sig = base64.b64encode(
        hmac.new(SECRET_KEY.encode(), sign_str.encode(), hashlib.sha1).digest()
    ).decode()
    return f"wss://{sign_str}&signature={urllib.parse.quote(sig, safe='')}"


def wrap_frame(packet, big_endian=False):
    # ExtractTencentOpusFrame 写的是小端；腾讯文档注释为大端。两种都试。
    if big_endian:
        return b"opus" + bytes([(len(packet) >> 8) & 0xFF, len(packet) & 0xFF]) + packet
    return b"opus" + bytes([len(packet) & 0xFF, (len(packet) >> 8) & 0xFF]) + packet


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ogg")
    ap.add_argument("--realtime", action="store_true", help="按 40ms/帧 实时速率发送")
    ap.add_argument("--big-endian", action="store_true", help="长度字段按大端封装")
    ap.add_argument("--novad", action="store_true", help="needvad=0")
    ap.add_argument("--pcm", action="store_true", help="先 ffmpeg 解码为 16k PCM 再按 voice_format=1 发送")
    ap.add_argument("--engine", default=None, help="覆盖 engine_model_type")
    args = ap.parse_args()

    if args.pcm:
        import subprocess
        pcm = subprocess.run(
            ["ffmpeg", "-v", "error", "-i", args.ogg, "-af", "aresample=16000",
             "-f", "s16le", "-ac", "1", "-"],
            capture_output=True, check=True).stdout
        frames = [pcm[i:i + 1280] for i in range(0, len(pcm) - 1279, 1280)]  # 40ms @16k
        print(f"decoded {len(frames)} pcm frames (1280B) from {args.ogg}")
    else:
        frames = demux_ogg_packets(args.ogg)
        print(f"demuxed {len(frames)} opus frames from {args.ogg}")
    if not frames:
        return 1

    voice_id = str(uuid.uuid4())
    url = build_signed_url(voice_id, "0" if args.novad else "1", "1" if args.pcm else "10", args.engine or ENGINE)
    ws = websocket.create_connection(url, timeout=20)
    print("connected, voice_id =", voice_id)

    results = []
    final_text = []

    ws.settimeout(5)
    if not args.pcm:
        print("first frame head:", wrap_frame(frames[0], args.big_endian)[:8].hex(), "len=", len(wrap_frame(frames[0], args.big_endian)))
    t0 = time.time()
    for i, pkt in enumerate(frames):
        ws.send_binary(pkt if args.pcm else wrap_frame(pkt, args.big_endian))
        if args.realtime:
            time.sleep(0.04)
        # 非阻塞读已到的结果
        ws.settimeout(0.001)
        while True:
            try:
                msg = ws.recv()
            except Exception:
                break
            if isinstance(msg, str):
                print(f"[+{time.time()-t0:.2f}s after frame {i}]", msg[:160])
                results.append(msg)
        ws.settimeout(5)

    ws.send('{"type":"end"}')
    # 等 final=1
    deadline = time.time() + 15
    ws.settimeout(5)
    while time.time() < deadline:
        try:
            msg = ws.recv()
        except Exception:
            break
        if isinstance(msg, str):
            results.append(msg)
            try:
                if json.loads(msg).get("final") == 1:
                    break
            except Exception:
                pass
    ws.close()

    n_slices = 0
    for m in results:
        try:
            obj = json.loads(m)
        except Exception:
            continue
        if obj.get("code") != 0:
            print("ERROR:", m)
            continue
        r = obj.get("result")
        if r:
            n_slices += 1
            txt = r.get("voice_text_str", "")
            print(f"slice_type={r.get('slice_type')} start={r.get('start_time')} "
                  f"end={r.get('end_time')} text={txt!r}")
            if r.get("slice_type") == 2 and txt:
                final_text.append(txt)
        elif obj.get("final") == 1:
            print("final=1 message_id=", obj.get("message_id"))
    print(f"== slices: {n_slices}, accumulated: {''.join(final_text)!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
