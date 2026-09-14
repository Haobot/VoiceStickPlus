# 离线授权激活（EdDSA 串码）设计与实施方案

- 日期：2026-09-13
- 状态：**已交付**（2026-09-15 凌晨；commits bfec04e8 → e20e49e2 + 后续修复）
- 范围：Windows 桌面端（本地离线模型仅 Windows 端存在）
- 关联：`Doc/Plan/local-model-distribution.md`（本地模型）、`Doc/Ref/desktop-config.md`（配置字段）

> **实施期修订（以此为准）**：
> 1. 签名算法为 monocypher 4.0.2 EdDSA（BLAKE2b-512），**非 RFC 8032 Ed25519**（monocypher 无 SHA-512 实现）；`scripts/gen_serial.py` 内嵌纯 Python 同款实现，互操作已由测试向量证明。
> 2. epoch 常量实际值 0x4FE6=20454（days_from_civil(2026,1,1)）。
> 3. `EvaluateLicense` 回拨语义：`remaining = min(anchor+30-last_seen, 宽限7天-回拨量)`（测试锁定）。
> 4. 修过一例真机事故：协调器 config 副本早于试用锚点写入，`SavePairedDeviceInfo` 全量落盘抹掉锚点 → 落盘前 `ReloadLicenseFromDisk`（`TestSavePairedDeviceInfoPreservesDiskLicense`）。
> 5. 观测：`license: status` 日志在启动与设备连接变化时记录（win32_app SetConnectedDevices 挂点）。

## 1. 设计摘要

对本地离线 ASR 启用「30 天试用 + 串码激活」授权。串码 = Ed25519 签名的自包含凭证，客户端内嵌公钥离线验签，全程无需联网（支付/发卡是一次性在线交易，由仓库外服务端完成）。

三处关键决策（相对最初设想的修正）：

1. **绑定键 = 设备ID + MachineGuid**，不用蓝牙 MAC（RPA 轮换、可伪造）。设备 ID 用既有归一化形式（去前缀 4 位大写 hex，`BleProtocol::NormalizeDeviceId`）；MachineGuid 读 `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid`。
2. **串码是签名不是查表**：payload 含绑定键哈希/到期日/流水号，Ed25519 签名，私钥只在发卡端。防整码共享：他人串码验签能过但绑定键不匹配。
3. **防时钟回拨**：持久化 last_seen_utc，回拨时按上次所见时间计 + 7 天宽限。

威胁模型：挡住拷贝/共享串码，不挡决心破解者（离线软件共识：门槛做到「共享不划算」即可）。

## 2. 串码格式

```text
payload (15 bytes):
  [0]      edition: 1=年费, 2=买断
  [1:9]    binding_hash = SHA-256( norm_device_id + "\n" + machine_guid_lower )[0:8]
  [9:11]   expiry_days uint16 LE = 自 2026-01-01 起的天数；0xFFFF = 买断
  [11:15]  counter uint32 LE（发卡流水号）
signature: Ed25519(payload)，64 bytes
串码: Crockford Base32(payload||sig)（79 bytes → 127 字符，定长无 padding），
      显示按 5 字符一组连字符分隔，输入时忽略大小写/连字符/空白
```

## 3. 文件结构

| 文件 | 责任 |
|---|---|
| `desktop/windows/third_party/monocypher/monocypher.c/.h` + `LICENSE` | Ed25519 实现（BSD-2，vendored） |
| `desktop/windows/src/serial_base32.h/.cc` | Crockford Base32 编解码（纯函数） |
| `desktop/windows/src/license.h/.cc` | payload 打包/解析、绑定键、验签、LicenseStatus 判定（纯函数，core） |
| `desktop/windows/src/license_public_key.h` | 内嵌 Ed25519 公钥 32 字节（发行端公钥，git 跟踪） |
| `scripts/gen_serial.py` | 纯 Python Ed25519 发卡器 + 测试向量生成（私钥不进仓库，见 §10） |
| `desktop/windows/src/machine_guid_win.h/.cc` | 读 MachineGuid（app shell，advapi32 已链接） |
| `desktop/windows/src/license_runtime.h/.cc` | shell 侧运行时：装配 device_ids+guid+config，试用锚点/last_seen 持久化（app shell） |
| `desktop/windows/src/app_config.h/.cc` | 新增 `[license]` section（3 字段） |
| `desktop/windows/src/voice_stick_coordinator.h/.cc` | `SetLicenseGate()` + 本地引擎闸口 |
| `desktop/windows/src/settings_dialog.h/.cc` | 「授权」分组 + 激活模态框 |
| `desktop/windows/src/localization.h/.cc` | 授权相关中英文案 |
| `desktop/windows/CMakeLists.txt` | monocypher 编入 core；core 无需新库（bcrypt/advapi32 已齐） |

