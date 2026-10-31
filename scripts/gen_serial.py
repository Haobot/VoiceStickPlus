#!/usr/bin/env python3
"""VoiceStick 离线授权串码发卡器（零第三方依赖）。

用法:
  python scripts/gen_serial.py gen-key --out scripts/license_private_key.hex
  python scripts/gen_serial.py show-pubkey --key-file scripts/license_private_key.hex
  python scripts/gen_serial.py sign --key-file ... --device-id AB12 --machine-guid "{...}"
      --edition 1 --expiry 2027-09-13 --counter 42
  python scripts/gen_serial.py test-vectors --key-file ... --out scripts/license_test_vectors.json
私钥文件绝不可提交（.gitignore 已排除）。

签名算法：Monocypher 4.0.2 的 EdDSA（curve25519 + BLAKE2b-512，即 Ed25519 结构
替换哈希函数），与 desktop/windows/third_party/monocypher 的 crypto_eddsa_check
逐字节互操作（见 Doc/Plan/offline-license-activation.md）。
"""
import argparse, hashlib, json, os, sys
from datetime import date

# ---- 纯 Python EdDSA（Monocypher 变体：BLAKE2b-512 替代 SHA-512）----
_q = 2**255 - 19
_l = 2**252 + 27742317777372353535851937790883648493

def _H(m): return hashlib.blake2b(m, digest_size=64).digest()
def _expmod(b, e, m):
    if e == 0: return 1
    t = _expmod(b, e >> 1, m) ** 2 % m
    return t * b % m if e & 1 else t
