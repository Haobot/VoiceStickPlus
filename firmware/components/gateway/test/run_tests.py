#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gateway 纯逻辑 host 侧单测运行器。

策略：先经 vcvars64.bat 捕获 MSVC 编译环境（cl.exe 路径与 INCLUDE/LIB），
再以参数化列表直调 cl.exe 编译运行——绕开 Git Bash→cmd 的引号转义问题。
纯逻辑模块（gateway_report_parser/gateway_keymap）不依赖 ESP-IDF，可在主机验证。

用法：python test/run_tests.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

SOURCES = [
    os.path.join(HERE, "test_gateway_logic.c"),
    os.path.join(SRC, "gateway_report_parser.c"),
    os.path.join(SRC, "gateway_keymap.c"),
    os.path.join(SRC, "gateway_hogp_report.c"),
]
INC = os.path.join(HERE, "..", "include")
EXE = os.path.join(HERE, "test_gateway_logic.exe")

# Phase 2 新增：ATVV 纯逻辑件（ADPCM 归一 + 会话状态机）独立目标
ATVV_SOURCES = [
    os.path.join(HERE, "test_gateway_atvv.c"),
    os.path.join(SRC, "gateway_adpcm.c"),
    os.path.join(SRC, "gateway_atvv_session.c"),
]
ATVV_EXE = os.path.join(HERE, "test_gateway_atvv.exe")

# P1 切换器：目标表纯逻辑件
TARGETS_SOURCES = [
    os.path.join(HERE, "test_gateway_targets.c"),
    os.path.join(SRC, "gateway_targets_core.c"),
]
TARGETS_EXE = os.path.join(HERE, "test_gateway_targets.exe")


def capture_msvc_env() -> dict[str, str]:
    """跑 vcvars64 后导出完整环境，供后续直调 cl.exe。"""
    # 经中间 .bat 避免 Git Bash 对 && 的干扰
    probe = os.path.join(HERE, "_msvc_env.bat")
    with open(probe, "w", encoding="ascii") as f:
        f.write(f'@echo off\r\ncall "{VCVARS}" >nul 2>&1\r\nset\r\n')
    result = subprocess.run(["cmd", "/c", probe], capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    os.remove(probe)
    env: dict[str, str] = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            env[key.strip()] = value  # 全收：多余变量进入子进程环境无害
    if "INCLUDE" not in env:
        raise RuntimeError(f"vcvars 环境捕获失败：{result.stdout[:200]} {result.stderr[:200]}")
    return env


def build_and_run(sources: list[str], exe: str, env: dict[str, str]) -> int:
    cl_path = os.path.join(env["VCToolsInstallDir"], "bin", "Hostx64", "x64", "cl.exe")
    if not os.path.exists(cl_path):
        raise RuntimeError(f"未找到 cl.exe：{cl_path}")
    merged = {k.upper(): v for k, v in os.environ.items()}
    merged.update({k.upper(): v for k, v in env.items()})
    cl = subprocess.run(
        [cl_path, "/nologo", "/W4", "/WX", "/utf-8", f"/I{INC}", *sources, f"/Fe{exe}", "/link", "/SUBSYSTEM:CONSOLE"],
        cwd=HERE, capture_output=True, text=True, encoding="utf-8", errors="replace", env=merged,
    )
    if cl.returncode != 0:
        print(cl.stdout)
        print(cl.stderr, file=sys.stderr)
        print(f"[运行器] 编译失败 exit={cl.returncode}（{os.path.basename(exe)}）", file=sys.stderr)
        return cl.returncode

    run = subprocess.run([exe], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    print(run.stdout)
    if run.stderr:
        print(run.stderr, file=sys.stderr)
    return run.returncode


def main() -> int:
    env = capture_msvc_env()
    failed = 0
    for sources, exe in [(SOURCES, EXE), (ATVV_SOURCES, ATVV_EXE), (TARGETS_SOURCES, TARGETS_EXE)]:
        rc = build_and_run(sources, exe, env)
        if rc != 0:
            failed = rc if failed == 0 else failed
    return failed


if __name__ == "__main__":
    sys.exit(main())