分层：`serial_base32` + `license` 进 `voicestick_core`（无 UI/注册表依赖，可单测）；`machine_guid_win` + `license_runtime` 进 `VoiceStickApp` shell。

## 4. 实施任务

### Task 1: Vendor monocypher + CMake 接线

- [ ] 下载 monocypher 4.0.2（BSD-2-Clause）：`https://raw.githubusercontent.com/LoupVaillant/Monocypher/4.0.2/src/monocypher.c` 和 `monocypher.h` 与 `LICENSE` 放到 `desktop/windows/third_party/monocypher/`。
- [ ] `desktop/windows/CMakeLists.txt` voicestick_core 源列表（显式列举，:127 起）加一行 `src/license.cc` `src/serial_base32.cc` `third_party/monocypher/monocypher.c`；include dir 加 `third_party/monocypher`（照 cjson :162/:183 模式）。
- [ ] 验签 API：`crypto_sign_verify(const uint8_t sig[64], const uint8_t* msg, size_t msg_len, const uint8_t pub[32])`。
- [ ] `git add -f` 新文件（desktop/windows 被 gitignore，AGENTS.md 红线），`git ls-files` 逐个确认跟踪。

### Task 2: serial_base32 模块（TDD）

- [ ] 失败测试（tests/core_tests.cc，自研 assert 风格，照 :1328 先例），`TestSerialBase32RoundTrip`：
```cpp
void TestSerialBase32RoundTrip() {
    using namespace voicestick;
    // 全 0 / 全 1 / 递增模式 / 随机 79 字节往返
    for (int seed = 0; seed < 8; ++seed) {
        std::vector<std::uint8_t> data(79);
        std::uint8_t v = static_cast<std::uint8_t>(seed * 37);
        for (auto& b : data) { b = v; v = static_cast<std::uint8_t>(v * 131 + 17); }
        const std::string enc = SerialBase32Encode(data);
        assert(enc.size() == 127);
        const auto dec = SerialBase32Decode(enc);
        assert(dec.has_value() && *dec == data);
    }
    // 输入规范化：小写 + 连字符 + 空格应等价
    std::vector<std::uint8_t> data(79, 0xAB);
    std::string enc = SerialBase32Encode(data);
    std::string messy;
    for (size_t i = 0; i < enc.size(); ++i) {
        messy += static_cast<char>(std::tolower(static_cast<unsigned char>(enc[i])));
        if (i % 5 == 4 && i + 1 < enc.size()) messy += '-';
    }
    const auto dec2 = SerialBase32Decode(messy);
    assert(dec2.has_value() && *dec2 == data);
    // 非法字符：I/L/O/U 与 0/1 之外的字母歧义 — Crockford 排除 I L O U
    assert(!SerialBase32Decode("IAAAA").has_value());
    assert(!SerialBase32Decode("0AAAA").has_value());  // 0 只在值 0 位置合法? 见实现注
    assert(!SerialBase32Decode("").has_value() || SerialBase32Decode("").value().empty());
}
```
- [ ] 跑测试确认失败（函数不存在）。
- [ ] 实现 `desktop/windows/src/serial_base32.h/.cc`：

```cpp
// serial_base32.h
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace voicestick {
// Crockford Base32：字母表 0-9 A-Z 去掉 I L O U；解码大小写不敏感，
// 忽略 '-' '_' 空白；输入须为整数个字符对应整数字节数（调用方定长 127→79）。
std::string SerialBase32Encode(std::span<const std::uint8_t> data);
std::optional<std::vector<std::uint8_t>> SerialBase32Decode(const std::string& text);
}  // namespace voicestick
```

