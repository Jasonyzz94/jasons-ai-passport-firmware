"""HTTP API for the Passport toolbox. Endpoints (all JSON unless noted):

  POST /api/pair                 {"device","name"} -> {"token"}       (no auth)
  GET  /api/hello                -> {"name","time"}
  GET  /api/weather              -> weather card data
  GET  /api/board                -> {"updated","cards":[...]}
  GET  /api/home                 -> {"source","items":[...]}
  POST /api/home/toggle          {"id"} -> {"ok"}
  POST /api/assistant/talk       body: IMA ADPCM blocks (chunked) -> {"question","answer","audio"}
  GET  /api/assistant/audio/{id} -> IMA ADPCM blocks (application/octet-stream)

Every endpoint except /api/pair needs "Authorization: Bearer <token>".
"""

from __future__ import annotations

import json
import logging
import secrets
import time
from pathlib import Path
from typing import Any

import aiohttp
from aiohttp import web

from . import board
from .assistant import ASR, LLM, TTS, Assistant, StageError
from .config import Config
from .home import Home
from .weather import Weather

log = logging.getLogger("server")
MAX_AUDIO_BYTES = 2 * 1024 * 1024


class Devices:
    """Paired devices and their tokens, persisted to data_dir/devices.json."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.data: dict[str, dict[str, Any]] = {}
        if path.exists():
            self.data = json.loads(path.read_text(encoding="utf-8"))

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, ensure_ascii=False, indent=2), encoding="utf-8")
        tmp.replace(self.path)
        self.path.chmod(0o600)

    def pair(self, device: str, name: str) -> str:
        token = secrets.token_hex(16)
        self.data[device] = {"token": token, "name": name, "paired_at": int(time.time())}
        self.save()
        return token

    def device_for(self, token: str) -> str | None:
        for device, info in self.data.items():
            if secrets.compare_digest(info.get("token", ""), token):
                return device
        return None


@web.middleware
async def auth(request: web.Request, handler):
    if request.path == "/api/pair" or not request.path.startswith("/api/"):
        return await handler(request)
    header = request.headers.get("Authorization", "")
    token = header[7:] if header.startswith("Bearer ") else ""
    device = request.app["devices"].device_for(token) if token else None
    if not device:
        raise web.HTTPUnauthorized(text="pair first")
    request["device"] = device
    return await handler(request)


async def pair(request: web.Request) -> web.Response:
    cfg: Config = request.app["cfg"]
    if not cfg.auto_approve:
        raise web.HTTPForbidden(text="auto pairing disabled; set auto_approve = true in config.toml to pair new devices")
    body = await request.json()
    device = str(body.get("device", ""))[:16]
    name = str(body.get("name", ""))[:32]
    if not device:
        raise web.HTTPBadRequest(text="device required")
    token = request.app["devices"].pair(device, name)
    log.info("paired device %s (%s) from %s", device, name, request.remote)
    return web.json_response({"token": token})


async def hello(request: web.Request) -> web.Response:
    cfg: Config = request.app["cfg"]
    return web.json_response({"name": cfg.name, "time": int(time.time())})


async def weather(request: web.Request) -> web.Response:
    try:
        data = await request.app["weather"].get(request.app["http"])
    except (aiohttp.ClientError, KeyError, ValueError) as exc:
        log.warning("weather failed: %s", exc)
        raise web.HTTPBadGateway(text="weather unavailable")
    return web.json_response(data)


async def board_view(request: web.Request) -> web.Response:
    cfg: Config = request.app["cfg"]
    return web.json_response(await board.build(cfg.board.get("cards"), request.app["http"]))


async def home_view(request: web.Request) -> web.Response:
    try:
        return web.json_response(await request.app["home"].items(request.app["http"]))
    except aiohttp.ClientError as exc:
        log.warning("home assistant failed: %s", exc)
        return web.json_response({"source": "连接失败", "items": []})


async def home_toggle(request: web.Request) -> web.Response:
    body = await request.json()
    try:
        ok = await request.app["home"].toggle(request.app["http"], str(body.get("id", "")))
    except aiohttp.ClientError:
        ok = False
    if not ok:
        raise web.HTTPBadGateway(text="toggle failed")
    return web.json_response({"ok": True})


async def talk(request: web.Request) -> web.Response:
    data = bytearray()
    async for chunk in request.content.iter_chunked(4096):
        data.extend(chunk)
        if len(data) > MAX_AUDIO_BYTES:
            raise web.HTTPRequestEntityTooLarge(max_size=MAX_AUDIO_BYTES, actual_size=len(data))
    try:
        result = await request.app["assistant"].talk(request["device"], bytes(data), request.app["http"])
    except StageError as exc:   # 告诉设备是哪一步、为什么失败
        return web.json_response({"error": str(exc)}, status=502)
    if result is None:
        raise web.HTTPUnprocessableEntity(text="no speech")
    return web.json_response(result)


async def talk_audio(request: web.Request) -> web.StreamResponse:
    data = request.app["assistant"].audio(request.match_info["audio_id"])
    if data is None:
        raise web.HTTPNotFound()
    return web.Response(body=data, content_type="application/octet-stream")


async def _session(app: web.Application):
    app["http"] = aiohttp.ClientSession()
    yield
    await app["http"].close()


def create_app(cfg: Config) -> web.Application:
    app = web.Application(middlewares=[auth], client_max_size=MAX_AUDIO_BYTES)
    app["cfg"] = cfg
    app["devices"] = Devices(cfg.data_dir / "devices.json")
    app["weather"] = Weather(cfg)
    app["home"] = Home(cfg.home)
    app["assistant"] = Assistant(ASR(cfg.asr), LLM(cfg.llm), TTS(cfg.tts))
    app.cleanup_ctx.append(_session)
    app.router.add_post("/api/pair", pair)
    app.router.add_get("/api/hello", hello)
    app.router.add_get("/api/weather", weather)
    app.router.add_get("/api/board", board_view)
    app.router.add_get("/api/home", home_view)
    app.router.add_post("/api/home/toggle", home_toggle)
    app.router.add_post("/api/assistant/talk", talk)
    app.router.add_get("/api/assistant/audio/{audio_id}", talk_audio)
    return app
