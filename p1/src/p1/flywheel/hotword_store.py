"""热词库 v1：SQLite + 字段级 AES-GCM 加密（路线图 §6.4 Schema 冻结格式）。

设计要点：
- surface/pron/tags 落盘密文（明文不可 grep，安全验收线）；
  去重靠 lower(surface) 的 SHA-256 索引列，加密不影响查询；
- weight/source/freq/last_used 为非敏感元数据，明文列便于 SQL 排序；
- source 优先级 manual > correction > auto_mine（手动添加是最强信号）；
- 权重飞轮：命中 weight+0.05（上限 2.0）；decay_days 天未用按周期 0.9 衰减
  （惰性计算 effective_weight，不回写，避免后台任务）。
"""
from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import uuid
from dataclasses import dataclass, field
from datetime import date, datetime, timedelta
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

# source 优先级：数字越大越强，重复添加只升不降
_SOURCE_RANK = {"auto_mine": 0, "correction": 1, "manual": 2}
WEIGHT_HIT_STEP = 0.05
WEIGHT_CAP = 2.0
DECAY_FACTOR = 0.9

_SCHEMA = """
CREATE TABLE IF NOT EXISTS hotwords (
    id            TEXT PRIMARY KEY,
    surface_key   TEXT UNIQUE NOT NULL,
    surface_enc   BLOB NOT NULL,
    pron_enc      BLOB NOT NULL,
    tags_enc      BLOB NOT NULL,
    weight        REAL NOT NULL,
    source        TEXT NOT NULL,
    freq          INTEGER NOT NULL,
    last_used     TEXT NOT NULL
)
"""


@dataclass
class HotwordEntry:
    """热词条目（Schema v1；embedding 字段 v1 恒为 None，预留语义召回）。"""

    id: str
    surface: str
    pron: list[str] = field(default_factory=list)
    weight: float = 1.0
    source: str = "manual"
    freq: int = 1
    last_used: str = ""
    tags: list[str] = field(default_factory=list)
    embedding: bytes | None = None


def _default_dir() -> Path:
    base = Path(os.environ.get("APPDATA", str(Path.home()))) / "VoiceStickP1"
    base.mkdir(parents=True, exist_ok=True)
    return base


