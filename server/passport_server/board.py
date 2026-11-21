"""Info-board cards. Each [[board.cards]] entry in config.toml becomes one card.

Card types:
  static    title, value, unit, sub
  countdown title, date = "2026-12-31"        -> days left
  exchange  base = "USD", target = "CNY"        -> rate (open.er-api.com, no key)
  calendar  url = "https://...ics"              -> next event in the coming 7 days
  github    token = "..."                       -> unread notifications
  system    (no fields)                         -> this computer's load and memory
Common optional fields: color = "38BDF8", icon = bell|coin|check|heart|chat|music|star|home|weather
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
from datetime import date, datetime, timedelta, timezone
from typing import Any

import aiohttp

DEFAULT_CARDS = [
    {"type": "countdown", "title": "距离元旦", "date": f"{date.today().year + 1}-01-01",
     "color": "FBBF24", "icon": "star"},
    {"type": "exchange", "title": "美元汇率", "base": "USD", "target": "CNY",
     "color": "4ADE80", "icon": "coin"},
    {"type": "system", "title": "这台电脑", "color": "A78BFA", "icon": "home"},
]


def _card(spec: dict[str, Any], value: str, unit: str = "", sub: str = "") -> dict[str, Any]:
    return {
        "title": spec.get("title", ""),
        "value": value,
        "unit": unit,
        "sub": sub,
        "color": spec.get("color", "38BDF8"),
        "icon": spec.get("icon", "star"),
    }


def countdown(spec: dict[str, Any], today: date | None = None) -> dict[str, Any]:
    today = today or date.today()
    target = date.fromisoformat(spec["date"])
    days = (target - today).days
    if days >= 0:
        return _card(spec, str(days), "天", f"{target.month}月{target.day}日")
    return _card(spec, str(-days), "天前", f"{target.month}月{target.day}日")


async def exchange(spec: dict[str, Any], session: aiohttp.ClientSession) -> dict[str, Any]:
    base, target = spec.get("base", "USD"), spec.get("target", "CNY")
    async with session.get(f"https://open.er-api.com/v6/latest/{base}",
                           timeout=aiohttp.ClientTimeout(total=8)) as r:
        data = await r.json()
    rate = data["rates"][target]
    return _card(spec, f"{rate:.2f}", target, f"1 {base} 兑换")


def _ics_lines(text: str) -> list[str]:
    out: list[str] = []
    for raw in text.splitlines():
        if raw[:1] in (" ", "\t") and out:
            out[-1] += raw[1:]  # 折行续接
        else:
            out.append(raw)
    return out


def _ics_time(name: str, value: str) -> tuple[datetime, bool] | None:
    try:
        if "VALUE=DATE" in name or len(value) == 8:
            return datetime.strptime(value[:8], "%Y%m%d"), True
        t = datetime.strptime(value[:15], "%Y%m%dT%H%M%S")
        if value.endswith("Z"):  # UTC 转本地
            t = t.replace(tzinfo=timezone.utc).astimezone().replace(tzinfo=None)
        return t, False
    except ValueError:
        return None


def next_events(text: str, now: datetime, days: int = 7) -> list[tuple[datetime, bool, str]]:
    """Upcoming (start, all_day, summary) within `days`, earliest first. Recurrence rules are ignored."""
    events: list[tuple[datetime, bool, str]] = []
    start: tuple[datetime, bool] | None = None
    summary = ""
    inside = False
    for line in _ics_lines(text):
        name, _, value = line.partition(":")
        key = name.split(";")[0].upper()
        if line == "BEGIN:VEVENT":
            inside, start, summary = True, None, ""
        elif line == "END:VEVENT" and inside:
            inside = False
            if start:
                t, all_day = start
                end_ok = t.date() >= now.date() if all_day else t >= now
                if end_ok and t < now + timedelta(days=days):
                    events.append((t, all_day, summary or "(无标题)"))
        elif inside and key == "DTSTART":
            start = _ics_time(name, value)
        elif inside and key == "SUMMARY":
            summary = value.replace("\\,", ",").replace("\\;", ";").replace("\\n", " ")
    events.sort(key=lambda e: e[0])
    return events


def calendar_card(spec: dict[str, Any], text: str, now: datetime | None = None) -> dict[str, Any]:
    now = now or datetime.now()
    events = next_events(text, now)
    if not events:
        return _card(spec, "0", "项", "接下来 7 天没有安排")
    t, all_day, summary = events[0]
    diff = (t.date() - now.date()).days
    unit = "今天" if diff == 0 else "明天" if diff == 1 else f"{t.month}月{t.day}日"
    sub = summary if len(events) == 1 else f"{summary} · 之后还有 {len(events) - 1} 项"
    return _card(spec, "全天" if all_day else t.strftime("%H:%M"), unit, sub)


async def calendar(spec: dict[str, Any], session: aiohttp.ClientSession) -> dict[str, Any]:
    url = spec["url"]
    if url.startswith("webcal://"):
        url = "https://" + url[len("webcal://"):]
    async with session.get(url, timeout=aiohttp.ClientTimeout(total=10)) as r:
        text = await r.text()
    return calendar_card(spec, text)


async def github(spec: dict[str, Any], session: aiohttp.ClientSession) -> dict[str, Any]:
    headers = {"Authorization": f"Bearer {spec['token']}", "Accept": "application/vnd.github+json"}
    async with session.get("https://api.github.com/notifications", headers=headers,
                           timeout=aiohttp.ClientTimeout(total=8)) as r:
        items = await r.json()
    sub = items[0]["subject"]["title"] if items else "全部已读"
    return _card(spec, str(len(items)), "条未读", sub)


def system(spec: dict[str, Any]) -> dict[str, Any]:
    load1 = os.getloadavg()[0] if hasattr(os, "getloadavg") else 0.0
    cpus = os.cpu_count() or 1
    pct = min(100, round(load1 / cpus * 100))
    disk = shutil.disk_usage("/")
    sub = f"磁盘剩余 {disk.free // 2**30} GB"
    try:  # macOS 内存压力
        out = subprocess.run(["memory_pressure", "-Q"], capture_output=True, text=True, timeout=2).stdout
        free = [line for line in out.splitlines() if "free percentage" in line]
        if free:
            sub = f"内存空闲 {free[0].split(':')[1].strip()} · " + sub
    except (OSError, subprocess.SubprocessError):
        pass
    return _card(spec, f"{pct}%", "负载", sub)


async def build(cards: list[dict[str, Any]] | None, session: aiohttp.ClientSession) -> dict[str, Any]:
    specs = cards if cards else DEFAULT_CARDS

    async def one(spec: dict[str, Any]) -> dict[str, Any]:
        kind = spec.get("type", "static")
        try:
            if kind == "countdown":
                return countdown(spec)
            if kind == "exchange":
                return await exchange(spec, session)
            if kind == "calendar":
                return await calendar(spec, session)
            if kind == "github":
                return await github(spec, session)
            if kind == "system":
                return system(spec)
            return _card(spec, str(spec.get("value", "")), spec.get("unit", ""), spec.get("sub", ""))
        except Exception as exc:  # 单张卡片失败不影响其他卡片
            return _card(spec, "--", "", f"获取失败:{type(exc).__name__}")

    result = await asyncio.gather(*(one(s) for s in specs))
    return {"updated": datetime.now().strftime("%H:%M"), "cards": list(result)}
