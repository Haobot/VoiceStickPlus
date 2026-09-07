#!/usr/bin/env python3
"""VoiceStick P1 桌面客户端入口（Windows 先行）。

用法（在 p1/ 目录，复用 m0 虚拟环境与模型权重）:
    ../m0/.venv/Scripts/python.exe main.py

交互：按住右 Ctrl 说话 → 松手出字（注入当前焦点窗口）；Esc 取消本句。
退出：控制台 Ctrl+C。
"""
from __future__ import annotations

import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))

from p1.config import load_config  # noqa: E402
from p1.controller import VoiceController  # noqa: E402
from p1.engines.sense_voice import SenseVoiceAdapter  # noqa: E402
from p1.flywheel.hotword_store import HotwordStore  # noqa: E402
from p1.interaction.hotkey import HotkeyListener  # noqa: E402
from p1.interaction.injector import ClipboardInjector  # noqa: E402
from p1.interaction.overlay import OverlayApp  # noqa: E402
from p1.orchestration.pipeline import Pipeline  # noqa: E402
from p1.orchestration.session import RecordingSession  # noqa: E402
from p1.rewrite.rewriter import build_rewriter  # noqa: E402

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(name)s %(message)s")
log = logging.getLogger("p1.main")


def build_application(cfg) -> tuple[OverlayApp, HotkeyListener]:
    """组装四层组件（交互/编排/飞轮/引擎），返回 UI 与热键监听。"""
    engine = SenseVoiceAdapter(models_dir=cfg.models_dir)
    if not engine.is_ready():
        log.error("SenseVoice 模型未就位: %s（先跑 m0/scripts/download_models.py）",
                  cfg.models_dir)
        sys.exit(1)
    engine.load()  # 启动即加载，避免首句卡顿
    log.info("SenseVoice 引擎就绪: %s", cfg.models_dir)

    store = HotwordStore()
    rewriter = build_rewriter(cfg.rewrite_provider) if cfg.rewrite_enabled else None
    pipeline = Pipeline(engine=engine, hotword_store=store,
                        rewriter=rewriter, injector=ClipboardInjector())
    overlay = OverlayApp()
    controller = VoiceController(session=RecordingSession(),
                                 pipeline=pipeline, overlay=overlay)
    hotkeys = HotkeyListener(
        push_key=cfg.push_to_talk, cancel_key=cfg.cancel_key,
        on_press=controller.start_session,
        on_release=controller.finish_session,
        on_cancel=controller.cancel_session)
    log.info("热词库 %d 条 | 改写 %s | 热键 按住[%s]说话 Esc取消",
             store.count(), cfg.rewrite_provider if rewriter else "关闭",
             cfg.push_to_talk)
    return overlay, hotkeys


def main() -> int:
    cfg = load_config()
    overlay, hotkeys = build_application(cfg)
    hotkeys.start()
    log.info("VoiceStick P1 已启动：按住 %s 说话，松手出字", cfg.push_to_talk)
    try:
        overlay.run()  # tkinter mainloop（主线程阻塞）
    except KeyboardInterrupt:
        log.info("收到退出信号")
    finally:
        hotkeys.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
