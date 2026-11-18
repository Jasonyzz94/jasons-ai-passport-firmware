"""Home Assistant bridge. The device never sees the HA token.

config.toml:
  [home]
  url = "http://homeassistant.local:8123"
  token = "long-lived access token"
  entities = ["light.living_room", "switch.fan", "scene.home", "sensor.room_temperature"]
"""

from __future__ import annotations

from typing import Any

import aiohttp

TOGGLE_DOMAINS = {"light", "switch", "fan", "input_boolean"}
RUN_DOMAINS = {"scene", "script"}


def item_from_state(state: dict[str, Any]) -> dict[str, Any]:
    entity_id = state["entity_id"]
    domain = entity_id.split(".", 1)[0]
    attrs = state.get("attributes", {})
    name = attrs.get("friendly_name", entity_id)
    raw = state.get("state", "")
    kind = "light" if domain == "light" else "switch" if domain in TOGGLE_DOMAINS else \
        "scene" if domain in RUN_DOMAINS else "sensor"
    if kind in ("light", "switch"):
        on = raw == "on"
        shown = "开" if on else ("关" if raw == "off" else "不可用")
    elif kind == "scene":
        on, shown = False, "执行"
    else:
        on = False
        unit = attrs.get("unit_of_measurement", "")
        shown = f"{raw}{unit}" if raw not in ("unknown", "unavailable") else "不可用"
    return {"id": entity_id, "name": name[:20], "type": kind, "on": on, "state": shown[:14]}


class Home:
    def __init__(self, cfg: dict[str, Any]) -> None:
        self.url = cfg.get("url", "").rstrip("/")
        self.token = cfg.get("token", "")
        self.entities: list[str] = list(cfg.get("entities", []))

    @property
    def configured(self) -> bool:
        return bool(self.url and self.token and self.entities)

    def _headers(self) -> dict[str, str]:
        return {"Authorization": f"Bearer {self.token}", "Content-Type": "application/json"}

    async def items(self, session: aiohttp.ClientSession) -> dict[str, Any]:
        if not self.configured:
            return {"source": "未配置", "items": []}
        out = []
        for entity in self.entities:
            async with session.get(f"{self.url}/api/states/{entity}", headers=self._headers(),
                                   timeout=aiohttp.ClientTimeout(total=6)) as r:
                if r.status == 200:
                    out.append(item_from_state(await r.json()))
        return {"source": "家庭助手", "items": out}

    async def toggle(self, session: aiohttp.ClientSession, entity_id: str) -> bool:
        if entity_id not in self.entities:
            return False
        domain = entity_id.split(".", 1)[0]
        service = "turn_on" if domain in RUN_DOMAINS else "toggle"
        async with session.post(f"{self.url}/api/services/{domain}/{service}",
                                headers=self._headers(), json={"entity_id": entity_id},
                                timeout=aiohttp.ClientTimeout(total=8)) as r:
            return r.status == 200