```cpp
// serial_base32.cc
#include "serial_base32.h"
#include <array>
namespace voicestick {
namespace {
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr signed char kInvalid = -1;
std::array<signed char, 128> BuildDecodeTable() {
    std::array<signed char, 128> table;
    table.fill(kInvalid);
    for (int i = 0; i < 32; ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<signed char>(i);
        table[static_cast<unsigned char>(kAlphabet[i] - 'A' + 'a')] = static_cast<signed char>(i);
    }
    return table;
}
constexpr auto kDecode = BuildDecodeTable();
}  // namespace
std::string SerialBase32Encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve((data.size() * 8 + 4) / 5);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const std::uint8_t byte : data) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            out.push_back(kAlphabet[(buffer >> (bits - 5)) & 0x1f]);
            bits -= 5;
        }
    }
    if (bits > 0) out.push_back(kAlphabet[(buffer << (5 - bits)) & 0x1f]);
    return out;
}
std::optional<std::vector<std::uint8_t>> SerialBase32Decode(const std::string& text) {
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : text) {
        const auto uc = static_cast<unsigned char>(ch);
        if (uc == '-' || uc == '_' || uc == ' ') continue;
        signed char value = uc < 128 ? kDecode[uc] : kInvalid;
        if (value == kInvalid) return std::nullopt;
        buffer = (buffer << 5) | static_cast<std::uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>((buffer >> (bits - 8)) & 0xff));
            bits -= 8;
        }
    }
    if (bits >= 5) return std::nullopt;  // 尾部不足一字节的碎片
    return out;
}
}  // namespace voicestick
```
注：C++20 `constexpr auto kDecode = BuildDecodeTable();` 若 MSVC 编译器不满意，改为函数内 static。

- [ ] main() 注册 `TestSerialBase32RoundTrip();`，跑 ctest 确认绿。
- [ ] `git add -f` + commit。

### Task 3: scripts/gen_serial.py 发卡器（纯 Python Ed25519，零依赖）

- [ ] 写 `scripts/gen_serial.py`（全文，含内嵌纯 Python Ed25519，公域经典实现）：

```python
#!/usr/bin/env python3
"""VoiceStick 离线授权串码发卡器（零第三方依赖）。

用法:
  python scripts/gen_serial.py gen-key --out scripts/license_private_key.hex
  python scripts/gen_serial.py show-pubkey --key-file scripts/license_private_key.hex
  python scripts/gen_serial.py sign --key-file ... --device-id AB12 --machine-guid "{...}"
      --edition 1 --days 365 --counter 42
  python scripts/gen_serial.py test-vectors --key-file ... --out scripts/license_test_vectors.json
私钥文件绝不可提交（.gitignore 已排除）。
"""
import argparse, hashlib, json, os, sys

# ---- 纯 Python Ed25519（公域经典实现，DJB 算法 Python 移植）----
_q = 2**255 - 19
_l = 2**252 + 27742317777372353535851937790883648493

def _H(m): return hashlib.sha512(m).digest()
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
    r = int.from_bytes(_H(h[32:64] + m), "little")
    R = _encodepoint(_scalarmult(_B, r))
    S = (r + int.from_bytes(_H(R + pk + m), "little") * a) % _l
    return R + _encodeint(S)

# ---- 串码格式（与 desktop/windows/src/license.cc 完全一致）----
EDITION_ANNUAL, EDITION_PERPETUAL = 1, 2
EPOCH = 0x1b21  # days for 2026-01-01  (date(2026,1,1) - date(1970,1,1)).days
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
```

- [ ] `git add -f scripts/gen_serial.py` + commit。
- [ ] 生成本地开发密钥：`python scripts/gen_serial.py gen-key --out scripts/license_private_key.hex`；`.gitignore` 加 `scripts/license_private_key.hex`、`scripts/license_test_vectors.json`。

### Task 4: license 核心模块（TDD，消费测试向量）

- [ ] 用 `gen_serial.py test-vectors --out scripts/license_test_vectors.json` 生成向量，把 `public_key_hex` 与 3 条 serial 硬编码进测试（**开发公钥**：后续替换发行公钥时只改 `license_public_key.h` 一处与测试常量）。
- [ ] 失败测试 `TestLicenseVerifySerial`（core_tests.cc）：

