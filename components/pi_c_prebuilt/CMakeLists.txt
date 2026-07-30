# pi_c_prebuilt — the pi-c agent runtime shipped as a PREBUILT static library.
#
# pi-c's source lives in a private upstream repo and is NOT vendored here. This
# component exposes pi-c's public headers and links a prebuilt archive:
#   - lib/esp32p4/libpi-c.a   (this ESP-IDF build; riscv32)
#   - lib/host-arm64/libpi*.a (consumed directly by sim/CMakeLists.txt on macOS)
# Regenerate all three from a pi-c checkout with ./pack_pi_c.sh (see that file).
#
# ESP note: cJSON comes from IDF's own `json` component (REQUIRES json), exactly
# as the source pi-c component did — third_party/cJSON is intentionally NOT on
# the ESP include path (that copy exists only for the host/sim build, which
# compiles cJSON.c directly). Keeping it off avoids a duplicate cJSON.h.

idf_component_register(
    INCLUDE_DIRS "include" "port/esp32" "port/posix"
    REQUIRES json esp_http_client esp-tls
    PRIV_REQUIRES mbedtls esp_timer
)

# Feature macros the archive was compiled with MUST be re-declared to callers so
# the pi-c headers they include expand identically to how the .a was built.
# Kconfig here mirrors pi-c/Kconfig, so the firmware sdkconfig stays the single
# source of truth. Changing any of these requires re-running pack_pi_c.sh with
# matching flags — a binary .a cannot be reconfigured after the fact.
set(PI_FEATURE_LIST IMAGES SESSION PARTIAL_JSON MODELS_JSON COMPAT PARALLEL_TOOLS
    COMPACTION SKILLS_IGNORE SUBAGENT OAUTH)
foreach(feat ${PI_FEATURE_LIST})
    if(CONFIG_PI_FEATURE_${feat})
        target_compile_definitions(${COMPONENT_LIB} INTERFACE PI_FEATURE_${feat}=1)
    endif()
endforeach()

# Import the prebuilt archive and link it into this (header-only) component so
# every dependent (main auto-requires it) resolves pi-c's symbols.
set(PI_C_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/lib/esp32p4/libpi-c.a")
if(NOT EXISTS "${PI_C_ARCHIVE}")
    message(FATAL_ERROR "pi_c_prebuilt: missing ${PI_C_ARCHIVE} — run ./pack_pi_c.sh <pi-c-src>")
endif()
add_prebuilt_library(pi_c_archive "${PI_C_ARCHIVE}"
    REQUIRES json esp_http_client esp-tls
    PRIV_REQUIRES mbedtls esp_timer)
target_link_libraries(${COMPONENT_LIB} INTERFACE pi_c_archive)
