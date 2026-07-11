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
  `mhal::network::StartAsync()` → `mhal::sysmon::Start()`), `display/screen/pi_screen/`,
  `display/screen/screen_util.{cc,h}`, 3 compressed pi fonts.
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

- UI code (pi_screen) talks to hardware only through the lib's public headers
  (`IOExpander.hpp` for PWR_KEY, `settings.h` for NVS persistence).
- The lib never calls into UI; anything it needs to report goes through registered callbacks
  (`mhal::network::OnEvent`, `mhal::bt::SetCallbacks`).
- pi_screen is the validated product UI — keep changes to it adaptation-only.

For every capability's API and a call example, read `docs/EXTRACTION.md` §2.