```cpp
void TestLicenseVerifySerial() {
    using namespace voicestick;
    const std::vector<std::string> devices = {"AB12", "00FF"};
    const std::string guid = "{11111111-2222-3333-4444-555555555555}";
    // 向量 1：年费码，绑定 AB12，到期 2027-01-01（未过期，相对固定 now=2026-09-13）
    auto r = VerifyLicenseSerial(kTestSerial1, devices, guid);
    assert(r.ok && r.edition == 1);
    assert(r.expiry_days == 365);  // 2027-01-01 距 2026-01-01
    // 绑定机器不匹配 → kWrongBinding
    r = VerifyLicenseSerial(kTestSerial1, devices, "{99999999-8888-7777-6666-555555555555}");
    assert(!r.ok && r.reason == LicenseError::kWrongBinding);
    // 设备不在场列表 → kWrongBinding
    r = VerifyLicenseSerial(kTestSerial1, {"EEEE"}, guid);
    assert(!r.ok && r.reason == LicenseError::kWrongBinding);
    // 向量 2：买断码（edition 2, expiry 0xFFFF）
    r = VerifyLicenseSerial(kTestSerial2, {"CD34"}, "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}");
    assert(r.ok && r.edition == 2 && r.perpetual);
    // 篡改一个字符 → kBadSignature
    std::string tampered = kTestSerial1; tampered[10] = tampered[10] == 'A' ? 'B' : 'A';
    r = VerifyLicenseSerial(tampered, devices, guid);
    assert(!r.ok && r.reason == LicenseError::kBadSignature);
    // 截断/垃圾 → kBadFormat
    r = VerifyLicenseSerial("HELLO", devices, guid);
    assert(!r.ok && r.reason == LicenseError::kBadFormat);
    // 过期：向量 3 到期 2026-12-31，用 now=2027-01-02 判定
    r = VerifyLicenseSerial(kTestSerial3, devices, guid,
                            /*now_days_since_epoch=*/DateToDays(2027, 1, 2));
    assert(!r.ok && r.reason == LicenseError::kExpired);
}
```

- [ ] 失败测试 `TestLicenseStatus`：

```cpp
void TestLicenseStatus() {
    using namespace voicestick;
    LicenseConfig cfg;  // 默认空
    const std::vector<std::string> devices = {"AB12"};
    const std::string guid = "{11111111-2222-3333-4444-555555555555}";
    const auto now = DateToDays(2026, 9, 13);
    // 无串码无锚点：调用方负责先写锚点；此处直接给锚点
    cfg.trial_anchor_days = DateToDays(2026, 9, 1);
    auto s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kTrial && s.days_remaining == 18);  // 9-1 + 30d
    // 锚点 40 天前 → 过期
    cfg.trial_anchor_days = DateToDays(2026, 8, 4);
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kExpired);
    // 有效串码 → Active
    cfg.serial = kTestSerial1;
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kActive);
    // 时钟回拨：last_seen 在未来 3 天 → grace 到 last_seen+7
    cfg.trial_anchor_days = DateToDays(2026, 9, 1);
    cfg.serial.clear();
    cfg.last_seen_days = now + 3;
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kTrial && s.clock_rollback && s.days_remaining == 4);  // (9-1+30) 被 grace (now+3+7) 延长
    // 回拨超过宽限（last_seen 未来 30 天）
    cfg.last_seen_days = now + 30;
    s = EvaluateLicense(cfg, devices, guid, now);
    assert(s.state == LicenseState::kExpired);
}
```

- [ ] 实现 `desktop/windows/src/license.h/.cc`：

