"""pytest 全局配置：把 src/ 加入模块搜索路径。"""
import sys
from pathlib import Path

P1_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(P1_ROOT / "src"))
