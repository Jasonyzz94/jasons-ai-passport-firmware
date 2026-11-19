<p align="right"><a href="relay-network-review.zh_CN.md">Simplified Chinese</a> · <strong>English</strong></p>

# Relay Discovery and DeepSeek Feedback Review

**Purpose:** Give Claude the combined evidence and prior feedback for an independent review of the Passport message “server not found.” This is a diagnostic note, not a request to flash, revert, or publish firmware.

## Summary

The strongest current evidence points to a failure before pairing: the Mac relay did not log a discovery datagram from the Passport, and its pairing data directory was empty. The Mac was listening on the expected TCP and UDP ports, and its advertised LAN address matched the physical LAN interface when checked. The ordinary HTTP/SOCKS proxy is unlikely to affect UDP discovery. A VPN/TUN filter or router isolation remains possible, but is not proven.

DeepSeek's pasted handover describes a separate, uncommitted `feature/toolbox-fixes` worktree. That worktree is absent from the checkout reviewed here. Its build and host-test results are reported by the handover only; device validation is explicitly not run. Its direct-weather fallback may reduce dependence on the relay after it is implemented and flashed, but it does not establish or repair relay discovery.

## Evidence collected

Observations below were made on 2026-11-19 unless stated otherwise. Private addresses, device identifiers, and raw logs are intentionally omitted.

- The active checkout was clean at `3f2cf3b` on `ccr-75ff76be-rli4x3`. The advertised server address in the local configuration matched the Mac's LAN address at that time.
- A Python process held listeners on TCP 8787 and UDP 8788. The server log recorded startup and local/Mac discovery tests, but no discovery from the Passport. The pairing data directory had no files.
- The connected Espressif USB JTAG/serial device matched the Passport identity recorded in the handover. A 10-second passive read returned no bytes. No reset or flash command was issued. Serial settings could not be changed through the read-only handle.
- The Mac had an active `utun` interface, while routes for the Passport's LAN subnet used `en1`. No active HTTP/SOCKS/PAC proxy entries were visible in the inspected system-proxy output. This makes a conventional web proxy a weak candidate; a VPN client can still filter local broadcast traffic.
- A localhost HTTP probe from the restricted execution environment failed even though the listening socket was visible. Treat this as inconclusive about API responsiveness, not as proof that the relay was offline.
- The handover document reports firmware `3f2cf3b` on the device. That version was not independently confirmed over serial during this inspection.

## Interpreting “server not found”

The exact screen page matters:

- On the weather page, `app_clock.c` distinguishes no saved Wi-Fi, Wi-Fi still connecting, and a connected Wi-Fi link with an unavailable relay. If the user saw “server not found” on that page, the UI had reached the relay-unavailable branch.
- The board page uses a generic relay-unavailable message and does not by itself prove that Wi-Fi obtained an IP address.

On Wi-Fi `GOT_IP`, `esp_net.c` calls `relay_discover()`. The device then broadcasts on UDP 8788; the server replies with its URL; the firmware calls `/api/hello` over HTTP and pairs after an expected 401. See [`esp_net.c`](../../../main/esp_net.c), [`esp_relay.c`](../../../main/esp_relay.c), and [`discovery.py`](../../../server/passport_server/discovery.py).

No device discovery log plus no pairing file means the request did not reach the relay's discovery handler. Plausible causes still include missing/invalid saved Wi-Fi credentials, discovery not being triggered, broadcast suppression or client isolation on the AP, a local firewall/VPN filter, or a firmware mismatch. The available evidence does not distinguish these.

## Proxy and VPN assessment

The discovery step is UDP broadcast and does not use the Mac's HTTP/SOCKS proxy. The subsequent device-to-relay request uses a literal LAN URL, so the Mac's browser proxy settings are not automatically applied to it. A VPN/TUN application may filter or suppress local broadcast packets even when the LAN subnet route uses the physical interface. Temporarily pausing VPN/TUN and retrying discovery is a useful test; it is not yet a confirmed fix.

## DeepSeek feedback assessment

The pasted feedback claims these changes in another worktree:

- Direct Open-Meteo weather with a server-first fallback, plus portal scan and advanced settings.
- Per-request DeepSeek/OpenAI-compatible LLM settings.
- BLE HID report changes and audio-analysis optimizations.

Assessment:

- The no-PSRAM/TLS memory concern is plausible, but freeze attribution is still a hypothesis without device logs or heap measurements.
- The suspected BLE vendor-report failure is explicitly unconfirmed. Reverting the report map may reduce complexity, but does not prove BLE startup is fixed.
- Direct weather can make weather independent of the relay only if the implementation is present in the flashed firmware and the device can reach the external endpoints. It does not fix local relay discovery.
- The handover itself says device tests were not run. Its build and host-test passes are claims in the pasted note, not independently reproduced here.
- The stated plaintext API key in NVS and transmission over LAN HTTP is a material security tradeoff that should be reviewed before adopting those changes.

The current checkout has no `main/esp_weather.c`, no `feature/toolbox-fixes` branch/worktree, and no DeepSeek override headers in the inspected firmware/server source. Do not assume the pasted changes are installed on the Passport.

## Suggested diagnostic sequence

1. Read the Passport Settings values for Wi-Fi and relay status. Do not share the Wi-Fi password.
2. On the weather page, press OK to retry discovery while VPN/TUN is temporarily paused. Check whether the server logs a discovery from the device and whether the relay status changes.
3. If UDP discovery remains absent, use the current provisioning portal's optional server-address field to test a direct relay URL of the form `http://<Mac-LAN-address>:8787`. A successful direct connection would isolate the problem to discovery/broadcast. The current portal saves Wi-Fi fields together with the server URL, so re-enter the existing SSID and password; a blank password can overwrite the saved password.
4. If direct HTTP also fails, inspect TCP 8787 reachability, the Mac firewall, and router client isolation. Avoid changing firmware until this transport-level distinction is established.

## Questions for Claude

- Does the current firmware reliably call discovery after Wi-Fi obtains an IP, including after a failed initial attempt?
- What minimal observation distinguishes no Wi-Fi/IP, UDP broadcast blocked, and TCP relay failure without resetting or flashing the device?
- Is the provisioning portal's manual server URL path suitable as a diagnostic workaround, and what NVS side effects should be documented?
- Which DeepSeek changes are worth reviewing separately, and what evidence is needed before treating the BLE/freeze fixes as complete?
