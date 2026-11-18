<p align="right">
  <a href="pomodoro-power-roadmap.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Pomodoro Power-Saving Roadmap (Plan)

Status: **planned, not implemented**. The current firmware only supports multiple
backlight-off levels (30s/60s/120s/5min/always-on); the CPU, LVGL, codec, and I2C
peripherals keep running. This document fixes the staged light/deep-sleep design and
timer-state recovery; each stage requires separate on-device authorization.

## Current state and goals

- Done: multi-level auto screen-off, OK long-press manual screen-off, first key only
  wakes, NVS persistence for durations/counter/theme/display mode.
- Missing: system-level sleep. The MCU and peripherals still draw power with the
  backlight off.
- Goal: reduce idle power without losing timer state, ghost key presses, or peripheral
  damage, and prove the gain by measurement.

## Stages

| Stage | Content | Timer recovery | Risk/effort |
| --- | --- | --- | --- |
| B1 light sleep | Automatic light sleep when idle, RAM retained; audio suspended via `bsp_audio_sleep()` / resumed via `bsp_audio_wake()` | Mostly unnecessary: `esp_timer` is RTC-compensated after light sleep, so existing deadline timestamps stay valid | Medium: verify ADC keys, I2C, and USB logging across sleep |
| B2 deep sleep (not RUNNING) | Terminal peripheral shutdown after an idle timeout, GPIO0 key wake; RTC memory holds frozen remaining time and UI state | Wake reboots the app; restore remaining time, phase, and page from RTC memory; key_guard blocks the wake key | Medium-high: shutdown order, re-init, and display flicker need per-item device acceptance |
| B3 (optional, deferred) deep sleep while RUNNING | Sleep until the next phase deadline with RTC timer wake | Advance the state machine by slept time, looping across phases; replay beeps after wake | High: no screen refresh while asleep; behaves like a timer, not a pomodoro clock |

Order: B1, measure, B2, re-evaluate. B3 is deferred by default.

## B1 light sleep design

1. Enter only when the backlight is off and no beep is playing; never while handling key
   events or while the beep task is active.
2. Prefer ESP-IDF automatic light sleep (`CONFIG_PM_ENABLE` + FreeRTOS tickless idle)
   over scattering `esp_light_sleep_start()` through the app loop.
3. Light sleep must **not** call terminal pin-release or LCD hold APIs; audio resumes
   only through `bsp_audio_wake()`. If the ADC keys use the oneshot driver, confirm
   sampling and events still work after light sleep.
4. `pomo_tick()` uses `esp_timer_get_time()`; IDF compensates elapsed time, and
   acceptance compares remaining-time error across sleep.

## B2 deep sleep design (not RUNNING)

Entry: READY or PAUSED past an idle timeout (independent of the backlight level, e.g.
5 minutes). Never while RUNNING.

Shutdown order (per the repository deep-sleep reference):

1. Configure wake sources: `esp_sleep_enable_gpio_wakeup()` (GPIO0 low) plus an optional
   RTC timer.
2. `bsp_battery_sleep()` -> `bsp_audio_sleep()` -> `bsp_audio_prepare_deep_sleep()` ->
   `bsp_i2c_prepare_deep_sleep()` -> `bsp_display_prepare_deep_sleep()`.
3. `bsp_button_prepare_deep_sleep()` stops the button device, releases the shared ADC
   unit, and returns GPIO0 to a pulled-up digital input. **While the ADC owns the pin the
   digital level always reads 0, so skipping this wakes the device immediately.**
4. `esp_deep_sleep_start()`; if it unexpectedly returns, restart instead of recovering
   released buses.

State save and restore:

- An `RTC_DATA_ATTR` struct holds a magic/version, `phase`, `state` (READY/PAUSED only),
  frozen `remaining_ms`, `work_min`, `break_min`, `done`, `theme`, `display_mode`, and a
  checksum. RTC memory survives deep sleep, avoiding NVS wear on every sleep cycle.
- Durations/counter/theme/display mode still go to NVS on change (unchanged), as the
  fallback after a checksum failure or a full power cycle.
- On wake, GPIO wake causes arm key_guard first; normal BSP init releases the LCD hold
  and rebuilds display/audio/I2C. Valid RTC data restores the frozen remaining time and
  page; otherwise fall back to READY with NVS durations.
- PAUSED restores as PAUSED with identical remaining time; READY shows the configured
  duration.

## Acceptance and measurement

- Build: `tools/validate.sh --all` stays green; new pure logic (serialization, restore,
  multi-phase settlement) passes host tests first.
- Device matrix, per sleep type: remaining-time error after wake, completed counter, no
  lost beeps, no ghost keys, no abnormal display flicker, consistent setup data, and ten
  consecutive sleep/wake cycles without a reboot loop.
- Power: compare identical scenarios (fixed 30 minutes, record battery-percentage drain;
  use a USB power meter for standby current when available). No claimed savings without
  measurements.
- Risk: firmware cannot remove amplifier standby, regulator quiescent, or external
  pull-up loads; the achievable gain must be measured on the target board.
