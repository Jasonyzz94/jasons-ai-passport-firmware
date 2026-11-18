<p align="right">
  <strong>English</strong> · <a href="app-requirements.zh_CN.md">简体中文</a>
</p>

# Application requirements: Pomodoro Timer (feature/pomodoro-timer)

## Goal

Use the device as an offline pomodoro timer: a countdown screen, three-button controls,
an audible phase-change alert, a completed-session counter, and configurable screen-off
levels including an always-on mode.

## Controls and behavior

- Default 25-minute focus plus 5-minute break.
- Main screen `OK` click: start / pause / resume.
- Main screen `OK` long-press: turn the screen off immediately; the timer keeps running
  and any key wakes it (the first press only wakes, no action).
- Main screen Up / Down click: fine-tune the current phase duration in READY only,
  clamped to 1–60 minutes.
- Main screen Up long-press: open the setup screen, including while RUNNING; duration
  rows are read-only and dimmed while RUNNING.
- Main screen Down long-press: switch the artwork theme manually; themes also rotate
  automatically after each completed focus session.
- Setup screen has five rows: Focus, Break, Display, Help, Reset.
  - Up / Down click: move the selected row up/down, wrapping across the five rows.
  - `OK` click: change the selected row — minutes +1 with wraparound (not RUNNING,
    back to 1 after 60), Display cycles to the next level, Help opens the
    controls-help page (OK returns), Reset resets the phase and returns to main.
  - `OK` long-press: save and return to the main screen.
- Display levels: 30s / 60s / 120s / 5min / Always on, default 60s. Always on disables
  auto screen-off; manual screen-off still works.
- After the configured idle time the backlight turns off while the timer keeps running;
  the first key press only wakes the screen and performs no action, and RELEASE
  events do not wake it (so a long-press screen-off is not undone by letting go). This is
  backlight-off, not light/deep sleep; see
  [pomodoro-power-roadmap.md](pomodoro-power-roadmap.md) for the power-saving plan.
- Focus finished: increment the counter, beep three times, and start the break automatically.
- Break finished: beep twice and return to a READY focus phase, waiting for a manual start.
- Durations, the counter, the theme, and the Display level persist in NVS
  (namespace `pomodoro`).
- Battery percentage sits in the top-right corner; an unavailable fuel gauge shows
  `--`, and unavailable audio degrades to a silent phase change.

## Screen

A custom design (the baseline test menu and pixel demo shell are not reused).
Regular text uses Montserrat 14/20 with wrapping disabled to avoid overlap, while
the countdown uses a 5x7 glyph grid scaled by an integer factor, with a blinking
colon while running. Phase artwork is two-frame character-map sprite animation:
frames alternate every 400 ms while RUNNING, with a two-pixel bounce. Three themes
rotate — garden (tomato / coffee), cats (sitting / sleeping), and space (rocket /
moon) — switching automatically after each completed focus session or manually via
a Down long-press; the theme persists in NVS. Completed sessions show up to four
mini tomatoes plus a counter. The main screen also carries a progress bar, a status
line, and button hints; the setup screen shows five selectable cards, with a separate
controls-help page. All text is English.

## Structure

- `main/pomodoro.c/h`: pure state machine without ESP-IDF/LVGL dependencies.
- `main/main.c`: UI, input dispatch, beep task, NVS persistence, and screen-off control.
- `main/pixel_time.c/h`: large pixel countdown rendering from a 5x7 glyph grid.
- `main/pixel_art.c/h`: character-map sprite rendering and three two-frame animated themes.
- `tests/test_pomodoro.c`: host tests for the state machine, registered in `tools/validate.sh`.
- `docs/development/pomodoro-power-roadmap.md` / `.zh_CN.md`: power-saving roadmap.

## Acceptance

- `./tools/validate.sh --all` passes.
- On-device confirmation: overlap-free layout, start/pause/resume, OK long-press
  screen-off and wake, READY minute tuning, setup entry while RUNNING with read-only
  durations, every Display level plus always-on, Help page navigation, Reset execution,
  manual/automatic theme switching, phase-change beeps, settings and counter surviving
  reboot, and the battery reading.
