"""Weather from Open-Meteo (no API key), reshaped for the device's small screen."""

from __future__ import annotations

import time
from datetime import datetime
from typing import Any

import aiohttp

from .config import Config

FORECAST = "https://api.open-meteo.com/v1/forecast"
AIR = "https://air-quality-api.open-meteo.com/v1/air-quality"
CACHE_SECONDS = 600
WEEKDAYS = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"]

# WMO weather code -> (device icon code, Chinese text)
_WMO: dict[int, tuple[str, str]] = {
    0: ("sun", "晴"), 1: ("partly", "大部晴朗"), 2: ("partly", "多云"), 3: ("cloud", "阴"),
    45: ("fog", "雾"), 48: ("fog", "雾凇"),
    51: ("rain", "毛毛雨"), 53: ("rain", "毛毛雨"), 55: ("rain", "毛毛雨"),
    56: ("rain", "冻毛毛雨"), 57: ("rain", "冻毛毛雨"),
    61: ("rain", "小雨"), 63: ("rain", "中雨"), 65: ("rain", "大雨"),
    66: ("rain", "冻雨"), 67: ("rain", "冻雨"),
    71: ("snow", "小雪"), 73: ("snow", "中雪"), 75: ("snow", "大雪"), 77: ("snow", "雪粒"),
    80: ("rain", "阵雨"), 81: ("rain", "阵雨"), 82: ("rain", "强阵雨"),
    85: ("snow", "阵雪"), 86: ("snow", "强阵雪"),
    95: ("storm", "雷阵雨"), 96: ("storm", "雷阵雨伴冰雹"), 99: ("storm", "强雷暴"),
}


def wmo(code: int, is_day: bool = True) -> tuple[str, str]:
    icon, text = _WMO.get(int(code), ("cloud", "多云"))
    if icon == "sun" and not is_day:
        icon = "moon"
    return icon, text


def aqi_text(aqi: float | None) -> str:
    if aqi is None:
        return "--"
    for limit, text in ((50, "优"), (100, "良"), (150, "轻度"), (200, "中度"), (300, "重度")):
        if aqi <= limit:
            return text
    return "严重"


def shape(cfg: Config, fc: dict[str, Any], air: dict[str, Any] | None, now: datetime) -> dict[str, Any]:
    cur = fc["current"]
    icon, text = wmo(cur["weather_code"], bool(cur.get("is_day", 1)))
    daily = fc["daily"]
    hourly = fc["hourly"]
    aqi = (air or {}).get("current", {}).get("us_aqi")

    hours = []
    start = now.strftime("%Y-%m-%dT%H:00")
    times = hourly["time"]
    idx = times.index(start) + 1 if start in times else 0
    for i in range(idx, min(idx + 5, len(times))):
        hour = int(times[i][11:13])
        hicon, _ = wmo(hourly["weather_code"][i], 6 <= hour < 18)
        hours.append({"h": f"{hour}时", "t": round(hourly["temperature_2m"][i]), "code": hicon})

    days = []
    for i, day in enumerate(daily["time"][:4]):
        label = "今天" if i == 0 else WEEKDAYS[datetime.fromisoformat(day).weekday()]
        dicon, _ = wmo(daily["weather_code"][i])
        days.append({"day": label, "code": dicon,
                     "hi": round(daily["temperature_2m_max"][i]),
                     "lo": round(daily["temperature_2m_min"][i])})
    return {
        "city": cfg.city,
        "now": {
            "temp": round(cur["temperature_2m"]),
            "feels": round(cur["apparent_temperature"]),
            "humidity": round(cur["relative_humidity_2m"]),
            "code": icon,
            "text": text,
            "aqi": round(aqi) if aqi is not None else None,
            "aqi_text": aqi_text(aqi),
        },
        "today": {"hi": days[0]["hi"] if days else 0, "lo": days[0]["lo"] if days else 0},
        "hourly": hours,
        "daily": days,
        "updated": now.strftime("%H:%M"),
    }


class Weather:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self._cache: dict[str, Any] | None = None
        self._at = 0.0

    async def get(self, session: aiohttp.ClientSession) -> dict[str, Any]:
        if self._cache and time.time() - self._at < CACHE_SECONDS:
            return self._cache
        params = {
            "latitude": self.cfg.latitude,
            "longitude": self.cfg.longitude,
            "timezone": self.cfg.timezone,
            "forecast_days": 4,
            "current": "temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day",
            "hourly": "temperature_2m,weather_code",
            "daily": "weather_code,temperature_2m_max,temperature_2m_min",
        }
        async with session.get(FORECAST, params=params, timeout=aiohttp.ClientTimeout(total=10)) as r:
            r.raise_for_status()
            fc = await r.json()
        air = None
        try:
            async with session.get(AIR, params={"latitude": self.cfg.latitude,
                                                "longitude": self.cfg.longitude,
                                                "current": "us_aqi"},
                                   timeout=aiohttp.ClientTimeout(total=6)) as r:
                if r.status == 200:
                    air = await r.json()
        except aiohttp.ClientError:
            pass
        local_now = datetime.fromisoformat(fc["current"]["time"])
        self._cache = shape(self.cfg, fc, air, local_now)
        self._at = time.time()
        return self._cache