```cpp
// license.h
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace voicestick {

enum class LicenseEdition : std::uint8_t { kAnnual = 1, kPerpetual = 2 };
enum class LicenseError { kNone, kBadFormat, kBadSignature, kWrongBinding, kExpired };
struct LicenseVerifyResult {
    bool ok = false;
    LicenseError reason = LicenseError::kNone;
    LicenseEdition edition = LicenseEdition::kAnnual;
    std::uint32_t expiry_days = 0;   // 自 2026-01-01；0xFFFF = 买断
    bool perpetual = false;
};
// device_ids 为归一化（去前缀大写 hex）设备 ID 列表；验签 + 绑定匹配 + 到期判定。
// now_days：当前日期距 2026-01-01 天数（调用方提供，便于测试）。
LicenseVerifyResult VerifyLicenseSerial(const std::string& serial,
                                        const std::vector<std::string>& device_ids,
                                        const std::string& machine_guid,
                                        std::uint32_t now_days);
std::string ComputeBindingKey(const std::string& normalized_device_id,
                              const std::string& machine_guid);  // 8 字节原始值返回 string

struct LicenseConfig {
    std::string serial;             // 原文串码（含连字符）
    std::optional<std::uint32_t> trial_anchor_days;  // 试用期起锚（自 2026-01-01）
    std::optional<std::uint32_t> last_seen_days;     // 见过的最大日期，防回拨
};
enum class LicenseState { kTrial, kActive, kExpired };
struct LicenseStatus {
    LicenseState state = LicenseState::kTrial;
    int days_remaining = 0;          // kTrial/kActive 有意义
    bool perpetual = false;
    bool clock_rollback = false;     // 检测到回拨并进入宽限
    std::uint32_t expiry_days = 0;   // Active 的到期日
};
LicenseStatus EvaluateLicense(const LicenseConfig& cfg,
                              const std::vector<std::string>& device_ids,
                              const std::string& machine_guid,
                              std::uint32_t now_days);
constexpr std::uint32_t kLicenseEpochDays = 0x1b21;  // date(2026,1,1)-date(1970,1,1)
constexpr int kLicenseTrialDays = 30;
constexpr int kLicenseRollbackGraceDays = 7;
constexpr std::uint32_t kLicensePerpetualMarker = 0xFFFF;
std::uint32_t DateToDays(int year, int month, int day);  // 自 2026-01-01，gmtime 安全（纯算术）
}  // namespace voicestick
```

```cpp
// license.cc 关键实现
#include "license.h"
#include "license_public_key.h"
#include "serial_base32.h"
#include <bcrypt.h>
#include <monocypher.h>
namespace voicestick {
namespace {
constexpr std::size_t kPayloadLen = 15, kSigLen = 64, kSerialBytes = 79;
// SHA-256（BCrypt 一次性模式，配方同 firmware_manifest.cc:187）
std::string Sha256Raw(const std::string& input) { /* BCryptOpenAlgorithmProvider + BCryptHash，返回 32 字节 string */ }
}  // namespace

LicenseVerifyResult VerifyLicenseSerial(const std::string& serial,
                                        const std::vector<std::string>& device_ids,
                                        const std::string& machine_guid,
                                        std::uint32_t now_days) {
    LicenseVerifyResult out;
    const auto bytes = SerialBase32Decode(serial);
    if (!bytes || bytes->size() != kSerialBytes) { out.reason = LicenseError::kBadFormat; return out; }
    const auto* payload = bytes->data();
    const auto* sig = bytes->data() + kPayloadLen;
    if (crypto_sign_verify(sig, payload, kPayloadLen, kEmbeddedPublicKey) != 0) {
        out.reason = LicenseError::kBadSignature; return out;
    }
    out.edition = static_cast<LicenseEdition>(payload[0]);
    if (out.edition != LicenseEdition::kAnnual && out.edition != LicenseEdition::kPerpetual) {
        out.reason = LicenseError::kBadFormat; return out;
    }
    const std::string embedded(payload + 1, payload + 9);
    bool bound = false;
    for (const auto& id : device_ids) {
        if (ComputeBindingKey(id, machine_guid) == embedded) { bound = true; break; }
    }
    if (!bound) { out.reason = LicenseError::kWrongBinding; return out; }
    out.expiry_days = payload[9] | (payload[10] << 8);
    out.perpetual = out.expiry_days == kLicensePerpetualMarker;
    if (!out.perpetual && now_days >= out.expiry_days) {
        out.reason = LicenseError::kExpired; return out;
    }
    out.ok = true; out.reason = LicenseError::kNone;
    return out;
}

std::string ComputeBindingKey(const std::string& device_id, const std::string& machine_guid) {
    return Sha256Raw(device_id + "\n" + machine_guid).substr(0, 8);
}

LicenseStatus EvaluateLicense(const LicenseConfig& cfg, ...) {
    // 1) 有串码：验签。ok→Active；kExpired→Expired；其余（格式/签名/绑定）视为无串码，落入试用。
    // 2) 无锚点：返回 kTrial 且 days_remaining=kLicenseTrialDays，调用方（runtime）负责立即写锚点。
    // 3) effective_now = max(now, last_seen)；rollback = now < last_seen。
    //    deadline = anchor + 30；rollback 时 deadline = max(deadline, last_seen + 7)。
    //    days_remaining = (deadline - effective_now + 86399)/86400 的整天数；<=0 → kExpired。
}
std::uint32_t DateToDays(int y, int m, int d) {
    // Howard Hinnant days_from_civil 纯算术，再减 kLicenseEpoch 之前的绝对天数常量
}
}  // namespace voicestick
```

