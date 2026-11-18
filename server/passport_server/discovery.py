"""LAN discovery: devices broadcast 'PASSPORT_DISCOVER 1 <id>' to UDP 8788.

Must stay in sync with RELAY_DISCOVERY_* in main/esp_internal.h.
"""

from __future__ import annotations

import asyncio
import json
import logging
import socket

PORT = 8788
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


async def serve(url: str, name: str) -> asyncio.DatagramTransport:
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(
        lambda: _Protocol(url, name), local_addr=("0.0.0.0", PORT), allow_broadcast=True
    )
    return transport
