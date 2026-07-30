#!/usr/bin/env bash
# pack_pi_c.sh — regenerate the vendored pi-c prebuilt artefacts in this
# component from a pi-c SOURCE checkout (private upstream). Run this whenever
# pi-c changes or the firmware's PI_FEATURE_* set changes.
#
# Produces, under this component:
#   include/pi/*.h  port/{esp32,posix}/*.h  third_party/cJSON/{cJSON.h,cJSON.c}
#   lib/esp32p4/libpi-c.a          (riscv32, this firmware's feature set)
#   lib/host-<arch>/libpi.a libpi_posix.a   (macOS host, pi-c host defaults)
#   LICENSE  NOTICE
#
# Usage:
#   ./pack_pi_c.sh [path-to-pi-c-src]      # default: ../../../../six-ddc/pi-c
#
# Requirements:
#   - ESP-IDF activated for esp32p4 (source your .idf-env.sh first), OR set
#     IDF_PATH. The P4 archive is built via tools/pic_p4_build (a throwaway IDF
#     project) with the feature flags in its sdkconfig.defaults — keep those in
#     sync with the firmware sdkconfig.
#   - cmake + a C toolchain + libcurl for the host archives.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI_C_SRC="${1:-$HERE/../../../../six-ddc/pi-c}"
PI_C_SRC="$(cd "$PI_C_SRC" && pwd)"

if [[ ! -f "$PI_C_SRC/CMakeLists.txt" ]]; then
    echo "ERROR: '$PI_C_SRC' is not a pi-c checkout (no CMakeLists.txt)" >&2
    exit 1
fi
echo "==> pi-c source: $PI_C_SRC"

HOST_ARCH="$(uname -m)"
HOST_LIBDIR="$HERE/lib/host-$HOST_ARCH"
mkdir -p "$HERE/include/pi" "$HERE/port/esp32" "$HERE/port/posix" \
         "$HERE/third_party/cJSON" "$HERE/lib/esp32p4" "$HOST_LIBDIR"

echo "==> 1/4 headers + license"
cp "$PI_C_SRC"/include/pi/*.h            "$HERE/include/pi/"
cp "$PI_C_SRC"/port/esp32/*.h            "$HERE/port/esp32/"
cp "$PI_C_SRC"/port/posix/*.h            "$HERE/port/posix/"
cp "$PI_C_SRC"/third_party/cJSON/cJSON.h "$PI_C_SRC"/third_party/cJSON/cJSON.c "$HERE/third_party/cJSON/"
cp "$PI_C_SRC/LICENSE" "$HERE/LICENSE"
cp "$PI_C_SRC/NOTICE"  "$HERE/NOTICE"

echo "==> 2/5 host archives ($HOST_ARCH)"
HB="$(mktemp -d)"
# Release (not RelWithDebInfo): pi-c is a PRIVATE upstream and DWARF debug info
# embeds absolute source paths — shipping it would leak the private repo's file
# tree (src/agent/pi_loop.c, src/ai/providers/*.c, …) plus the build machine's
# username into this public repo. Step 4/5 strips whatever survives.
cmake -S "$PI_C_SRC" -B "$HB" -DPI_BUILD_TESTS=OFF -DPI_BUILD_EXAMPLES=OFF \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$HB" --target pi pi_posix -j >/dev/null
cp "$(find "$HB" -name libpi.a       | head -1)" "$HOST_LIBDIR/libpi.a"
cp "$(find "$HB" -name libpi_posix.a | head -1)" "$HOST_LIBDIR/libpi_posix.a"
rm -rf "$HB"
echo "    -> $HOST_LIBDIR/{libpi.a,libpi_posix.a}"

echo "==> 3/5 P4 archive (esp32p4, riscv32)"
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "ERROR: IDF_PATH unset — source your ESP-IDF env (.idf-env.sh) first" >&2
    exit 1
fi
PB="$HERE/tools/pic_p4_build"
rm -rf "$PB/build" "$PB/sdkconfig"
PI_C_SRC="$PI_C_SRC" idf.py -C "$PB" build >/dev/null
cp "$(find "$PB/build" -name libpi-c.a | head -1)" "$HERE/lib/esp32p4/libpi-c.a"
rm -rf "$PB/build" "$PB/sdkconfig" "$PB/managed_components" "$PB/dependencies.lock"
echo "    -> $HERE/lib/esp32p4/libpi-c.a"

echo "==> 4/5 strip debug info (must not leak private pi-c paths)"
# Keeps the symbol table (linking needs it), drops DWARF. Verified below: any
# surviving '/Users', '/home' or 'pi-c/src' string is a hard failure — this repo
# is public, pi-c is not.
strip -S "$HOST_LIBDIR/libpi.a" "$HOST_LIBDIR/libpi_posix.a"
"${IDF_TOOLCHAIN_PREFIX:-riscv32-esp-elf-}strip" --strip-debug "$HERE/lib/esp32p4/libpi-c.a"
leaked=0
for a in "$HERE/lib/esp32p4/libpi-c.a" "$HOST_LIBDIR"/*.a; do
    n="$(strings -a "$a" | grep -cE '/Users/|/home/|pi-c/(src|port|include)' || true)"
    printf '    %-16s leaked-path-strings=%s\n' "$(basename "$a")" "$n"
    [[ "$n" == "0" ]] || leaked=1
done
if [[ "$leaked" != "0" ]]; then
    echo "ERROR: private source paths survived stripping — do NOT commit these archives" >&2
    exit 1
fi

echo "==> 5/5 done. Vendored artefacts:"
ls -la "$HERE/lib/esp32p4/libpi-c.a" "$HOST_LIBDIR"/*.a
echo "Reminder: P4 feature set is fixed in tools/pic_p4_build/sdkconfig.defaults;"
echo "keep it in sync with the firmware sdkconfig's CONFIG_PI_FEATURE_* flags."
