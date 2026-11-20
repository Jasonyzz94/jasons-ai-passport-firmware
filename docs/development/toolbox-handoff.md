<p align="right">
  <a href="toolbox-handoff.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Pocket toolbox development handoff

The review is based on commit `a92d1f2` on branch `ccr-75ff76be-rli4x3`. The changes below are pending. The breathing and pixel pet apps are enabled in a separate local build. That build was flashed and reached `app_main()`; menu navigation and app interactions still need device acceptance.

## 1. Keep the last valid weather snapshot

The weather page currently keeps its response in RAM and replaces it on any HTTP 200 response. After a restart with no network, it has no snapshot to show. An incomplete response can also overwrite valid data. Start with [the weather page](../../main/app_clock.c) and [the storage helpers](../../main/esp_sys.c).

- Persist a versioned snapshot with the source location, data timestamp, current conditions, and dated forecasts. Restore it before the first network request. Validate the length and required fields before replacing it and limit writes to meaningful updates.
- Keep the snapshot after a timeout, disconnect, or invalid response. Mark it offline or stale and show its actual city and update date. When the location changes, retain the previous snapshot with its original city label until a valid replacement arrives.
- Use the same location selection for direct and relay weather, or make the active source explicit. The direct path reads the device settings. The relay uses its own `[location]`. [Direct request](../../main/esp_local.c) · [Relay configuration](../../server/config.example.toml)

**Acceptance.** Fetch weather, disconnect Wi-Fi, and restart. The last valid snapshot remains visible with its date. A malformed response leaves it intact. Reconnecting or selecting a different city updates the card only after a valid response.

## 2. Split QR setup into separate actions and save multiple Wi-Fi networks

The portal submits one form for every setting. It acknowledges receipt before persistence finishes. A new network replaces the only saved credentials before connection succeeds. Start with [the portal and network task](../../main/esp_net.c), [the device settings page](../../main/app_settings.c), and [settings persistence](../../main/settings.c).

| Menu | Actions |
| --- | --- |
| Wi-Fi | Show the active connection, saved networks, and nearby networks. Rescan, enter a hidden network, connect and save, change a password, or forget a network. |
| Voice assistant | Choose a mode, test recognition and chat, preview the voice, and save these settings separately. |
| Weather location | Choose automatic location or a city, save the location, and refresh weather. |
| More settings | Save the relay address, Home Assistant, and calendar settings independently. |
| Finish setup | Close the setup session and hotspot. Saving a section keeps the menu open. |

- Start with a proposed limit of five profiles. Migrate the existing SSID and password once. Keep the last successful profile and try it first on reconnect; after a bounded failure, try other known networks. Keep the current connection while it is healthy. Flag a full list and let users choose what to remove.
- Trial credentials live in memory. Commit a profile only after the intended network obtains an IP address. Restore the previous connection on failure. Use an operation ID to report progress and attach the result to the correct request. Report confirmed storage success separately from connection success.
- Validate each section on the device and propagate storage errors to the page. Distinguish an omitted password from an explicit open network. Use an authenticated setup session on a temporary password protected hotspot. Restrict access to the setup interface and keep stored secrets out of responses and logs.
- Run connection attempts and storage writes outside the display loop. Keep the portal available through a failed trial so the user can retry.

**Acceptance.** Upgrade a device with one saved network and confirm it still connects. Add a second network, verify both survive a restart, then take the first network out of range and verify fallback. Try an incorrect password and confirm the original profiles are still usable. Save weather or voice settings and confirm the Wi-Fi profiles are unchanged. Exercise add, forget, and finish on a phone with the real hotspot.

## 3. Make voice configuration testable

The direct client shares one base URL and key across recognition, chat, and speech synthesis. Its playback path expects 16 kHz PCM. The relay has separate providers for each stage and takes precedence today when available. [Direct voice client](../../main/esp_ai.c) · [Relay voice pipeline](../../server/passport_server/assistant.py)

- Offer explicit **Mac relay** and **device direct** modes. Explain which mode is active. For the relay, keep provider credentials on the Mac and make the portal test relay connectivity. The existing sample uses `mlx_whisper` for recognition on Apple silicon and macOS `say` with `Tingting` for speech. The chat provider is configured separately in `[llm]`. [Relay configuration example](../../server/config.example.toml)
- For direct setup, offer a provider preset with tested recognition, chat, and synthesis models and a voice picker. The existing SiliconFlow preset uses `FunAudioLLM/SenseVoiceSmall` for recognition and `FunAudioLLM/CosyVoice2-0.5B:anna` as its voice. Verify model availability with the configured account. [Recognition API](https://docs.siliconflow.cn/docs/api/audio-transcriptions-post) · [Speech API](https://docs.siliconflow.cn/docs/api/audio-speech-post)
- Put custom endpoints and model IDs under advanced settings. Validate recognition, chat, and synthesis separately. Require HTTPS for cloud credentials and bind credentials to their provider and endpoint. Switching providers requires the corresponding credential. Support a deliberate clear action.
- Verify the returned audio format and sample rate, adapting it to device playback as needed. Offer a short preview and keep the text answer visible when speech fails. Report authentication, timeouts, unavailable models, and audio failures at the affected stage.

**Progress.** Done: the auto, relay-only, and direct-only modes; separate chat and voice endpoint/key pairs; DeepSeek + SiliconFlow, SiliconFlow, OpenAI, and custom presets; key clearing when the endpoint changes; 24 kHz speech resampling; stage-specific errors on the device and from the relay; connecting before recording. Remaining: a portal test button and voice preview, an HTTPS requirement for cloud keys, and device acceptance.

**Acceptance.** Verify a short recording, a text reply, and voice playback in each supported mode. Check that an invalid key or wrong audio format produces a stage specific message. Check that changing the provider requires an explicit credential choice and that a synthesis failure still leaves the answer visible.

## Delivery

Implement weather persistence first. Follow with protected, section based setup and Wi-Fi profile migration, then add voice presets and diagnostics. Cover invalid snapshots, profile migration and fallback, and section update semantics in host tests. Run the [repository validation](engineering/build-and-test.md) and report physical device checks separately.

For device acceptance, use a partition compatible segmented flash when keeping NVS settings and cached data. Follow the [firmware layout and data policy](engineering/firmware-layout.md).
