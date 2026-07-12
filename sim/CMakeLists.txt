# pi_sim — desktop (macOS) SDL2 simulator for the pi_screen UI.
#
# Compiles the firmware UI sources UNCHANGED against:
#   - LVGL from managed_components/lvgl__lvgl (exact device version, own SDL driver)
#   - pi-c's host build (libpi + libpi_posix, curl transport → real DeepSeek)
#   - sim/shim (FreeRTOS→pthreads, esp_log, Settings, IOExpander, audio/volc stubs)
#
#   cmake -S sim -B sim/build && cmake --build sim/build -j && ./sim/build/pi_sim
cmake_minimum_required(VERSION 3.20)
project(pi_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -O0 makes the 720x720 software render ~200ms/frame, starving every LVGL
# timer (indev reads included) — always build optimized by default.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

get_filename_component(CLAW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
get_filename_component(PI_C_DIR ${CLAW_ROOT}/../../six-ddc/pi-c ABSOLUTE)

if(NOT EXISTS ${CLAW_ROOT}/main/display/screen/pi_screen/pi_models_data.h)
    message(FATAL_ERROR "pi_models_data.h missing (gitignored secret) — required for the real-API build")
endif()
if(NOT EXISTS ${CLAW_ROOT}/managed_components/lvgl__lvgl/lvgl.h)
    message(FATAL_ERROR "managed_components/lvgl__lvgl missing — run one idf.py build first (or idf.py reconfigure)")
endif()

# ---- LVGL: the exact copy the device uses, desktop build path ----
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CLAW_ROOT}/managed_components/lvgl__lvgl lvgl EXCLUDE_FROM_ALL)

find_package(SDL2 REQUIRED)
target_link_libraries(lvgl PUBLIC SDL2::SDL2)

# ---- pi-c host libs (dual-mode CMake: non-ESP path) ----
set(PI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${PI_C_DIR} pi_c EXCLUDE_FROM_ALL)

# ---- the simulator ----
add_executable(pi_sim
    main.cc
    shim/src/freertos_shim.c
    shim/src/esp_shim.c
    shim/src/settings_shim.cc
    shim/src/ioexpander_shim.cc
    shim/src/audio_pipeline_shim.cc
    shim/src/volc_stub.cc
    # firmware UI sources, unchanged
    ${CLAW_ROOT}/main/display/screen/screen_util.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_screen.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_agent_task.c
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_mock_paced.c
    # lv_markdown component (markdown rendering for assistant replies)
    ${CLAW_ROOT}/components/lv_markdown/src/md_parser.cc
    ${CLAW_ROOT}/components/lv_markdown/src/md_inline.cc
    ${CLAW_ROOT}/components/lv_markdown/src/md_theme.cc
    ${CLAW_ROOT}/components/lv_markdown/src/md_view.cc
    ${CLAW_ROOT}/main/display/font/font_pi_mono_14.c
    ${CLAW_ROOT}/main/display/font/font_pi_mono_17.c
    ${CLAW_ROOT}/main/display/font/font_pi_clock_132.c
    ${CLAW_ROOT}/managed_components/78__xiaozhi-fonts/src/font_puhui_20_4.c
    ${CLAW_ROOT}/managed_components/78__xiaozhi-fonts/src/font_puhui_30_4.c
)

target_include_directories(pi_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/include   # must precede everything ESP-flavored
    ${CLAW_ROOT}/main/display/screen
    ${CLAW_ROOT}/main/display/screen/pi_screen
    ${CLAW_ROOT}/components/volc_speech/include # real volc headers, stub impls
    ${CLAW_ROOT}/components/lv_markdown/include
)

target_link_libraries(pi_sim PRIVATE lvgl pi pi_posix SDL2::SDL2)

# Offline deterministic replay of the two-turn mock script (pi_scr_mock.h)
# instead of the real API -- markdown-rendering fixture included. Usage:
#   cmake -S sim -B sim/build -DPI_SIM_MOCK=ON && cmake --build sim/build -j
option(PI_SIM_MOCK "pi_sim replays the built-in mock script (no network)" OFF)
if(PI_SIM_MOCK)
    target_compile_definitions(pi_sim PRIVATE PI_AGENT_TASK_USE_MOCK=1)
endif()

# ---- md_test: host unit tests for the LVGL-free markdown parser core ----
add_executable(md_test
    md_test.cc
    ${CLAW_ROOT}/components/lv_markdown/src/md_parser.cc
    ${CLAW_ROOT}/components/lv_markdown/src/md_inline.cc
)
target_include_directories(md_test PRIVATE ${CLAW_ROOT}/components/lv_markdown/include)
