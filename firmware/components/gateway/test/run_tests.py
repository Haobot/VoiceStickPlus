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
]
INC = os.path.join(HERE, "..", "include")
EXE = os.path.join(HERE, "test_gateway_logic.exe")


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


def main() -> int:
    env = capture_msvc_env()
    # Windows 环境变量名不区分大小写而 dict 区分（PATH/Path 并存会让 CreateProcess
    # 取到旧值），统一大写合并：vcvars 捕获值优先
    merged = {k.upper(): v for k, v in os.environ.items()}
    merged.update({k.upper(): v for k, v in env.items()})

    cl_path = os.path.join(env["VCToolsInstallDir"], "bin", "Hostx64", "x64", "cl.exe")
    if not os.path.exists(cl_path):
        raise RuntimeError(f"未找到 cl.exe：{cl_path}")
    cl = subprocess.run(
        [cl_path, "/nologo", "/W4", "/WX", "/utf-8", f"/I{INC}", *SOURCES, f"/Fe{EXE}", "/link", "/SUBSYSTEM:CONSOLE"],
        cwd=HERE, capture_output=True, text=True, encoding="utf-8", errors="replace", env=merged,
    )
    if cl.returncode != 0:
        print(cl.stdout)
        print(cl.stderr, file=sys.stderr)
        print(f"[运行器] 编译失败 exit={cl.returncode}", file=sys.stderr)
        return cl.returncode

    run = subprocess.run([EXE], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    print(run.stdout)
    if run.stderr:
        print(run.stderr, file=sys.stderr)
    return run.returncode


if __name__ == "__main__":
    sys.exit(main())