class HotwordStore:
    """加密热词库。db 与密钥分离存放；密钥丢失 = 热词库不可恢复（隐私取舍）。"""

    def __init__(self, db_path: Path | None = None, key_file: Path | None = None):
        base = _default_dir()
        self.db_path = Path(db_path) if db_path else base / "hotwords.db"
        key_path = Path(key_file) if key_file else base / "hw.key"
        self._aes = AESGCM(self._load_or_create_key(key_path))
        # 连接在主线程创建、识别 worker 线程读写（controller 的 busy 互斥保证
        # 同一时刻只有一个线程访问，无需 SQLite 自身的线程校验）。
        self._conn = sqlite3.connect(self.db_path, check_same_thread=False)
        self._conn.execute(_SCHEMA)
        self._conn.commit()

    def close(self) -> None:
        """显式关连接（Windows 下未释放句柄会锁住 db 文件）。"""
        self._conn.close()

    def __enter__(self) -> "HotwordStore":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    # ---- 密钥与字段加密 ----

    @staticmethod
    def _load_or_create_key(key_file: Path) -> bytes:
        if key_file.exists():
            return key_file.read_bytes()
        key_file.parent.mkdir(parents=True, exist_ok=True)
        key = os.urandom(32)
        key_file.write_bytes(key)
        return key

    def _encrypt(self, text: str) -> bytes:
        nonce = os.urandom(12)
        return nonce + self._aes.encrypt(nonce, text.encode("utf-8"), None)

    def _decrypt(self, blob: bytes) -> str:
        return self._aes.decrypt(blob[:12], blob[12:], None).decode("utf-8")

    @staticmethod
    def _surface_key(surface: str) -> str:
        return hashlib.sha256(surface.strip().lower().encode("utf-8")).hexdigest()

    # ---- 写路径 ----

    def add(self, surface: str, source: str = "manual",
            pron: list[str] | None = None, tags: list[str] | None = None) -> HotwordEntry:
        """新增或幂等合并（同词再添加：freq+1，source 只升不降）。"""
        surface = surface.strip()
        if not surface:
            raise ValueError("热词 surface 不能为空")
        key = self._surface_key(surface)
        today = date.today().isoformat()
        row = self._conn.execute(
            "SELECT id, surface_enc, weight, source, freq FROM hotwords WHERE surface_key=?",
            (key,)).fetchone()
        if row:
            _, surface_enc, weight, old_source, freq = row
            merged_source = source if _SOURCE_RANK[source] > _SOURCE_RANK[old_source] else old_source
            self._conn.execute(
                "UPDATE hotwords SET freq=?, source=?, last_used=? WHERE surface_key=?",
                (freq + 1, merged_source, today, key))
            self._conn.commit()
            return HotwordEntry(id=row[0], surface=self._decrypt(surface_enc),
                                weight=weight, source=merged_source, freq=freq + 1,
                                last_used=today)
        entry = HotwordEntry(
            id=str(uuid.uuid4()), surface=surface,
            pron=list(pron or []), tags=list(tags or []),
            source=source, freq=1, weight=1.0, last_used=today)
        self._conn.execute(
            "INSERT INTO hotwords VALUES (?,?,?,?,?,?,?,?,?)",
            (entry.id, key,
             self._encrypt(surface), self._encrypt(json.dumps(entry.pron, ensure_ascii=False)),
             self._encrypt(json.dumps(entry.tags, ensure_ascii=False)),
             entry.weight, entry.source, entry.freq, entry.last_used))
        self._conn.commit()
        return entry

    def record_hit(self, surface: str) -> HotwordEntry | None:
        """识别命中：freq+1、weight 上调、last_used 刷新（飞轮正循环）。"""
        key = self._surface_key(surface)
        row = self._conn.execute(
            "SELECT id, surface_enc, pron_enc, tags_enc, weight, source, freq, last_used "
            "FROM hotwords WHERE surface_key=?", (key,)).fetchone()
        if not row:
            return None
        rid, surface_enc, pron_enc, tags_enc, weight, source, freq, _ = row
        new_weight = min(WEIGHT_CAP, weight + WEIGHT_HIT_STEP)
        today = date.today().isoformat()
        self._conn.execute(
            "UPDATE hotwords SET freq=?, weight=?, last_used=? WHERE surface_key=?",
            (freq + 1, new_weight, today, key))
        self._conn.commit()
        return HotwordEntry(id=rid, surface=self._decrypt(surface_enc),
                            pron=json.loads(self._decrypt(pron_enc)),
                            tags=json.loads(self._decrypt(tags_enc)),
                            weight=new_weight, source=source, freq=freq + 1,
                            last_used=today)

    def remove(self, surface: str) -> bool:
        cur = self._conn.execute("DELETE FROM hotwords WHERE surface_key=?",
                                 (self._surface_key(surface),))
        self._conn.commit()
        return cur.rowcount > 0

    # ---- 读路径 ----

    def get(self, surface: str) -> HotwordEntry | None:
        return self._get_by_key(self._surface_key(surface))

    def _get_by_key(self, key: str) -> HotwordEntry | None:
        row = self._conn.execute(
            "SELECT id, surface_enc, pron_enc, tags_enc, weight, source, freq, last_used "
            "FROM hotwords WHERE surface_key=?", (key,)).fetchone()
        if not row:
            return None
        rid, surface_enc, pron_enc, tags_enc, weight, source, freq, last_used = row
        return HotwordEntry(
            id=rid, surface=self._decrypt(surface_enc),
            pron=json.loads(self._decrypt(pron_enc)),
            tags=json.loads(self._decrypt(tags_enc)),
            weight=weight, source=source, freq=freq, last_used=last_used)

    def list_all(self) -> list[HotwordEntry]:
        rows = self._conn.execute(
            "SELECT surface_key FROM hotwords").fetchall()
        return [e for (key,) in rows if (e := self._get_by_key(key)) is not None]

    def count(self) -> int:
        return self._conn.execute("SELECT COUNT(*) FROM hotwords").fetchone()[0]

    # ---- 权重飞轮 ----

    @staticmethod
    def effective_weight(entry: HotwordEntry, decay_days: int) -> float:
        """使用中上调、久置衰减后的有效权重（惰性计算，不回写）。"""
        last = date.fromisoformat(entry.last_used) if entry.last_used else date.today()
        periods = max(0, (date.today() - last).days // max(1, decay_days))
        return entry.weight * (DECAY_FACTOR ** periods)

    def top_n(self, n: int, decay_days: int = 30) -> list[HotwordEntry]:
        """按有效权重取前 N（ContextBuilder 的注入候选）。"""
        entries = self.list_all()
        entries.sort(key=lambda e: self.effective_weight(e, decay_days), reverse=True)
        return entries[:n]
