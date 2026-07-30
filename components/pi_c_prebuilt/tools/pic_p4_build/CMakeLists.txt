# Minimal ESP-IDF project whose ONLY purpose is to compile the pi-c source
# checkout into a standalone static library (libpi-c.a) for esp32p4, so
# pack_pi_c.sh can vendor a fresh prebuilt into ../../lib/esp32p4/ without
# building the whole firmware. Never flashed.
#
# The pi-c source path comes from the PI_C_SRC environment variable (set by
# pack_pi_c.sh). We add it as a component dir and prune COMPONENTS to pi-c +
# its transitive deps so the build stays small.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{PI_C_SRC})
    message(FATAL_ERROR "PI_C_SRC env var must point at the pi-c source checkout")
endif()
get_filename_component(PI_C_SRC_ABS "$ENV{PI_C_SRC}" ABSOLUTE)
if(NOT EXISTS "${PI_C_SRC_ABS}/CMakeLists.txt")
    message(FATAL_ERROR "PI_C_SRC='${PI_C_SRC_ABS}' has no CMakeLists.txt — not a pi-c checkout")
endif()

# Point EXTRA_COMPONENT_DIRS at the pi-c checkout directly (a dir with an
# idf_component_register CMakeLists is treated as a component named by its
# folder — normalize to 'pi-c' via a symlink-free direct entry).
set(EXTRA_COMPONENT_DIRS "${PI_C_SRC_ABS}")
# Limit the build graph to pi-c; IDF auto-pulls its REQUIRES (json,
# esp_http_client, esp-tls, mbedtls, esp_timer) and their transitive deps.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pic_p4_build)
