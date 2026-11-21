<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# Relay server (optional)

Runs on an always-on computer at home (for example a Mac mini) and gives the toolbox
weather, the info board, a Home Assistant bridge, and the voice assistant (local speech
recognition → language model → speech synthesis). **The device works without it**: when
no server is found it fetches weather, talks to Home Assistant and calls the cloud voice
APIs directly (see [application requirements](../docs/development/app-requirements.md)).

## Install (macOS)

```bash
cd server
export ANTHROPIC_API_KEY=...   # needed for Claude; see config.toml for other models
./install-macos.sh            # creates a venv, installs dependencies, starts with launchd
```

- Configuration: `server/config.toml` (copied from `config.example.toml` on first run).
  Rerun the installer after editing it.
- Log: `~/.passport-server/server.log`; paired devices and tokens:
  `~/.passport-server/devices.json`.
- On first start macOS asks whether Python may accept incoming connections; allow it.
- Other systems: `pip install -r requirements.txt`, then
  `python -m passport_server --config config.toml`.

## How the device finds the server

1. The device broadcasts a query to UDP 8788 and the server replies with its URL.
2. The server also announces its URL to UDP 8789 every 3 s, so discovery still works
   when a VPN or router blocks one direction.
3. The device calls `/api/hello` over HTTP 8787; on 401 it calls `/api/pair` for a token
   (approved automatically when `auto_approve = true`).

If discovery fails, check whether the Mac runs a VPN or TUN-mode proxy, whether the macOS
firewall allows Python, and whether the router enables client/AP isolation. You can also
enter `http://<Mac LAN address>:8787` in the relay server field of the device setup
page to skip broadcasting. The device's About page shows the latest discovery result.

## API

| Method | Path | Notes |
| --- | --- | --- |
| POST | `/api/pair` | `{"device","name"}` → `{"token"}`; the only endpoint without a token |
| GET | `/api/hello` | server name and time |
| GET | `/api/weather` | same shape as the device's direct weather |
| GET | `/api/board` | board cards (configure `[[board.cards]]`) |
| GET / POST | `/api/home`, `/api/home/toggle` | Home Assistant bridge |
| POST | `/api/assistant/talk` | body: IMA ADPCM blocks (chunked) → `{"question","answer","audio"}` |
| GET | `/api/assistant/audio/{id}` | speech audio as IMA ADPCM blocks |

## Tests

```bash
pip install -r requirements.txt
python -m unittest discover -s tests
```

The tests cover the ADPCM codec (bit-exact with the firmware), the API flow, weather /
board / Home Assistant shaping, discovery broadcasts and the language-model request.
