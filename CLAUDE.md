# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for **Metalio Claw**, a palm-size ESP32-P4 device with a 720×720 MIPI-DSI touch screen.
The firmware is a **single-purpose pi Agent chat terminal**: it boots straight into
`main/display/screen/pi_screen/` (a four-state conversation UI driven by the
[pi-c](../../six-ddc/pi-c) agent runtime talking to the real DeepSeek API) — there is no menu, no
other screens. All hardware capability lives in a reusable local component **`components/metalio_hal/`**.

History: this repo started as a fork of xiaozhi-esp32 (Claw4/Claw5). The xiaozhi business layer
(Application/protocols/ota/mcp/audio sessions, 25+ app screens) was removed in the Claw6 refactor —
everything is recoverable from git history. **`docs/EXTRACTION.md` is the authoritative record** of
the refactor: as-built `mhal::` API (with call examples), old→new mapping, capability acceptance
matrix, asset-cleanup inventory.

Wi-Fi comes from an ESP32-C5 coprocessor over ESP-Hosted (SDIO); 4G from an NT26 module (UART
eth-modem). Network type is persisted in NVS `"network"/"type"` (0=WiFi, 1=4G; default 4G).

## Layout

- `main/` — UI only: `main.cc` (boot chain: nvs + event loop → `mhal::Init()` → load pi_screen →
  `mhal::network::StartAsync()` → `mhal::sysmon::Start()`), `display/screen/pi_screen/` (chat UI
  `pi_screen.cc` plus its satellites: `pi_quick_panel` quick panel, `pi_settings` six-page settings
  stack, `pi_sleep` screen-off state machine, `pi_net_events` network-event fan-out, `pi_theme`
  dual-theme tokens, `pi_sys_info.h`/`sys_info.cc` platform info), `display/screen/screen_util.{cc,h}`,
  3 compressed pi fonts.
- `components/metalio_hal/` — the hardware library. Public facade headers in
  `include/metalio_hal/` (`hal.h` `display.h` `backlight.h` `network.h` `bluetooth.h` `audio.h`
  `power.h` `sysmon.h`) plus pass-through `include/IOExpander.hpp`, `include/settings.h`,
  `include/audio_codec.h`. Private implementation in `src/`. The lib must stay free of any
  screen/UI/business references — notify upward only via registered callbacks.

## Secrets — never commit

`main/display/screen/pi_screen/pi_models_data.h` contains a **live DeepSeek API key**
(`PI_MODELS_JSON_TEXT`). It is gitignored and must exist on disk for real-API builds. Never stage
it, never delete it; check `git status` before every commit.

## Build / flash

```bash
source ./.idf-env.sh           # activate ESP-IDF v5.5.4 for this project (see below)
idf.py build                   # sdkconfig is pre-set; NEVER run idf.py set-target
idf.py -p /dev/ttyACM0 flash monitor   # P4 port = "USB JTAG/serial debug unit"; quit monitor with Ctrl+]
```

- **烧录后默认自动采集串口日志（别让设备裸奔）** —— 很多问题要运行一段时间才复现
  （如 TTS 播到一定时长掉声、内存缓慢泄漏），所以 `idf.py flash` 之后应立即起采集：
  `uv run tools/serial_cap.py`。该脚本用 uv 按 PEP 723 内联声明自动装 `pyserial`，且
  **断链/设备重启会自动重连、只有 Ctrl-C 才停**，因此可以在烧录前就起好、跨烧录连续
  抓取。写文件：`OUT=serial.log uv run tools/serial_cap.py`（`serial*.log` 已 gitignore，
  日志每行带相对时间戳 `[+SS.mmm]` 便于对齐复现时刻）。它与 `idf.py monitor` 抢同一个
  P4 串口，二选一——要**连续采集**就用 `idf.py flash`（不带 `monitor`）+ serial_cap.py。
- **Local env setup (`.idf-env.sh`)** — this repo uses a **project-local ESP-IDF** so it doesn't
  depend on a wrong-version global install. Both `.esp-idf/` and `.idf-env.sh` are git-ignored (via
  `.git/info/exclude`), so activation is one command: `source ./.idf-env.sh`. If they don't exist yet
  (fresh clone), recreate them:
  - Clone ESP-IDF **v5.5.4** into `.esp-idf/` — the pin is required: managed dependencies need
    `idf >=5.5.2` (uart-uhci UHCI driver).
    `git clone --branch v5.5.4 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git .esp-idf`
  - Install tools (reuses the global `~/.espressif` toolchain cache): `cd .esp-idf && ./install.sh esp32p4`
  - `.idf-env.sh` exports `IDF_PATH=.esp-idf`, `IDF_TOOLS_PATH=~/.espressif`,
    `IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.13_env`, then sources `export.sh`.
  - **Gotcha:** the machine's default `python3` is platformio's 3.11, which makes a plain `export.sh`
    hunt for a nonexistent `idf5.5_py3.11_env` and fail. `.idf-env.sh` prepends Homebrew python 3.13
    to `PATH` and pins `IDF_PYTHON_ENV_PATH` to sidestep this — don't drop those two lines.
