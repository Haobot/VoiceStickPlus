"""pytest 全局配置：搜索路径 + 跨文件共享 fixture。"""
import sys
from pathlib import Path

P1_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(P1_ROOT / "src"))

from p1.flywheel.hotword_store import HotwordStore  # noqa: E402 须在路径插入后

import pytest  # noqa: E402


@pytest.fixture
def store(tmp_path):
    """临时加密热词库（每用例独立 db 与密钥，自动关连接防 Windows 句柄锁）。"""
    s = HotwordStore(db_path=tmp_path / "hotwords.db", key_file=tmp_path / "hw.key")
    yield s
    s.close()
