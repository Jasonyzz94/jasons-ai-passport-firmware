<p align="right">
  <strong>English</strong> · <a href="app-requirements.zh_CN.md">简体中文</a>
</p>

# Application requirements: pocket toolbox

This branch turns the device into a 12-in-1 toolbox. Every screen is a custom Chinese
UI (the baseline test menu is not reused). This document records implemented behavior
and acceptance status; hardware limits are in the
[hardware guide](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md).

## Global conventions

- Keys (physical order, top to bottom): Up ▲, Down ▼, OK.
- ▲▼ click: move / adjust; OK click: primary action; **long-press ▲: back**; long-press OK,
  long-press ▼ and double clicks are app-defined. The bottom hint bar shows the
  available actions (trailing items are dropped when they do not fit).
- While the screen is off, the first key only wakes it; the Bluetooth remote is the
  exception (keys are sent while the screen stays off).
- Status bar: time, background timer chip (pomodoro / countdown), Wi-Fi and Bluetooth
  icons, battery.
- The power key is a dedicated hardware switch that is not wired to the MCU, so
  firmware cannot see short presses and cannot use it to turn the screen off.

## Tools

| Tool | Main features |
| --- | --- |
| Pomodoro | Ring progress, long break every N, skip, auto start, 30 s re-alert until acknowledged, weekly stats; runs in the background |
| Clock & weather | Large clock, current weather and 4-day / hourly forecast, nightstand mode |
| Voice assistant | Hold OK to talk; uses the home relay server when found, otherwise the cloud directly (ASR → chat → TTS) |
| Bluetooth remote | Vibe coding, slides, media, camera; the device acts as a BLE keyboard |
| Sound tools | Spectrum and decibel meter, tuner, metronome |
| Device link | ESP-NOW peer to peer: walkie-talkie, quick messages, ping, focus together |
| Smart home / info board | Via the relay or directly to Home Assistant; the board shows local cards without a server |
| Wi-Fi analyzer, timer, random, settings | Channel load, stopwatch / countdown, dice / what to eat, setup and system info |

### Bluetooth remote · vibe coding

| Key | Click | Long press | Double click |
| --- | --- | --- | --- |
| ▲ | ↑ | back to the mode list | ↓ (when ▼ is the voice key) or Shift+Tab |
| ▼ (voice key) | pressing holds the chosen key, releasing releases it | same | same |
| OK | Enter | Esc (interrupt) | quick commands |

- The mode list opens picker pages that save the role of ▼ (voice key / down ↓) and the
  computer key the voice key holds (Fn, left/right Control, left/right Command,
  left/right Option).
- Fn uses an Apple vendor report; if that report map makes HID initialization fail,
  firmware falls back to the base map and the UI suggests Command.
- The ESP32-C3 only has BLE and no Classic Bluetooth headset profile, so the device
  cannot act as the computer's Bluetooth microphone.

## Network and relay server

- When Wi-Fi is configured, the device connects once at boot to sync time and turns the
  radio off only after the time is valid **and** the first relay discovery has finished
  (at most 60 s).
- Discovery: the device sends UDP 8788 queries to 255.255.255.255 and the subnet
  broadcast address, and listens for the server's announcement sent to UDP 8789 every
  3 s; it then pairs over HTTP 8787. The result appears on the discovery line of
  the device's About page.
- Without a server the device works directly: weather (typed city geocoded / manual
  lat-lon / IP lookup → Open-Meteo), info board (local cards), smart home (LAN Home
  Assistant) and the voice assistant (an OpenAI-compatible cloud, SiliconFlow by
  default).
- The voice assistant mode is selectable: automatic, Mac relay only, or direct cloud
  only. Direct cloud has separate endpoint/key pairs for chat and for voice
  (recognition and speech). An empty voice endpoint means the chat one; an empty voice
  key reuses the chat key only when both endpoints match. Setup page presets: DeepSeek
  chat + SiliconFlow voice, SiliconFlow for everything, OpenAI for everything (speech
  resampled from 24 kHz), custom. Changing an endpoint without entering a new key
  clears the old key.
- Direct recognition opens the HTTPS connection before it starts the microphone and
  beeps when recording starts; the dock shows "connecting" meanwhile.
- Errors name the step and the reason, such as "recognition: invalid key", "chat: out
  of credit", or "speech: network down"; a speech failure keeps the text answer. In
  relay mode the relay returns the same text in `error` / `tts_error`.
- Setup page: pops up automatically when a phone joins the device hotspot; status card,
  nearby networks, weather location (built-in city coordinates), assistant key and
  models, Home Assistant, relay address; it shows the connection result live. Leaving
  the network name empty saves only the other settings.

## Known limits and security

- The API key and Home Assistant token are stored in NVS in plain text; they are not
  echoed back to the web page, but storage is not encrypted.
- Direct weather uses TLS, which is tight without PSRAM; every app switch logs free
  heap and the largest block on the serial console.
- The direct cloud voice upload format (chunked multipart WAV) and speech parameters
  follow the provider documentation but have not been exercised against the real API.
  Direct recognition waits for the HTTPS handshake before recording, so each utterance
  starts after a one- to two-second delay.

## Acceptance status

- Build and host tests (icons, pomodoro, DSP, HID keymap, link protocol, weather
  shaping, button spike rejection, server tests): pass.
- Device tests: not run. Pending: sound tools no longer freeze, key responsiveness on
  Wi-Fi pages, BLE pairing and voice-key hold, Fn recognition on macOS, setup page
  popup, direct weather and cloud voice, two-device link.

Power planning: [pomodoro power roadmap](pomodoro-power-roadmap.md). Server:
[server/README.md](../../server/README.md).
