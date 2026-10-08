#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gateway / voice_ble 纯逻辑 host 侧单测运行器（跨平台，E8）。

- POSIX（macOS / Linux / CI ubuntu）：cc 直编（-std=c11 -Wall -Wextra -Werror）。
- Windows：保留原 MSVC 路线（vcvars64.bat 捕获环境 → cl.exe /W4 /WX），绕开
  Git Bash→cmd 的引号转义问题。
纯逻辑模块（gateway_report_parser/keymap/atvv/targets/switcher、voice_ble conn_table）
不依赖 ESP-IDF，可在主机验证；产物写临时目录，不污染源码树。
CI：.github/workflows/ci.yml 的 host-tests job 每次推送运行 POSIX 路线。

用法：
    python3 test/run_tests.py            # 全部目标
    python3 test/run_tests.py atvv        # 按名字子串过滤
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GATEWAY_SRC = os.path.join(HERE, "..", "src")
GATEWAY_INC = os.path.join(HERE, "..", "include")
VOICE_BLE = os.path.abspath(os.path.join(HERE, "..", "..", "voice_ble"))
VOICE_BLE_INC = os.path.join(VOICE_BLE, "include")
CJSON_INC = os.path.join(VOICE_BLE, "test", "third_party", "cjson")
# repo root = gateway/test -> gateway -> components -> firmware -> <root>
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

# (name, sources, include_dirs)
TARGETS = [
    ("gateway_logic",
     [os.path.join(HERE, "test_gateway_logic.c"),
      os.path.join(GATEWAY_SRC, "gateway_report_parser.c"),
      os.path.join(GATEWAY_SRC, "gateway_keymap.c"),
      os.path.join(GATEWAY_SRC, "gateway_hogp_report.c")],
     [GATEWAY_INC]),
    ("gateway_atvv",
     [os.path.join(HERE, "test_gateway_atvv.c"),
      os.path.join(GATEWAY_SRC, "gateway_adpcm.c"),
      os.path.join(GATEWAY_SRC, "gateway_atvv_session.c")],
     [GATEWAY_INC]),
    ("gateway_targets",
     [os.path.join(HERE, "test_gateway_targets.c"),
      os.path.join(GATEWAY_SRC, "gateway_targets_core.c")],
     [GATEWAY_INC]),
    ("gateway_switcher",
     [os.path.join(HERE, "test_gateway_switcher.c"),
      os.path.join(GATEWAY_SRC, "gateway_switcher.c")],
     [GATEWAY_INC]),
    # A4：voice_ble 连接表宿主单测与 gateway 同跑道（纯 C 零 ESP 依赖）。
    ("voice_ble_conn_table",
     [os.path.join(VOICE_BLE, "test", "conn_table_test.c"),
      os.path.join(VOICE_BLE, "conn_table.c")],
     [VOICE_BLE_INC]),
    # D7：OTA abort 作用判定（不匹配拒绝）——纯头文件策略，宿主可测。
    ("voice_ble_ota_policy",
     [os.path.join(VOICE_BLE, "test", "voice_ble_ota_policy_test.c")],
     [VOICE_BLE_INC]),
    # 0.1 固件端契约 reader：control_rx 黄金样本经 control_cmd_parse 对拍
    #（消费 tests/contract/fixtures/manifest.json，需 VOICESTICK_REPO_ROOT）。
    ("firmware_control_cmd",
     [os.path.join(VOICE_BLE, "test", "control_cmd_contract_test.c"),
      os.path.join(VOICE_BLE, "control_cmd.c"),
      os.path.join(CJSON_INC, "cJSON.c")],
     [VOICE_BLE_INC, CJSON_INC]),
]


def capture_msvc_env() -> dict:
    """跑 vcvars64 后导出完整环境，供后续直调 cl.exe。"""
    probe = os.path.join(HERE, "_msvc_env.bat")
    with open(probe, "w", encoding="ascii") as f:
        f.write(f'@echo off\r\ncall "{VCVARS}" >nul 2>&1\r\nset\r\n')
    result = subprocess.run(["cmd", "/c", probe], capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    os.remove(probe)
    env = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            env[key.strip()] = value  # 全收：多余变量进入子进程环境无害
    if "INCLUDE" not in env:
        raise RuntimeError(f"vcvars 环境捕获失败：{result.stdout[:200]} {result.stderr[:200]}")
    return env


def build_and_run_posix(sources, includes, exe, workdir) -> int:
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror"]
    for inc in includes:
        cmd += ["-I", inc]
    cmd += sources + ["-o", exe]
    build = subprocess.run(cmd, cwd=workdir, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
    if build.returncode != 0:
        print(build.stdout)
        print(build.stderr, file=sys.stderr)
        print(f"[运行器] 编译失败 exit={build.returncode}（{os.path.basename(exe)}）",
              file=sys.stderr)
        return build.returncode
    run = subprocess.run([exe], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    print(run.stdout)
    if run.stderr:
        print(run.stderr, file=sys.stderr)
    return run.returncode


def build_and_run_msvc(sources, includes, exe, env, workdir) -> int:
    cl_path = os.path.join(env["VCToolsInstallDir"], "bin", "Hostx64", "x64", "cl.exe")
    if not os.path.exists(cl_path):
        raise RuntimeError(f"未找到 cl.exe：{cl_path}")
    merged = {k.upper(): v for k, v in os.environ.items()}
    merged.update({k.upper(): v for k, v in env.items()})
    args = [cl_path, "/nologo", "/W4", "/WX", "/utf-8"]
    for inc in includes:
        args.append(f"/I{inc}")
    args += [*sources, f"/Fe{exe}", "/link", "/SUBSYSTEM:CONSOLE"]
    cl = subprocess.run(args, cwd=workdir, capture_output=True, text=True,
                        encoding="utf-8", errors="replace", env=merged)
    if cl.returncode != 0:
        print(cl.stdout)
        print(cl.stderr, file=sys.stderr)
        print(f"[运行器] 编译失败 exit={cl.returncode}（{os.path.basename(exe)}）",
              file=sys.stderr)
        return cl.returncode
    run = subprocess.run([exe], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    print(run.stdout)
    if run.stderr:
        print(run.stderr, file=sys.stderr)
    return run.returncode


def main() -> int:
    wanted = sys.argv[1:]
    targets = [(n, s, i) for (n, s, i) in TARGETS
               if not wanted or any(w in n for w in wanted)]
    if not targets:
        print(f"[运行器] 无匹配目标：{wanted}", file=sys.stderr)
        return 2
    is_windows = os.name == "nt"
    msvc_env = capture_msvc_env() if is_windows else None
    # 契约 reader 用它定位 tests/contract/fixtures（子进程继承）。
    os.environ["VOICESTICK_REPO_ROOT"] = REPO_ROOT
    failed = 0
    with tempfile.TemporaryDirectory(prefix="voicestick_host_tests_") as tmp:
        for name, sources, includes in targets:
            exe = os.path.join(tmp, name + (".exe" if is_windows else ""))
            if is_windows:
                rc = build_and_run_msvc(sources, includes, exe, msvc_env, tmp)
            else:
                rc = build_and_run_posix(sources, includes, exe, tmp)
            if rc != 0:
                print(f"[运行器] {name} FAILED exit={rc}", file=sys.stderr)
                failed = rc if failed == 0 else failed
    if failed == 0:
        print(f"[运行器] {len(targets)} 个 host 测试目标全部通过"
              + ("（MSVC）" if is_windows else "（cc）"))
    return failed


if __name__ == "__main__":
    sys.exit(main())
