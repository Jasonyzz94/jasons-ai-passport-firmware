"""Entry point: python -m passport_server [--config config.toml]"""

from __future__ import annotations

import argparse
import asyncio
import logging
from pathlib import Path

from aiohttp import web

from . import discovery
from .app import create_app
from .config import load


async def run(config_path: Path | None) -> None:
    cfg = load(config_path)
    url = cfg.public_url or f"http://{discovery.lan_ip()}:{cfg.port}"
    app = create_app(cfg)
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, cfg.host, cfg.port).start()
    transport = await discovery.serve(url, cfg.name)
    logging.info("passport-server ready at %s (discovery on UDP %d)", url, discovery.PORT)
    try:
        await asyncio.Event().wait()
    finally:
        transport.close()
        await runner.cleanup()


def main() -> None:
    parser = argparse.ArgumentParser(description="Relay server for the Passport toolbox")
    parser.add_argument("--config", type=Path, default=Path("config.toml"))
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    try:
        asyncio.run(run(args.config))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