- The device exposes **four** serial ports. Flash/monitor the P4 firmware on the *USB JTAG/serial
  debug unit* port. The other three (`CH340K USB Serial` = Bluetooth codec chip, `log` / `at` = NT26
  4G module) are for separate subsystems — do **not** flash P4 firmware to them.
- `pi-c` is a **path dependency** (`main/idf_component.yml`): the two repos must stay siblings —
  `Code/esp32/MetalioClaw6` ↔ `Code/six-ddc/pi-c`.
- Code style: `.clang-format` (Google-based, 4-space indent, 120 col). Format C/C++ before committing.

## Host simulator (sim/)

`sim/` builds the pi_screen UI **unchanged** into a macOS SDL2 window (LVGL taken straight from
`managed_components/lvgl__lvgl`, agent = pi-c POSIX port + libcurl → real DeepSeek):

```bash
cmake -S sim -B sim/build && cmake --build sim/build -j && ./sim/build/pi_sim
```

F1 = PWR_KEY click, F2 = PWR_KEY long-press (quick panel), typing while listening = speech
(1s pause auto-sends), F12 = screenshot. All
platform differences live in `sim/shim/` — never patch `main/display/**` for host reasons.
See `sim/README.md` for the interaction map and the unattended self-test/screenshot mode.

## sdkconfig — handle with care

The local `sdkconfig` is hand-tuned for this hardware and **not tracked by git**;
`sdkconfig.defaults` carries the full load-bearing set for reproducibility. **Never run
`idf.py set-target`** (regenerates sdkconfig, destroys hardware tuning). Load-bearing items you
must not lose: 32MB flash + `partitions/v1/32m.csv`, PSRAM HEX/200M/XIP full set,
`LV_COLOR_DEPTH_24`, `CONFIG_LV_USE_FONT_COMPRESSED=y` (the 3 pi fonts are RLE-compressed —
turning this off crashes the device on first label render), the whole ESP-Hosted C5 SDIO block
(pins CMD50/CLK51/D0-49/D1-34/D2-31/D3-53/RST54), `CONFIG_PI_FEATURE_{PARTIAL_JSON,MODELS_JSON,COMPAT}=y`
(from the pi-c component; the latter two are required for real DeepSeek).

## Architecture

Boot: `app_main` → `mhal::Init()` (I2C → TCA9555 power rails → BQ27220 gauge → battery boot guard
→ BT module UART → SD mount → DSI panel → GT911 → LVGL adapter → wireless-charger monitor →
backlight restore) → load pi_screen under `mhal::display::Lock()` with `screen_attach_lifecycle`
(the LOAD hook starts `pi_agent_task` and registers the PWR_KEY click) → `network::StartAsync()`.

Interaction shell around the chat UI: PWR_KEY long-press or status-bar pull-down opens
`pi_quick_panel` (brightness/volume sliders, theme ◐, gear → `pi_settings` six-page stack);
view navigation is **edge-swipe only** (indev-level layer in `screen_util`, no per-widget
opt-out tagging): swiping right from the left screen edge exits Chat to standby (or pops one
settings page), swiping left from the right edge returns Idle → Chat — interior horizontal
drags always belong to the widget under the finger;
`pi_sleep` dims then blanks the screen after the configured idle time (touch/PWR_KEY wakes, first
input is swallowed). `pi_theme::Init()` must run before any widget is built.

- UI code (pi_screen) talks to hardware only through the lib's public headers
  (`IOExpander.hpp` for PWR_KEY, `settings.h` for NVS persistence).
- The lib never calls into UI; anything it needs to report goes through registered callbacks
  (`mhal::network::OnEvent` — UI side multiplexes it via `pi_net_events` since it is a single
  overwrite-style callback — and `mhal::bt::SetCallbacks`).
- UI-owned NVS keys: `ui/theme` (0 dark / 1 light), `ui/sleep_s` (screen-off seconds, 0 = never),
  `bt/last_name` + `bt/last_addr` (UI-side cache of the last successfully connected BT device).
- pi_screen is the validated product UI — keep changes to it adaptation-only.

For every capability's API and a call example, read `docs/EXTRACTION.md` §2.