def _inv(x): return _expmod(x, _q - 2, _q)
_d = -121665 * _inv(121666) % _q
_I = _expmod(2, (_q - 1) // 4, _q)
def _xrecover(y):
    xx = (y * y - 1) * _inv(_d * y * y + 1)
    x = _expmod(xx, (_q + 3) // 8, _q)
    if (x * x - xx) % _q != 0: x = x * _I % _q
    if x % 2 != 0: x = _q - x
    return x
_By = 4 * _inv(5) % _q
_B = [_xrecover(_By), _By]
def _edwards(P, Q):
    x1, y1 = P; x2, y2 = Q
    common = _d * x1 * x2 * y1 * y2
    x3 = (x1 * y2 + x2 * y1) * _inv(1 + common)
    y3 = (y1 * y2 + x1 * x2) * _inv(1 - common)
    return [x3 % _q, y3 % _q]
def _scalarmult(P, e):
    if e == 0: return [0, 1]
    Q = _scalarmult(P, e >> 1)
    Q = _edwards(Q, Q)
    return _edwards(Q, P) if e & 1 else Q
def _encodeint(y): return y.to_bytes(32, "little")
def _encodepoint(P):
    x, y = P
    bits = bytearray(_encodeint(y)); bits[31] |= (x & 1) << 7
    return bytes(bits)
def _bit(h, i): return (h[i // 8] >> (i % 8)) & 1
def _secret_scalar(h):
    return 2**254 + sum(2**i * _bit(h, i) for i in range(3, 254))
def publickey(sk):
    return _encodepoint(_scalarmult(_B, _secret_scalar(_H(sk))))
def signature(m, sk, pk):
    h = _H(sk)
    a = _secret_scalar(h)
    r = int.from_bytes(_H(h[32:64] + m), "little") % _l
    R = _encodepoint(_scalarmult(_B, r))
    S = (r + int.from_bytes(_H(R + pk + m), "little") * a) % _l
    return R + _encodeint(S)

# ---- 串码格式（与 desktop/windows/src/license.cc 完全一致）----
EDITION_ANNUAL, EDITION_PERPETUAL = 1, 2
EPOCH = 0x4FE6  # days_from_civil(2026,1,1) = date(2026,1,1) - date(1970,1,1) = 20454
PAYLOAD_LEN, SIG_LEN, SERIAL_BYTES = 15, 64, 79
ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
def _b32encode(data: bytes) -> str:
    out, buf, bits = [], 0, 0
    for byte in data:
        buf = (buf << 8) | byte; bits += 8
        while bits >= 5:
            bits -= 5; out.append(ALPHABET[(buf >> bits) & 0x1F])
    if bits: out.append(ALPHABET[(buf << (5 - bits)) & 0x1F])
    return "".join(out)
def binding_hash(device_id: str, machine_guid: str) -> bytes:
    return hashlib.sha256((device_id + "\n" + machine_guid).encode()).digest()[:8]
def build_payload(edition: int, device_id: str, machine_guid: str,
                  expiry_days: int, counter: int) -> bytes:
    return (bytes([edition]) + binding_hash(device_id, machine_guid)
            + expiry_days.to_bytes(2, "little") + counter.to_bytes(4, "little"))
def days_since_epoch(y, m, d):
    return (date(y, m, d) - date(1970, 1, 1)).days - EPOCH
def group(s: str) -> str:  # 5 字符一组
    return "-".join(s[i:i+5] for i in range(0, len(s), 5))

def cmd_gen_key(a):
    sk = os.urandom(32)
    with open(a.out, "w") as f: f.write(sk.hex() + "\n")
    os.chmod(a.out, 0o600)
    print("private key written:", a.out); print("public key:", publickey(sk).hex())
def cmd_show_pubkey(a):
    print("public key:", _load_key(a.key_file).hex())
def _load_key(path):
    with open(path) as f: return bytes.fromhex(f.read().strip())
def cmd_sign(a):
    sk = _load_key(a.key_file); pk = publickey(sk)
    exp = 0xFFFF if a.edition == EDITION_PERPETUAL else days_since_epoch(*map(int, a.expiry.split("-")))
    payload = build_payload(a.edition, a.device_id.upper(), a.machine_guid.lower(), exp, a.counter)
    serial = _b32encode(payload + signature(payload, sk, pk))
    print(group(serial))
def cmd_test_vectors(a):
    sk = _load_key(a.key_file); pk = publickey(sk)
    vecs = []
    for i, (dev, guid, edition, expiry, cnt) in enumerate([
            ("AB12", "{11111111-2222-3333-4444-555555555555}", 1, "2027-01-01", 1),
            ("CD34", "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}", 2, None, 2),
            ("00FF", "no-braces-guid", 1, "2026-12-31", 3)]):
        exp = 0xFFFF if expiry is None else days_since_epoch(*map(int, expiry.split("-")))
        payload = build_payload(edition, dev, guid.lower(), exp, cnt)
        vecs.append({"device_id": dev, "machine_guid": guid.lower(), "edition": edition,
                     "expiry_days": exp, "counter": cnt,
                     "payload_hex": payload.hex(), "serial": group(_b32encode(payload + signature(payload, sk, pk)))})
    with open(a.out, "w") as f: json.dump({"public_key_hex": pk.hex(), "vectors": vecs}, f, indent=2)
    print("vectors written:", a.out)

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen-key"); g.add_argument("--out", required=True); g.set_defaults(fn=cmd_gen_key)
    s = sub.add_parser("show-pubkey"); s.add_argument("--key-file", required=True); s.set_defaults(fn=cmd_show_pubkey)
    s = sub.add_parser("sign"); s.add_argument("--key-file", required=True)
    s.add_argument("--device-id", required=True); s.add_argument("--machine-guid", required=True)
    s.add_argument("--edition", type=int, choices=[1, 2], required=True)
    s.add_argument("--expiry", help="YYYY-MM-DD（edition=1 必填，edition=2 忽略）")
    s.add_argument("--counter", type=int, required=True); s.set_defaults(fn=cmd_sign)
    t = sub.add_parser("test-vectors"); t.add_argument("--key-file", required=True); t.add_argument("--out", required=True)
    t.set_defaults(fn=cmd_test_vectors)
    a = p.parse_args()
    if getattr(a, "edition", 0) == EDITION_ANNUAL and not getattr(a, "expiry", None):
        p.error("--expiry required for edition=1")
    a.fn(a)
