#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP-IDF 固件开发脚本：编译、烧录、串口监控一体化。

参考 scripts/ref/arduino-cli.py 的设计与组件（Spinner 进度条、彩色日志、
串口评分检测、pyserial 监控），适配 ESP-IDF (idf.py / esptool) 工具链。

用法:
    python scripts/idf_cli.py -c            # 编译 (idf.py build)
    python scripts/idf_cli.py -u            # 烧录 (idf.py -p PORT flash)
    python scripts/idf_cli.py -s            # 串口监控
    python scripts/idf_cli.py -cus          # 编译+烧录+监控
    python scripts/idf_cli.py --list-ports  # 列出串口
    python scripts/idf_cli.py -u -p COM17   # 指定端口烧录

按 ESC 或 Ctrl+C 退出串口监控。
"""

import os
import sys
import json
import glob
import re
import time
import platform
import shutil
import subprocess
import threading
import itertools
import logging
from typing import Optional

# 强制 UTF-8（解决 Windows 控制台乱码）
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    except Exception:
        pass

import argparse
import yaml
import serial
from serial.tools import list_ports


# ============================================================
# 日志
# ============================================================
class CustomFormatter(logging.Formatter):
    grey = "\x1b[38;20m"
    yellow = "\x1b[33;20m"
    red = "\x1b[31;20m"
    bold_red = "\x1b[31;1m"
    green = "\x1b[32;20m"
    reset = "\x1b[0m"
    format_str = "%(asctime)s - %(levelname)s - %(message)s"

    FORMATS = {
        logging.DEBUG: grey + format_str + reset,
        logging.INFO: green + format_str + reset,
        logging.WARNING: yellow + format_str + reset,
        logging.ERROR: red + format_str + reset,
        logging.CRITICAL: bold_red + format_str + reset,
    }

    def format(self, record):
        log_fmt = self.FORMATS.get(record.levelno)
        return logging.Formatter(log_fmt, datefmt='%H:%M:%S').format(record)


def setup_logging(log_file, level_name):
    level = getattr(logging, level_name.upper(), logging.INFO)
    log_dir = os.path.dirname(log_file)
    if log_dir and not os.path.exists(log_dir):
        os.makedirs(log_dir)
    file_handler = logging.FileHandler(log_file, encoding='utf-8')
    file_handler.setFormatter(logging.Formatter('%(asctime)s - %(levelname)s - %(message)s'))
    console_handler = logging.StreamHandler()
    console_handler.setFormatter(CustomFormatter())
    logging.basicConfig(level=level, handlers=[file_handler, console_handler])
    return logging.getLogger("EspIdfCLI")


# ============================================================
# Spinner（命令行加载动画 + 进度条后缀）
# ============================================================
class Spinner:
    def __init__(self, message="Processing... ", delay=0.15, enable_progress=True):
        self.spinner = itertools.cycle(['|', '/', '-', '\\'])
        self.delay = delay
        self.busy = False
        self.message = message
        self.suffix = ""
        self.enable_progress = enable_progress
        if not sys.stdout.isatty() and os.environ.get("TERM") == "dumb":
            self.enable_progress = False
        self._screen_lock = threading.Lock()
        if not self.enable_progress:
            sys.stdout.write(message + "\n")
            sys.stdout.flush()

    def update_suffix(self, text):
        with self._screen_lock:
            self.suffix = text
            if self.enable_progress and self.busy:
                self._render()

    def _render(self):
        if not self.enable_progress:
            return
        if self.suffix:
            line = f"\r  {self.message} {self.suffix}"
        else:
            line = f"\r{next(self.spinner)} {self.message}"
        line += "\033[K"
        sys.stdout.write(line)
        sys.stdout.flush()

    def spinner_task(self):
        while self.busy:
            with self._screen_lock:
                if not self.suffix:
                    self._render()
            time.sleep(self.delay)

    def __enter__(self):
        self.busy = True
        if self.enable_progress:
            threading.Thread(target=self.spinner_task, daemon=True).start()
        return self

    def __exit__(self, exception, value, tb):
        self.busy = False
        time.sleep(self.delay)
        with self._screen_lock:
            if self.enable_progress:
                line = f"\r✅ {self.message}" if not exception else f"\r❌ {self.message}"
                line += "\033[K\n"
                sys.stdout.write(line)
            else:
                sys.stdout.write('FAILED\n' if exception else 'DONE\n')
            sys.stdout.flush()


# ============================================================
# 进度解析
# ============================================================
def format_progress_bar(percent: float) -> str:
    percent = max(0.0, min(100.0, percent))
    bar_length = 20
    filled = int(percent / 100 * bar_length)
    bar = "[" + "=" * filled + " " * (bar_length - filled) + "]"
    return f"{bar} {percent:.1f}%"


def parse_flash_progress(line: str) -> Optional[str]:
    """解析 esptool 烧录进度行（Writing at 0x... (X%)），返回进度条字符串。"""
    if not line:
        return None
    # 黑名单：擦除/校验/Hash 等非写入阶段，避免进度条闪烁
    for kw in ("Eras", "Verif", "Hash", "Compress", "Check", "CRC", "Leaving", "Reset", "Connecting"):
        if kw in line:
            return None
    match = re.search(r'(\d+(?:\.\d+)?)\s*%', line)
    if match:
        try:
            return format_progress_bar(float(match.group(1)))
        except ValueError:
            return None
    return None


def parse_build_progress(line: str) -> Optional[str]:
    """解析 ninja 构建进度行（[3/13] Building C object ...），返回步骤计数进度。"""
    if not line:
        return None
    match = re.match(r'\[(\d+)/(\d+)\]', line)
    if match:
        cur, total = int(match.group(1)), int(match.group(2))
        if total > 0:
            return format_progress_bar(cur / total * 100.0) + f" [{cur}/{total}]"
    return None


def split_progress_chunks(text: str):
    for chunk in re.split(r'[\r\n]+', text):
        if chunk:
            yield chunk


# ============================================================
# IDF 环境探测与注入
# ============================================================
class IdfEnvironment:
    """探测 ESP-IDF 安装路径，并执行 init 脚本把环境变量注入当前进程。"""

    def __init__(self, logger, configured_idf_path="", configured_tools_path=""):
        self.logger = logger
        self.idf_path = self._detect_idf_path(configured_idf_path)
        self.tools_path = self._detect_tools_path(configured_tools_path)
        self.python_exe = sys.executable
        self.idf_py = os.path.join(self.idf_path, "tools", "idf.py") if self.idf_path else "idf.py"
        self.injected = False

    def _detect_idf_path(self, configured):
        if configured and os.path.isabs(configured) and os.path.isdir(configured):
            return os.path.normpath(configured)
        env_path = os.environ.get("IDF_PATH", "")
        if env_path and os.path.isdir(env_path) and os.path.exists(os.path.join(env_path, "tools", "idf.py")):
            return os.path.normpath(env_path)
        # Windows: C:\Espressif\frameworks\esp-idf-v*
        candidates = []
        espressif = r"C:\Espressif\frameworks"
        if os.path.isdir(espressif):
            candidates.extend(glob.glob(os.path.join(espressif, "esp-idf-v*")))
        # Linux/Mac: ~/esp/esp-idf, ~/esp-idf
        home = os.path.expanduser("~")
        for p in [os.path.join(home, "esp", "esp-idf"), os.path.join(home, "esp-idf")]:
            if os.path.isdir(p):
                candidates.append(p)
        valid = [c for c in candidates if os.path.exists(os.path.join(c, "tools", "idf.py"))]
        if not valid:
            return ""
        # 取版本号最大的
        def ver_key(p):
            m = re.findall(r'v(\d+(?:\.\d+)*)', os.path.basename(p))
            return [int(x) for x in m[0].split('.')] if m else [0]
        return os.path.normpath(sorted(valid, key=ver_key, reverse=True)[0])

    def _detect_tools_path(self, configured):
        if configured and os.path.isdir(configured):
            return os.path.normpath(configured)
        env_tools = os.environ.get("IDF_TOOLS_PATH", "")
        if env_tools and os.path.isdir(env_tools):
            return os.path.normpath(env_tools)
        if os.path.isdir(r"C:\Espressif"):
            return r"C:\Espressif"
        return ""

    def _init_script(self):
        """返回 init 脚本绝对路径（Windows 优先 idf_cmd_init.bat，否则 export.bat；Linux 用 export.sh）。"""
        if sys.platform == "win32":
            # idf_cmd_init.bat 更完整（含 python venv 激活）
            if self.tools_path:
                cand = os.path.join(self.tools_path, "idf_cmd_init.bat")
                if os.path.exists(cand):
                    return cand
            if self.idf_path:
                cand = os.path.join(self.idf_path, "export.bat")
                if os.path.exists(cand):
                    return cand
        else:
            if self.idf_path:
                cand = os.path.join(self.idf_path, "export.sh")
                if os.path.exists(cand):
                    return cand
        return ""

    def inject(self):
        """执行 init 脚本，捕获其输出的环境变量并注入当前进程 os.environ。"""
        if self.injected:
            return True
        if not self.idf_path:
            self.logger.error("未找到 ESP-IDF 安装，请用 --idf-path 指定或在 config.yaml 配置")
            sys.exit(2)

        init_script = self._init_script()
        if not init_script:
            # 退化：假设当前环境已激活（idf.py 在 PATH）
            self.logger.warning("未找到 IDF init 脚本，假设环境已激活，直接调用 idf.py")
            self.injected = True
            return True

        self.logger.debug(f"IDF init 脚本: {init_script}")
        # Windows: cmd /c "call <init> && set"  → 输出 KEY=VALUE
        # Linux:   bash -c "source <init> >/dev/null 2>&1 && env"
        # 用 shell=True + 单字符串，避免 list 形式下 cmd /c 复合命令引号被双重转义。
        if sys.platform == "win32":
            cmd_str = f'call "{init_script}" && set'
        else:
            cmd_str = f'source "{init_script}" >/dev/null 2>&1 && env'

        # 系统环境变量 IDF_PATH 可能指向不存在的旧版本（如 v5.5.3 但实际装的是 v5.5.1），
        # init.bat 会按 IDF_PATH 找文件失败。这里用脚本探测到的真实路径覆盖，
        # 同时剥离 MSys 标记避免子进程被识别为 MSys。
        init_env = os.environ.copy()
        init_env["IDF_PATH"] = self.idf_path
        if self.tools_path:
            init_env["IDF_TOOLS_PATH"] = self.tools_path
        for k in ("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CHOST", "MSYSTEM_CARCH",
                  "MINGW_CHOST", "MINGW_PREFIX", "MINGW_PACKAGE_PREFIX"):
            init_env.pop(k, None)

        try:
            result = subprocess.run(cmd_str, shell=True, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=60,
                                    env=init_env)
        except subprocess.TimeoutExpired:
            self.logger.error("IDF 环境初始化超时")
            sys.exit(2)
        except Exception as e:
            self.logger.error(f"IDF 环境初始化失败: {e}")
            sys.exit(2)

        if result.returncode != 0:
            self.logger.error(f"IDF init 脚本执行失败 (退出码 {result.returncode}):\n{result.stderr}")
            sys.exit(2)

        # 解析 KEY=VALUE，注入当前进程
        count = 0
        for line in result.stdout.splitlines():
            if '=' in line:
                key, _, value = line.partition('=')
                key = key.strip()
                if key and not key.startswith('='):
                    os.environ[key] = value
                    count += 1
        # 注入后定位 IDF venv 的 python（idf.py 必须在 IDF venv 里运行，
        # 用 sys.executable 或 PATH 里的系统 python 会报 No module named 'esp_idf_monitor'）。
        # 优先用 IDF_PYTHON_ENV_PATH（idf_cmd_init.bat / export.sh 会设置）。
        venv_python = ""
        venv_env = os.environ.get("IDF_PYTHON_ENV_PATH", "")
        if venv_env and os.path.isdir(venv_env):
            if sys.platform == "win32":
                cand = os.path.join(venv_env, "Scripts", "python.exe")
            else:
                cand = os.path.join(venv_env, "bin", "python")
            if os.path.exists(cand):
                venv_python = cand
        if not venv_python:
            # 退化：探测已知 venv 目录
            for pat in [r"C:\Espressif\python_env\idf*_env",
                        os.path.expanduser("~/esp/python_env/idf*_env")]:
                for d in glob.glob(pat):
                    cand = os.path.join(d, "Scripts", "python.exe") if sys.platform == "win32" \
                        else os.path.join(d, "bin", "python")
                    if os.path.exists(cand):
                        venv_python = cand
                        break
                if venv_python:
                    break
        if venv_python:
            self.python_exe = venv_python
        else:
            self.logger.warning("未定位到 IDF venv python，回退到 PATH python，idf.py 可能失败")
            self.python_exe = shutil.which("python") or sys.executable
        self.injected = True
        self.logger.info(f"IDF 环境就绪: {os.path.basename(self.idf_path)} "
                         f"({count} 变量, python={self.python_exe})")
        return True


# ============================================================
# 串口检测（评分制，复用 arduino-cli.py 逻辑）
# ============================================================
class PortDetector:
    DEFAULT_DESCRIPTION_KEYWORDS = ["esp32", "usb serial", "usb-serial", "jtag", "uart", "cp210", "ch340", "ch343", "wch", "ftdi"]
    DEFAULT_MANUFACTURER_KEYWORDS = ["espressif", "wch", "silicon labs", "ftdi"]
    DEFAULT_HWID_KEYWORDS = ["vid:pid=303a:", "vid:pid=1a86:", "vid:pid=10c4:ea60", "vid:pid=0403:6001"]

    def __init__(self, logger, config, state_file):
        self.logger = logger
        self.cfg = config or {}
        self.state_file = state_file

    def _norm_list(self, values, defaults=None):
        if values is None:
            values = defaults or []
        elif isinstance(values, str):
            values = [values]
        return [str(v).strip().lower() for v in values if str(v).strip()]

    def enumerate(self):
        ports = []
        for port in list_ports.comports():
            ports.append({
                "device": port.device or "",
                "description": port.description or "",
                "manufacturer": getattr(port, "manufacturer", "") or "",
                "product": getattr(port, "product", "") or "",
                "hwid": port.hwid or "",
                "serial_number": getattr(port, "serial_number", "") or "",
                "location": getattr(port, "location", "") or "",
                "vid": getattr(port, "vid", None),
                "pid": getattr(port, "pid", None),
            })
        ports.sort(key=lambda x: x["device"].lower())
        return ports

    def format_summary(self, p):
        s = f'{p["device"]}: {p["description"] or "Unknown"}'
        extras = []
        if p.get("manufacturer"):
            extras.append(f'mfr={p["manufacturer"]}')
        if p.get("serial_number"):
            extras.append(f'sn={p["serial_number"]}')
        if p.get("vid") is not None and p.get("pid") is not None:
            extras.append(f'vid_pid={p["vid"]:04X}:{p["pid"]:04X}')
        if extras:
            s += f' ({", ".join(extras)})'
        return s

    def list(self, level="info"):
        ports = self.enumerate()
        if not ports:
            getattr(self.logger, level)("未检测到可用串口")
            return ports
        getattr(self.logger, level)(f"检测到 {len(ports)} 个串口设备:")
        for p in ports:
            getattr(self.logger, level)(f"  - {self.format_summary(p)}")
        return ports

    def _score(self, p):
        score = 0
        reasons = []
        desc = f'{p.get("description","")} {p.get("product","")}'.lower()
        mfr = p.get("manufacturer", "").lower()
        hwid = p.get("hwid", "").lower()
        for kw in self._norm_list(self.cfg.get("description_keywords"), self.DEFAULT_DESCRIPTION_KEYWORDS):
            if kw in desc:
                score += 30; reasons.append(f"desc:{kw}")
        for kw in self._norm_list(self.cfg.get("manufacturer_keywords"), self.DEFAULT_MANUFACTURER_KEYWORDS):
            if kw in mfr:
                score += 20; reasons.append(f"mfr:{kw}")
        for kw in self._norm_list(self.cfg.get("hwid_keywords"), self.DEFAULT_HWID_KEYWORDS):
            if kw in hwid:
                score += 40; reasons.append(f"hwid:{kw}")
        vid, pid = p.get("vid"), p.get("pid")
        vid_pid = f"{vid:04x}:{pid:04x}" if vid is not None and pid is not None else None
        preferred = self._norm_list(self.cfg.get("preferred_vid_pid"))
        if vid_pid and preferred and vid_pid in preferred:
            score += 160; reasons.append(f"preferred:{vid_pid}")
        return score, reasons

    def select_best(self, ports, fixed_port=""):
        if fixed_port:
            for p in ports:
                if p["device"].lower() == fixed_port.lower():
                    return p, f"命中指定端口 {fixed_port}"
        scored = []
        for p in ports:
            s, r = self._score(p)
            if s > 0:
                scored.append((s, r, p))
        scored.sort(key=lambda x: (-x[0], x[2]["device"].lower()))
        if scored:
            s, r, p = scored[0]
            return p, f"自动匹配命中 {p['device']} (score={s}, {', '.join(r)})"
        if len(ports) == 1:
            return ports[0], f"仅检测到一个串口，兜底选择 {ports[0]['device']}"
        return None, "未找到满足匹配规则的串口，请用 --port 指定"

    def load_state(self):
        if not os.path.exists(self.state_file):
            return {}
        try:
            with open(self.state_file, "r", encoding="utf-8") as f:
                d = json.load(f)
                return d if isinstance(d, dict) else {}
        except Exception:
            return {}

    def save_port(self, port):
        if not port:
            return
        d = self.load_state()
        d["last_success_port"] = port
        d["updated_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
        try:
            os.makedirs(os.path.dirname(self.state_file) or ".", exist_ok=True)
            with open(self.state_file, "w", encoding="utf-8") as f:
                json.dump(d, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    @property
    def remembered_port(self):
        return str(self.load_state().get("last_success_port", "")).strip()


# ============================================================
# 主自动化类
# ============================================================
class EspIdfAutomation:
    def __init__(self, config_path, args):
        self.logger = logging.getLogger("EspIdfCLI")
        self.args = args
        self.config = self._load_config(config_path)
        self.config_path = config_path
        base_dir = os.path.dirname(os.path.abspath(config_path))

        dft = self.config.get('default', {}) or {}
        self.project_dir = self._resolve_path(dft.get('project_dir', '../firmware'), base_dir)
        self.target = args.target or dft.get('target', 'esp32s3')
        self.port = args.port or dft.get('port', '') or ''
        self.baud = args.baud or dft.get('baudrate', 921600)
        self.monitor_baud = args.monitor_baud or dft.get('monitor_baudrate', 115200)
        self.reset_before_monitor = dft.get('flash_baud_before_monitor', True)

        self.idf_env = IdfEnvironment(self.logger,
                                      dft.get('idf_path', ''),
                                      dft.get('idf_tools_path', ''))
        sd_cfg = self.config.get('serial_detection', {}) or {}
        state_file = sd_cfg.get('state_file', '.idf_cli_state.json')
        if not os.path.isabs(state_file):
            state_file = os.path.join(base_dir, state_file)
        self.port_detector = PortDetector(self.logger, sd_cfg, state_file)
        self.os_type = platform.system()

    def _load_config(self, path):
        if not os.path.exists(path):
            return {}
        try:
            with open(path, 'r', encoding='utf-8') as f:
                return yaml.safe_load(f) or {}
        except Exception as e:
            self.logger.error(f"加载配置文件失败: {e}")
            sys.exit(1)

    @staticmethod
    def _resolve_path(p, base_dir):
        p = str(p or "").strip()
        if not p:
            return ""
        return p if os.path.isabs(p) else os.path.normpath(os.path.join(base_dir, p))

    # ---------- 串口 ----------
    def resolve_port(self, required=False):
        ports = self.port_detector.enumerate()
        # 自动模式优先用记忆端口
        if not self.args.port and not self.port:
            remembered = self.port_detector.remembered_port
            if remembered:
                for p in ports:
                    if p["device"].lower() == remembered.lower():
                        self.port = p["device"]
                        self.logger.info(f"串口解析: {self.port} (命中上次成功端口)")
                        return self.port
                self.logger.warning(f"上次成功串口 {remembered} 当前不可用，改为自动匹配")

        fixed = self.port if (self.port and self.port.lower() not in {"auto", "detect", "scan"}) else ""
        selected, reason = self.port_detector.select_best(ports, fixed)
        if selected:
            self.port = selected["device"]
            self.logger.info(f"串口解析: {self.port} ({reason})")
            return self.port
        self.logger.error(reason or "未检测到串口")
        self.port_detector.list(level="error")
        return None if required else self.port

    # ---------- 命令执行 ----------
    def run_command(self, cmd, timeout=None, message="Processing... ",
                    enable_progress=True, progress_parser=parse_flash_progress,
                    cwd=None):
        self.logger.debug(f"执行: {' '.join(cmd)}")
        start = time.time()
        use_progress = enable_progress
        use_progress = use_progress and self.logger.getEffectiveLevel() >= logging.INFO
        use_progress = use_progress and not getattr(self.args, 'no_progress', False)
        if not sys.stdout.isatty() and os.environ.get("TERM") == "dumb":
            use_progress = False

        spinner = None
        try:
            if self.logger.getEffectiveLevel() >= logging.INFO:
                spinner = Spinner(message, delay=0.15, enable_progress=use_progress)
                spinner.__enter__()

            # 子进程环境：剥离 MSys/Mingw 标记。从 Git Bash 调脚本时父进程带 MSYSTEM 等
            # 变量，idf.py 检测到会拒绝运行（"MSys/Mingw is no longer supported"）并以
            # 退出码 0 静默退出，导致看似编译成功实则什么都没做。
            sub_env = os.environ.copy()
            for k in ("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CHOST", "MSYSTEM_CARCH",
                      "MINGW_CHOST", "MINGW_PREFIX", "MINGW_PACKAGE_PREFIX"):
                sub_env.pop(k, None)

            process = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding='utf-8', errors='replace',
                bufsize=1, universal_newlines=True, cwd=cwd, env=sub_env,
            )
            stdout_lines = []
            pending = ""
            while True:
                char = process.stdout.read(1)
                if char == "" and process.poll() is not None:
                    break
                if char == "":
                    continue
                if char in "\r\n":
                    for line in split_progress_chunks(pending):
                        stdout_lines.append(line)
                        self._handle_line(line, spinner, use_progress, progress_parser)
                    pending = ""
                else:
                    pending += char
            for line in split_progress_chunks(pending):
                stdout_lines.append(line)
                self._handle_line(line, spinner, use_progress, progress_parser)

            returncode = process.wait(timeout=timeout)
            stdout_full = "\n".join(stdout_lines)
            if returncode != 0:
                raise subprocess.CalledProcessError(returncode, cmd, output=stdout_full, stderr="")
            # idf.py 在 MSys/Mingw / 未注入环境等场景下会以退出码 0 静默拒绝运行，
            # 必须查输出关键字判定是否真的执行了构建/烧录流程。
            for refuse_kw in ("MSys/Mingw is no longer supported",
                              "is not recognized as an internal or external command"):
                if refuse_kw in stdout_full:
                    if spinner:
                        spinner.__exit__(RuntimeError, RuntimeError(refuse_kw), None)
                    self.logger.error(f"命令拒绝运行：{refuse_kw}")
                    self.logger.error(f"输出:\n{stdout_full}")
                    return False, stdout_full
            if spinner:
                spinner.__exit__(None, None, None)
            self.logger.info(f"命令成功 (耗时 {time.time()-start:.2f}s)")
            return True, stdout_full
        except subprocess.CalledProcessError as e:
            if spinner:
                spinner.__exit__(type(e), e, None)
            self.logger.error(f"命令失败 (退出码 {e.returncode})")
            self.logger.error(f"输出:\n{e.stdout}")
            return False, e.stdout
        except subprocess.TimeoutExpired:
            process.kill()
            if spinner:
                spinner.__exit__(subprocess.TimeoutExpired, None, None)
            self.logger.error(f"命令超时 ({timeout}s)")
            return False, "Timeout"
        except Exception as e:
            if spinner:
                spinner.__exit__(Exception, e, None)
            self.logger.error(f"未知错误: {e}")
            return False, str(e)

    def _handle_line(self, line, spinner, use_progress, progress_parser):
        if not line:
            return
        progress_text = None
        if use_progress:
            progress_text = progress_parser(line)
        if progress_text and spinner:
            spinner.update_suffix(progress_text)
            return
        # 关键错误/警告行穿透显示
        low = line.lower()
        is_important = any(k in low for k in ("error", "fail", "warning", "fatal"))
        if self.logger.getEffectiveLevel() <= logging.DEBUG:
            sys.stdout.write("\r\033[K" + line + "\n")
            if spinner:
                spinner._render()
        elif is_important:
            sys.stdout.write("\r\033[K" + line + "\n")
            sys.stdout.flush()
            if spinner:
                spinner._render()

    def _idf_cmd(self, subcmd):
        """构造 idf.py 命令（用注入环境的 python 调用 idf.py 脚本）。"""
        return [self.idf_env.python_exe, self.idf_env.idf_py] + subcmd

    # ---------- 编译 ----------
    def compile(self):
        self.idf_env.inject()
        self.logger.info(f"开始编译: {self.project_dir} (target={self.target})")
        cmd = self._idf_cmd(["build"])
        success, _ = self.run_command(
            cmd, message="正在编译... ",
            progress_parser=parse_build_progress,
            cwd=self.project_dir,
        )
        if not success:
            self.logger.error("编译失败，终止流程")
            sys.exit(10)
        return True

    # ---------- 烧录 ----------
    def upload(self):
        if not self.resolve_port(required=True):
            self.logger.error("未指定烧录端口或自动匹配失败")
            sys.exit(11)
        self.idf_env.inject()
        self.logger.info(f"开始烧录: {self.port} @ {self.baud} baud")
        cmd = self._idf_cmd(["-p", self.port, "-b", str(self.baud), "flash"])
        success, _ = self.run_command(
            cmd, message="正在烧录... ",
            progress_parser=parse_flash_progress,
            cwd=self.project_dir,
        )
        if success:
            self.port_detector.save_port(self.port)
            return True
        self.logger.error("烧录失败，终止流程")
        sys.exit(12)

    # ---------- 自动复位 ----------
    def dtr_rts_reset(self):
        try:
            ser = serial.Serial(self.port, self.monitor_baud, timeout=1)
            ser.dtr = False
            ser.rts = False
            time.sleep(0.05)
            ser.rts = True
            time.sleep(0.05)
            ser.rts = False
            time.sleep(0.05)
            ser.dtr = False
            ser.close()
            return True
        except Exception as e:
            self.logger.warning(f"DTR/RTS 复位失败: {e}")
            return False

    # ---------- 串口监控 ----------
    def _keyboard_listener(self, stop_event):
        try:
            if sys.platform == "win32":
                import msvcrt
                while not stop_event.is_set():
                    if msvcrt.kbhit():
                        if msvcrt.getch() == b'\x1b':  # ESC
                            stop_event.set()
                            break
                    time.sleep(0.02)
            else:
                import select, tty, termios
                old = termios.tcgetattr(sys.stdin.fileno())
                try:
                    tty.setcbreak(sys.stdin.fileno())
                    while not stop_event.is_set():
                        if select.select([sys.stdin], [], [], 0.1)[0]:
                            if sys.stdin.read(1) == '\x1b':
                                stop_event.set()
                                break
                finally:
                    termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old)
        except Exception as e:
            self.logger.debug(f"键盘监听异常: {e}")

    def monitor(self):
        if not self.resolve_port(required=True):
            self.logger.error("未指定监控端口或自动匹配失败")
            sys.exit(13)
        if self.reset_before_monitor:
            self.logger.info(f"复位设备以捕获 boot 日志: {self.port}")
            self.dtr_rts_reset()
            time.sleep(0.3)

        self.logger.info(f"打开串口监控: {self.port} @ {self.monitor_baud}")
        self.logger.info("按 ESC 或 Ctrl+C 退出监控")
        if self.os_type == "Windows":
            os.system("cls")
        else:
            os.system("clear")

        ser = None
        stop_event = threading.Event()
        kb_thread = None
        try:
            ser = serial.Serial(self.port, self.monitor_baud, timeout=0.1)
            kb_thread = threading.Thread(target=self._keyboard_listener, args=(stop_event,), daemon=True)
            kb_thread.start()
            while not stop_event.is_set():
                if ser.in_waiting:
                    data = ser.read(ser.in_waiting)
                    sys.stdout.write(data.decode('utf-8', errors='replace'))
                    sys.stdout.flush()
                time.sleep(0.005)
        except KeyboardInterrupt:
            pass
        except serial.SerialException as e:
            self.logger.error(f"串口错误: {e}")
        finally:
            stop_event.set()
            if kb_thread and kb_thread.is_alive():
                kb_thread.join(timeout=0.5)
            if ser and ser.is_open:
                ser.close()
            if self.os_type == "Windows":
                os.system("cls")
            else:
                os.system("clear")
            sys.stdout.write("\033[?25h")
            sys.stdout.flush()
        self.logger.info("用户停止监控")

    # ---------- 主流程 ----------
    def run(self):
        total_start = time.time()
        if self.args.list_ports:
            self.port_detector.list()
            if not (self.args.compile or self.args.upload or self.args.serial):
                return
        if not (self.args.compile or self.args.upload or self.args.serial):
            self.logger.warning("未指定任何操作。请用 -c / -u / -s 参数。")
            return
        if self.args.compile:
            self.compile()
        if self.args.upload:
            self.upload()
        if self.args.serial:
            if self.args.upload:
                time.sleep(1)
            self.monitor()
        self.logger.info(f"所有任务完成，总耗时: {time.time()-total_start:.2f}s")


def setup_signal_handlers():
    def handle_sigint(signum, frame):
        sys.stdout.write("\033[?25h\n")
        sys.stdout.flush()
        sys.exit(1)
    try:
        import signal
        signal.signal(signal.SIGINT, handle_sigint)
        signal.signal(signal.SIGTERM, handle_sigint)
    except (ValueError, ImportError):
        pass


def main():
    setup_signal_handlers()
    parser = argparse.ArgumentParser(description="ESP-IDF 固件开发脚本：编译/烧录/监控")
    parser.add_argument('-c', '--compile', action='store_true', help='编译 (idf.py build)')
    parser.add_argument('-u', '--upload', action='store_true', help='烧录 (idf.py flash)')
    parser.add_argument('-s', '--serial', action='store_true', help='串口监控')
    parser.add_argument('--port', '-p', help='串口设备 (COMx / /dev/ttyUSBx)，留空自动检测')
    parser.add_argument('--baud', '-b', type=int, help='烧录波特率 (默认 921600)')
    parser.add_argument('--monitor-baud', dest='monitor_baud', type=int, help='监控波特率 (默认 115200)')
    parser.add_argument('--target', help='ESP-IDF 目标芯片 (默认 esp32s3)')
    parser.add_argument('--config', default='idf_cli.yaml', help='配置文件路径')
    parser.add_argument('--idf-path', dest='idf_path', help='ESP-IDF 安装路径（覆盖自动探测）')
    parser.add_argument('--list-ports', dest='list_ports', action='store_true', help='列出检测到的串口')
    parser.add_argument('--no-progress', dest='no_progress', action='store_true', help='关闭进度条，逐行输出')
    parser.add_argument('-v', '--verbose', action='store_true', help='DEBUG 日志，显示完整命令输出')
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = args.config if os.path.isabs(args.config) else os.path.join(script_dir, args.config)

    # 日志路径与级别
    log_file = os.path.join(script_dir, "idf_cli.log")
    log_level = "DEBUG" if args.verbose else "INFO"
    try:
        with open(config_path, 'r', encoding='utf-8') as f:
            cfg = yaml.safe_load(f) or {}
            if cfg.get('logging', {}).get('file'):
                lf = cfg['logging']['file']
                log_file = lf if os.path.isabs(lf) else os.path.join(script_dir, lf)
            if not args.verbose and cfg.get('logging', {}).get('level'):
                log_level = cfg['logging']['level']
    except Exception:
        pass
    setup_logging(log_file, log_level)

    # --idf-path 覆盖：写回 config 对象
    if args.idf_path:
        # 简单注入：构造 EspIdfAutomation 后它会读 config，这里先把 env 设上
        os.environ['IDF_PATH'] = os.path.abspath(args.idf_path)

    automation = EspIdfAutomation(config_path, args)
    automation.run()


if __name__ == "__main__":
    main()
