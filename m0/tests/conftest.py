"""pytest 全局配置：把 src/ 加入模块搜索路径。"""
import sys
from pathlib import Path

M0_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(M0_ROOT / "src"))
