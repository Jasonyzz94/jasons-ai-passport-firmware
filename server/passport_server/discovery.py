"""LAN discovery: devices broadcast 'PASSPORT_DISCOVER 1 <id>' to UDP 8788.

Must stay in sync with RELAY_DISCOVERY_* in main/esp_internal.h.
"""

from __future__ import annotations

import asyncio
import json
import logging
import socket

PORT = 8788
BEACON_PORT = 8789     # 服务器主动广播自己地址的端口(设备在发现时监听)
BEACON_SECONDS = 3
MAGIC = b"PASSPORT_DISCOVER 1"
log = logging.getLogger("discovery")


def lan_ip() -> str:
    """Best-effort primary LAN address (no packets are actually sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def reply_for(datagram: bytes, url: str, name: str) -> bytes | None:
    if not datagram.startswith(MAGIC):
        return None
    return json.dumps({"url": url, "name": name}, ensure_ascii=False).encode()


class _Protocol(asyncio.DatagramProtocol):
    def __init__(self, url: str, name: str) -> None:
        self.url = url
        self.name = name
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport):  # type: ignore[override]
        self.transport = transport

    def datagram_received(self, data: bytes, addr):  # type: ignore[override]
        reply = reply_for(data, self.url, self.name)
        if reply and self.transport:
            log.info("discovery from %s", addr[0])
            self.transport.sendto(reply, addr)


def beacon_payload(url: str, name: str) -> bytes:
    return json.dumps({"url": url, "name": name, "beacon": 1}, ensure_ascii=False).encode()


def broadcast_targets(ip: str) -> list[str]:
    """受限广播 + 按 /24 推算的本网段广播(覆盖绝大多数家用路由器)。"""
    targets = ["255.255.255.255"]
    parts = ip.split(".")
    if len(parts) == 4 and ip != "127.0.0.1":
        targets.append(".".join(parts[:3] + ["255"]))
    return targets


async def _beacon(url: str, name: str) -> None:
    """有的 VPN / 路由器只拦设备发出的广播:服务器也定期主动广播自己的地址。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setblocking(False)
    payload = beacon_payload(url, name)
    try:
        while True:
            for target in broadcast_targets(lan_ip()):
                try:
                    sock.sendto(payload, (target, BEACON_PORT))
                except OSError as exc:
                    log.debug("beacon to %s failed: %s", target, exc)
            await asyncio.sleep(BEACON_SECONDS)
    finally:
        sock.close()


async def serve(url: str, name: str) -> tuple[asyncio.DatagramTransport, asyncio.Task]:
    """启动询问应答(UDP 8788)与主动广播(UDP 8789);返回两者,退出时都要关掉。"""
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(
        lambda: _Protocol(url, name), local_addr=("0.0.0.0", PORT), allow_broadcast=True
    )
    return transport, loop.create_task(_beacon(url, name))
