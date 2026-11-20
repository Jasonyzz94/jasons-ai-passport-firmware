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
export DEEPSEEK_API_KEY=sk-...   # the default config chats through DeepSeek; see below for others
./install-macos.sh              # creates a venv, installs dependencies, starts with launchd
```

The installer copies any exported `ANTHROPIC_API_KEY`, `DEEPSEEK_API_KEY`,
`SILICONFLOW_API_KEY`, `OPENAI_API_KEY` and `LLM_API_KEY` into the launchd job; rerun it
after changing a key.

- Configuration: `server/config.toml` (copied from `config.example.toml` on first run).
  Rerun the installer after editing it.
- Log: `~/.passport-server/server.log`; paired devices and tokens:
  `~/.passport-server/devices.json`.
- On first start macOS asks whether Python may accept incoming connections; allow it.
- Other systems: `pip install -r requirements.txt`, then
  `python -m passport_server --config config.toml`.

## Configure the voice assistant

The assistant has three steps: recognition (speech to text), chat, and speech (text to
speech). They map to `[asr]`, `[llm]` and `[tts]` in `config.toml`, and each step can use
a different service.

| What you have | `[llm]` | `[asr]` / `[tts]` |
| --- | --- | --- |
| Only DeepSeek or another OpenAI-compatible chat API | `provider = "openai_compatible"` with `base_url`, `model` and a key | Keep the defaults: local `mlx_whisper` and macOS `say`, free |
| Nothing paid | Local Ollama: `base_url = "http://localhost:11434/v1"`, no key | Same as above |
| A nicer voice | Any | `[tts] provider = "openai"` pointing at a provider with a speech endpoint, such as SiliconFlow |

DeepSeek offers chat only, with no recognition or speech endpoint, so do not point
`[asr]` / `[tts]` at it. When a step fails the device shows the step and the reason
(for example "chat: out of credit"); details are in the server log.

The device can also call the cloud directly, without this computer: on the device's
setup page, pick a voice-assistant preset such as "DeepSeek chat + SiliconFlow voice"
and enter both keys.

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
| POST | `/api/assistant/talk` | body: IMA ADPCM blocks (chunked) → `{"question","answer","audio"}`; adds `tts_error` when speech fails; 502 `{"error"}` when recognition or chat fails |
| GET | `/api/assistant/audio/{id}` | speech audio as IMA ADPCM blocks |

## Tests

```bash
pip install -r requirements.txt
python -m unittest discover -s tests
```

The tests cover the ADPCM codec (bit-exact with the firmware), the API flow, weather /
board / Home Assistant shaping, discovery broadcasts and the language-model request.
