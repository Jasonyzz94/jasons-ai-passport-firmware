"""Configuration loaded from config.toml (see config.example.toml)."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class Config:
    host: str = "0.0.0.0"
    port: int = 8787
    name: str = "Mac mini"
    public_url: str = ""          # advertised to devices; empty = detect LAN IP
    auto_approve: bool = True     # pair new devices on the LAN without confirmation
    data_dir: Path = Path("~/.passport-server").expanduser()

    city: str = "上海"
    latitude: float = 31.23
    longitude: float = 121.47
    timezone: str = "Asia/Shanghai"

    llm: dict[str, Any] = field(default_factory=dict)
    asr: dict[str, Any] = field(default_factory=dict)
    tts: dict[str, Any] = field(default_factory=dict)
    home: dict[str, Any] = field(default_factory=dict)
    board: dict[str, Any] = field(default_factory=dict)


def load(path: Path | None) -> Config:
    cfg = Config()
    if path is None or not path.exists():
        return cfg
    raw = tomllib.loads(path.read_text(encoding="utf-8"))
    server = raw.get("server", {})
    for key in ("host", "port", "name", "public_url", "auto_approve"):
        if key in server:
            setattr(cfg, key, server[key])
    if "data_dir" in server:
        cfg.data_dir = Path(server["data_dir"]).expanduser()
    loc = raw.get("location", {})
    for key in ("city", "latitude", "longitude", "timezone"):
        if key in loc:
            setattr(cfg, key, loc[key])
    for section in ("llm", "asr", "tts", "home", "board"):
        setattr(cfg, section, raw.get(section, {}))
    return cfg