`DateToDays` 用 Howard Hinnant `days_from_civil` 公式算绝对天数再减 `days_from_civil(2026,1,1)`（常量 20454 - 0x1b21=6945? —— 实现时以公式计算为准，测试用例锁行为）。`days_from_civil(1970,1,1)=0`。

`license_public_key.h`（**占位开发公钥，正式发行前替换**）：

```cpp
// license_public_key.h — VoiceStick 离线授权 Ed25519 公钥（32 字节）。
// 私钥由发卡服务端离线保管，绝不进仓库。开发期用 scripts/gen_serial.py gen-key 生成。
#pragma once
#include <cstdint>
namespace voicestick {
inline constexpr std::uint8_t kEmbeddedPublicKey[32] = {
    /* 从 gen_serial.py show-pubkey 输出填入（0x.., 共 32 项） */ };
}
```

- [ ] CMake 源列表加 `src/license.cc src/serial_base32.cc third_party/monocypher/monocypher.c`。
- [ ] ctest 全绿，`git add -f` + commit。

### Task 5: config `[license]` 字段

- [ ] `app_config.h` `struct AppConfig` 加成员：`struct LicenseConfigEntry { std::string serial; std::optional<std::uint32_t> trial_anchor_days; std::optional<std::uint32_t> last_seen_days; bool operator==...= default; } license;`（`#include <optional>`）。
- [ ] `app_config.cc` 解析（仿 :696 local_asr 模式）+ `Save` 序列化（仿 :1025；optional 空则只写 `serial = ""` 与 section 头，锚点/last_seen 为整数或省略）。**不落敏感信息之外的内容**；串码是密文签名，落盘无妨。
- [ ] round-trip 测试 `TestLicenseConfigRoundTrip`（仿 :1328 先例：写→读→相等，含锚点/last_seen 有值与缺省两态）。
- [ ] `Doc/Ref/desktop-config.md` 补 `[license]` 字段说明。ctest 绿，commit。

### Task 6: machine_guid_win + license_runtime（shell）

- [ ] `desktop/windows/src/machine_guid_win.h/.cc`（VoiceStickApp target；`advapi32` 已在 :322）：

```cpp
// machine_guid_win.h
#pragma once
#include <optional>
#include <string>
namespace voicestick {
// 读 HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid；失败（权限/无键）返回 nullopt。
std::optional<std::string> ReadMachineGuid();
}
```
```cpp
// machine_guid_win.cc
#include "machine_guid_win.h"
#include <windows.h>
namespace voicestick {
std::optional<std::string> ReadMachineGuid() {
    wchar_t buffer[64] = {};
    DWORD bytes = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                     L"MachineGuid", RRF_RT_REG_SZ, nullptr, buffer, &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    // 转 UTF-8 并 trim，返回小写原文（绑定键计算在 license.cc 内统一 lower）
    ...
}
}
```

