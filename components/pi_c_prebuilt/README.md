# pi_c_prebuilt

The **pi-c** agent runtime, shipped to this firmware as a **prebuilt static
library**. pi-c's source lives in a private upstream repo and is intentionally
**not** vendored here — only its public headers and compiled archives are.

## Layout

| Path | What |
|------|------|
| `include/pi/*.h` | pi-c public API headers |
| `port/esp32/pi_esp32.h`, `port/posix/pi_posix.h` | port entrypoints |
| `third_party/cJSON/` | cJSON (MIT) — headers + source; used by the host/sim build |
| `lib/esp32p4/libpi-c.a` | device archive (riscv32), this firmware's feature set |
| `lib/host-<arch>/libpi.a`, `libpi_posix.a` | macOS host archives for `sim/` |
| `CMakeLists.txt` | ESP-IDF component: imports the P4 archive + re-exports feature macros |
| `Kconfig` | `PI_FEATURE_*` options (mirrors pi-c/Kconfig; firmware sdkconfig is authoritative) |
| `pack_pi_c.sh` | regenerates everything above from a pi-c source checkout |
| `tools/pic_p4_build/` | throwaway IDF project that compiles the P4 archive |

## Consumers

- **Device (ESP-IDF):** `main` auto-requires this component; it links
  `lib/esp32p4/libpi-c.a` and defines the `PI_FEATURE_*` the archive was built
  with (driven by the firmware sdkconfig).
- **Simulator (`sim/CMakeLists.txt`):** defines IMPORTED targets `pi` /
  `pi_posix` from `lib/host-<arch>/*.a`; the subtree layout mirrors the pi-c
  source tree so `${PI_C_DIR}/include`, `/port/posix`, `/third_party/cJSON`
  resolve unchanged.

## Regenerating after a pi-c change

```bash
source ./.idf-env.sh                      # ESP-IDF for esp32p4 (P4 archive)
components/pi_c_prebuilt/pack_pi_c.sh /path/to/pi-c
```

The archives are architecture- and ABI-pinned: the host `.a` matches the build
machine's arch (`uname -m`), and the P4 `.a` matches the ESP-IDF toolchain +
the `PI_FEATURE_*` flags in `tools/pic_p4_build/sdkconfig.defaults` (keep those
in sync with the firmware sdkconfig). Changing any feature flag or the pi-c
source requires re-running `pack_pi_c.sh`.