- [ ] `license_runtime.h/.cc`（shell）：持有 `AppConfig&` 与 `VoiceStickCoordinator*` 引用：
  - `LicenseStatus CurrentStatus()`：取 `coordinator_->ConnectedDeviceIds()`（若协调器只暴露 `connected_device_ids_` 私有成员，则加一个公开只读访问器；**先 Grep 确认是否已有公开 getter，有则直接用**）→ 各 id 过 `BleProtocol::NormalizeDeviceId` → `ReadMachineGuid()`（失败时视为空串并记日志）→ `DateToDays(今日 UTC)` → `EvaluateLicense`。
  - `EnsureTrialAnchor()`：`local_asr.enabled && !config_.license.trial_anchor_days` 时写锚点+保存。
  - `AdvanceLastSeen()`：启动时与每 6 小时（SetTimer，照 win32_app 现有 timer 模式）若 now > last_seen 则更新并保存（保存失败仅记日志）。
  - 注意：写 config 走 `AppConfig::Save` 会覆盖用户在设置页未保存的改动？不会——设置页改动经 `on_config_changed` 立即落盘（SaveInputOptions），运行时没有未落盘状态，安全。
- [ ] commit。

### Task 7: coordinator 闸口 + 通知

- [ ] `voice_stick_coordinator.h`：`void SetLicenseGate(std::function<bool()> allow_local_asr);`（默认空 = 放行，保证既有测试不受影响），成员 `std::function<bool()> allow_local_asr_;`。
- [ ] `voice_stick_coordinator.cc`：
  - `HandleLocalMicHotkeyPressed`（:3484）：`if (!local_mic_capture_ || !local_asr_ || !config_.local_asr.enabled) return;` 之后加：
    ```cpp
    if (allow_local_asr_ && !allow_local_asr_()) {
        ui_->ShowMessage(Tr(StringId::kLicenseLocalBlocked, ui_language_));  // 用 Ui 抽象现有通知方法，先 Grep 确认方法名（FakeUi 已有对应实现处）
        return;
    }
    ```
  - `HandlePrimaryButtonDown`（:1726）：`session_asr_` 三分支选择处，本地分支选中但 `allow_local_asr_` 返回 false 时同样通知并 return（不开始会话）。
  - **UI 通知方法名以现有 `Ui` 抽象为准**（core_tests FakeUi :241 已有实现，照着调用）。
- [ ] localization：枚举**末尾**追加（`kStringCount` 自动跟随，localization.cc:12）：
```cpp
kLicenseSectionTitle,        // "License" / "授权"
kLicenseStatusTrial,         // "Trial: %d days remaining" / "试用剩余 %d 天"
kLicenseStatusActive,        // "Licensed until %s" / "已授权至 %s"
kLicenseStatusPerpetual,     // "Licensed (perpetual)" / "已授权（买断）"
kLicenseStatusExpired,       // "License expired" / "授权已到期，请购买续期"
kLicenseActivateButton,      // "Activate..." / "激活…"
kLicenseActivatePrompt,      // "Enter license key:" / "请输入授权码："
kLicenseActivateSuccess,     // "License activated." / "激活成功"
kLicenseActivateFailFormat,  // "Invalid license key format." / "授权码格式无效"
kLicenseActivateFailBinding, // "This key is bound to another device/machine." / "该授权码绑定了其他设备/电脑"
kLicenseActivateFailExpired, // "This license key has expired." / "该授权码已过期"
kLicenseDeviceRequired,      // "Connect your paired voice device first." / "请先连接已配对的语音设备"
kLicenseLocalBlocked,        // "Local ASR requires an active license (trial expired)." / "本地语音识别需要有效授权（试用已到期）"
```
  三张表（枚举 + `EnglishStrings()` + `ChineseStrings()`）同步，漏配会被 `LocalizationTablesAreComplete()`（core_tests.cc:1455）判红。
- [ ] core_tests 补 `FakeLicenseGate` 用例：gate 返回 false 时按主键不下发本地会话、local-mic 热键不启动 capture（仿 :12876 构造先例）。
- [ ] ctest 绿，commit。

### Task 8: 设置页「授权」分组 + 激活框

- [ ] `settings_dialog.h`：构造参数增加 `std::function<std::vector<std::string>()> connected_device_ids`（Win32App 提供，来自 coordinator）与 `std::function<std::string()> machine_guid`（或一个 `LicenseStatusProvider` 聚合回调，二选一，取改动小者）。新控件 ID：`kIdLicenseActivate`（=2049）。
- [ ] `settings_dialog.cc`：
  - 「关于/通用」附近新增分组（`section_title(kLicenseSectionTitle)` + separator + `add()` 布局，仿 :583-604）：
    - 状态 label（`CreateLabel`，初始空，WM_INITDIALOG 时由 `RefreshLicenseStatus()` 填：按 `EvaluateLicense` 状态显示 kLicenseStatusTrial/Active/Perpetual/Expired，%d/%s 调用点替换，仿 `kModelDownloadDiskSpace` 先例）。
    - 「激活…」按钮：无已连接设备时禁用（`EnableWindow`），点击 → 模态输入框（仿 `ApplyTrialApiKey` :1468-1510：`DialogBoxIndirectParamW` + edit + OK/Cancel）。
  - 激活流程：`SerialBase32Decode` 不过 → kLicenseActivateFailFormat；否则 `VerifyLicenseSerial`（devices=connected ids 归一化 + machine guid）→ 按 reason 弹对应文案；ok → `config_.license.serial = 原文` → `SaveSettingsDialog()` → `on_config_changed(config_)` → 成功框 + 刷新状态 label。
- [ ] `win32_app.cc`：`ShowSettings()`（:2482）构造 dialog 时传入两个回调（lambda 捕获 `coordinator_.get()` 与 `license_runtime_.get()`）；`Win32App` 新增 `std::unique_ptr<LicenseRuntime> license_runtime_`（:495 附近构造，持有 config_ 引用需留意 ApplyUpdatedConfig 时 config_ 被替换——LicenseRuntime 改持 `AppConfig*` 或每次从 Win32App 取，**实现时保证不悬垂**）；启动后 `EnsureTrialAnchor()` + `AdvanceLastSeen()`；`SyncLocalMicRuntime`（:1399）处同时 `coordinator_->SetLicenseGate([this]{ return license_runtime_->LocalAsrAllowed(); })`（gate 空时协调器默认放行，不受影响）。
- [ ] ctest 绿，commit。

### Task 9: 构建 + 真机 smoke + 文档收尾

- [ ] 根目录 `build_win.bat` 全量构建；`ctest --test-dir desktop\windows\build-x64 --output-on-failure` 全绿。
- [ ] 真机 smoke（按 AGENTS.md 自动重启 VoiceStick.exe 并查 `%LOCALAPPDATA%\VoiceStick\VoiceStickApp.log`）：
  1. 首次启用本地识别 → 日志出现试用锚点写入；状态行显示「试用剩余 30 天」。
  2. 退出，把 config 的 `trial_anchor_days` 改成 31 天前 → 重启 → 按主键/热键本地 ASR 被拒并出现 kLicenseLocalBlocked 通知。
  3. `python scripts/gen_serial.py sign --key-file scripts/license_private_key.hex --device-id <归一化ID> --machine-guid <本机MachineGuid> --edition 1 --expiry 2027-09-13 --counter 1` → 设置页输入激活 → 状态变「已授权至 2027-09-13」，本地 ASR 恢复。
- [ ] 文档同步：`Doc/Ref/desktop-config.md`（Task 5 已做，复查）、CHANGELOG 加条目（中英）、`Doc/Plan/` 本文状态改「已交付」。
- [ ] 提醒项（向用户报告，不做）：发行前用正式密钥对重新生成 `license_public_key.h` 与测试向量；发卡服务端与购买页不在本计划内。

## 5. 风险与对策

- **回拨宽限被滥用**（每次启动回拨 1 天）：last_seen 单调不减，回拨只延长到 last_seen+7 一次，之后硬性到期——攻击面封顶 7 天。
- **MachineGuid 读取失败**（极少见权限问题）：绑定键退化为仅设备 ID，记警告日志；功能可用，安全性略降。
- **多设备用户**：串码绑激活时在场的设备；运行时任一绑定设备在场即授权通过（VerifyLicenseSerial 已按列表匹配）。
- **重装系统**（MachineGuid 变化）：串码失效——客服用 gen_serial.py 按新 guid 重签发放（counter 递增），属预期运维流程。
- **desktop/windows gitignore**：所有新文件必须 `git add -f` + `git ls-files` 验证（AGENTS.md 红线，f75af4f5 教训）。

## 6. 非目标

不改固件/BLE 协议；不做 macOS；不做账号/多机漫游；云端 ASR 不加闸；收银台服务端不在本计划。
