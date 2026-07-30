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
# pi-c ships as a PREBUILT static library (private upstream, not vendored). The
# host archives + headers live under components/pi_c_prebuilt, mirroring the pi-c
# source subtree layout so every ${PI_C_DIR}/include, /port/posix and
# /third_party/cJSON reference below resolves unchanged. Regenerate the archives
# with components/pi_c_prebuilt/pack_pi_c.sh after any pi-c change.
get_filename_component(PI_C_DIR ${CLAW_ROOT}/components/pi_c_prebuilt ABSOLUTE)

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

# ---- pi-c host libs: PREBUILT static archives (was add_subdirectory of source) ----
# Expose IMPORTED targets named `pi` and `pi_posix` so the rest of this file is
# unchanged. The archives are produced by pack_pi_c.sh; on any pi-c change rerun
# it to refresh components/pi_c_prebuilt/lib/host-arm64/*.a.
set(PI_C_LIBDIR ${PI_C_DIR}/lib/host-arm64)
if(NOT EXISTS ${PI_C_LIBDIR}/libpi.a)
    message(FATAL_ERROR "prebuilt ${PI_C_LIBDIR}/libpi.a missing — run components/pi_c_prebuilt/pack_pi_c.sh")
endif()
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(pi STATIC IMPORTED)
set_target_properties(pi PROPERTIES
    IMPORTED_LOCATION ${PI_C_LIBDIR}/libpi.a
    INTERFACE_INCLUDE_DIRECTORIES "${PI_C_DIR}/include;${PI_C_DIR}/third_party/cJSON")
# libpi.a is built with pi-c's host defaults (all B-tier features ON). Re-declare
# them so caller TUs (pi_agent_task.c, pi_card_*) expand the pi-c headers exactly
# as the archive was compiled — a mismatch is silent struct-layout UB.
target_compile_definitions(pi INTERFACE
    PI_FEATURE_IMAGES=1 PI_FEATURE_SESSION=1 PI_FEATURE_PARTIAL_JSON=1
    PI_FEATURE_MODELS_JSON=1 PI_FEATURE_COMPAT=1 PI_FEATURE_PARALLEL_TOOLS=1
    PI_FEATURE_COMPACTION=1 PI_FEATURE_SKILLS_IGNORE=1 PI_FEATURE_HARNESS_TOOLS=1
    PI_FEATURE_HARNESS_EXEC=1 PI_FEATURE_SUBAGENT=1 PI_FEATURE_OAUTH=1)

add_library(pi_posix STATIC IMPORTED)
set_target_properties(pi_posix PROPERTIES
    IMPORTED_LOCATION ${PI_C_LIBDIR}/libpi_posix.a
    INTERFACE_INCLUDE_DIRECTORIES "${PI_C_DIR}/port/posix")
# libpi_posix.a's transport_curl.c pulls libcurl; pi provides the core symbols.
target_link_libraries(pi_posix INTERFACE pi Threads::Threads CURL::libcurl)

# ---- the simulator ----
add_executable(pi_sim
    main.cc
    shim/src/freertos_shim.c
    shim/src/esp_shim.c
    shim/src/settings_shim.cc
    shim/src/ioexpander_shim.cc
    shim/src/audio_pipeline_shim.cc
    shim/src/mhal_shim.cc
    shim/src/net_shim.cc
    # SD 音乐 Web 后台：可移植 core + sim POSIX socket 薄壳（复用同一份网页）
    ${CLAW_ROOT}/components/media_admin/media_admin_core.cc
    shim/src/media_admin_httpd_sim.cc
    shim/src/bt_shim.cc
    shim/src/volc_stub.cc
    # firmware UI sources, unchanged
    ${CLAW_ROOT}/main/display/screen/screen_util.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_screen.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_quick_panel.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_settings.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_media.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_media_focus.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_net_events.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_sleep.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_theme.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_agent_task.c
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_mock_paced.c
    # pi_card 声明式 UI 卡片框架（LLM 下发 JSON → LVGL 渲染）
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_data.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_icons.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_actions.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_render.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_solver.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_host.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_preview.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_preview_sig.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_cmd.cc
    # 股票行情（腾讯财经数据层 + stock tool + stock_chart 控件；HTTP 用 libcurl shim）
    ${CLAW_ROOT}/main/stock/stock_api.cc
    ${CLAW_ROOT}/main/stock/stock_tool.cc
    ${CLAW_ROOT}/main/stock/stock_fetch_worker.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_stock.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/stock_chart_renderer.cc
    shim/src/stock_http_curl.cc
    # media_player AI 集成层（media 工具 + media.* 路径/命令）
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_media.cc
    # media_player 媒体播放核心（可移植源 device/sim 同源；HTTP 流用 libcurl shim）
    ${CLAW_ROOT}/components/media_player/src/media_controller.cc
    ${CLAW_ROOT}/components/media_player/src/media_pump.cc
    ${CLAW_ROOT}/components/media_player/src/media_decoder_minimp3.cc
    ${CLAW_ROOT}/components/media_player/src/media_resampler.cc
    ${CLAW_ROOT}/components/media_player/src/media_file_source.cc
    ${CLAW_ROOT}/components/media_player/src/media_id3.cc
    ${CLAW_ROOT}/components/media_player/src/media_hls.cc
    ${CLAW_ROOT}/components/media_player/src/media_ts_demux.cc
    shim/src/media_http_curl.cc
    shim/src/media_http_fetch_curl.cc
    shim/src/media_decoder_sim.cc
    shim/src/media_decoder_atb_aac.cc
    shim/src/media_textconv_sim.cc
    # lv_markdown component (markdown rendering for assistant replies)
    ${CLAW_ROOT}/components/lv_markdown/src/md_parser.cc
    ${CLAW_ROOT}/components/lv_markdown/src/md_inline.cc
    ${CLAW_ROOT}/components/lv_markdown/src/md_theme.cc
    ${CLAW_ROOT}/components/lv_markdown/src/md_view.cc
    ${CLAW_ROOT}/main/display/font/font_pi_mono_14.c
    ${CLAW_ROOT}/main/display/font/font_pi_mono_17.c
    ${CLAW_ROOT}/main/display/font/font_pi_mono_20.c
    ${CLAW_ROOT}/main/display/font/font_pi_clock_132.c
    ${CLAW_ROOT}/main/display/font/font_puhui_24_4.c
    ${CLAW_ROOT}/main/display/font/font_pi_icons_16.c
    ${CLAW_ROOT}/main/display/font/font_pi_icons_22.c
    ${CLAW_ROOT}/main/display/font/font_pi_icons_28.c
    ${CLAW_ROOT}/managed_components/78__xiaozhi-fonts/src/font_puhui_20_4.c
    ${CLAW_ROOT}/managed_components/78__xiaozhi-fonts/src/font_puhui_30_4.c
)

target_include_directories(pi_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/include   # must precede everything ESP-flavored
    ${CLAW_ROOT}/main                          # stock/…（行情数据层）
    ${CLAW_ROOT}/main/stock
    ${CLAW_ROOT}/components/media_admin/include # media_admin_core.h / _httpd.h / _web.h
    ${CLAW_ROOT}/main/display/screen
    ${CLAW_ROOT}/main/display/screen/pi_screen
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card
    ${CLAW_ROOT}/components/volc_speech/include # real volc headers, stub impls
    ${CLAW_ROOT}/components/lv_markdown/include
    ${CLAW_ROOT}/components/media_player/include # media_player 公开契约
    ${CLAW_ROOT}/components/media_player/src     # minimp3.h / media_resampler.h / media_internal.h（私有）
)

# pi_posix INTERFACE-links pi (correct static-lib order), so listing pi again
# here would make ld warn about a duplicate archive — link pi_posix only.
target_link_libraries(pi_sim PRIVATE lvgl pi_posix SDL2::SDL2 iconv "-framework AudioToolbox" "-framework CoreFoundation")

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

# ---- id3_test: host unit tests + fuzz harness for the LVGL-free ID3 parser ----
add_executable(id3_test
    id3_test.cc
    ${CLAW_ROOT}/components/media_player/src/media_id3.cc
    shim/src/media_textconv_sim.cc
)
target_include_directories(id3_test PRIVATE
    ${CLAW_ROOT}/components/media_player/include
)
target_link_libraries(id3_test PRIVATE iconv)

# ---- ts_demux_test: TS 解封装 + HLS playlist 解析单测（真实 fixture + 合成 + 变异 fuzz，ASan/UBSan） ----
add_executable(ts_demux_test
    ts_demux_test.cc
    ${CLAW_ROOT}/components/media_player/src/media_ts_demux.cc
    ${CLAW_ROOT}/components/media_player/src/media_hls.cc
    shim/src/media_http_fetch_curl.cc
)
target_include_directories(ts_demux_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/include
    ${PI_C_DIR}/include
    ${PI_C_DIR}/port/posix
    ${CLAW_ROOT}/components/media_player/include
    ${CLAW_ROOT}/components/media_player/src
)
target_compile_options(ts_demux_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
target_link_options(ts_demux_test PRIVATE -fsanitize=address,undefined)
target_link_libraries(ts_demux_test PRIVATE curl)

# ---- media_stress: ASan/UBSan 泵切换压力测试（无头，复现真机切曲堆破坏用） ----
add_executable(media_stress
    media_stress.cc
    shim/src/audio_pipeline_shim.cc
    shim/src/media_http_curl.cc
    ${CLAW_ROOT}/components/media_player/src/media_controller.cc
    ${CLAW_ROOT}/components/media_player/src/media_pump.cc
    ${CLAW_ROOT}/components/media_player/src/media_decoder_minimp3.cc
    ${CLAW_ROOT}/components/media_player/src/media_file_source.cc
    ${CLAW_ROOT}/components/media_player/src/media_resampler.cc
    ${CLAW_ROOT}/components/media_player/src/media_hls.cc
    ${CLAW_ROOT}/components/media_player/src/media_ts_demux.cc
    shim/src/media_http_fetch_curl.cc
    shim/src/media_decoder_sim.cc
    shim/src/media_decoder_atb_aac.cc
)
target_include_directories(media_stress PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/include
    ${PI_C_DIR}/include
    ${PI_C_DIR}/port/posix
    ${CLAW_ROOT}/components/media_player/include
    ${CLAW_ROOT}/components/media_player/src
)
target_compile_options(media_stress PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
target_link_options(media_stress PRIVATE -fsanitize=address,undefined)
target_link_libraries(media_stress PRIVATE curl "-framework AudioToolbox" "-framework CoreFoundation")

# ---- card_solver_test: CARD V2 布局求解器（纯 C++、无 LVGL 依赖）单测 ----
# solver = 零 LVGL / 零 ESP 依赖的纯函数，复用 pi-c 里同一份 cJSON 源码，ASan/UBSan 全程开。
add_executable(card_solver_test
    tests/card_solver_test.cc
    tests/card_archetype_test.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_solver.cc
    ${PI_C_DIR}/third_party/cJSON/cJSON.c
)
target_include_directories(card_solver_test PRIVATE
    tests
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card
    ${PI_C_DIR}/third_party/cJSON
)
target_compile_options(card_solver_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
target_link_options(card_solver_test PRIVATE -fsanitize=address,undefined)

# ---- preview_prefix_test: CARD V2 流式预览签名机制(§4)+ 前缀不变量(P1-P3，§7.1) 宿主单测 ----
# 用 pi-c 真实的 partial JSON 解析器（pi_partial_json_parse_streaming，与 sim 主程序、真机
# pi_agent_task.c 走同一份实现）对语料逐字节/加步长切分喂流，验证 preview_sig 的签名判定 +
# solver::Solve 组合起来的"grid 整块重建"管线满足前缀不变量。链 `pi`（已含 cJSON + partial
# JSON，见 pi-c/CMakeLists.txt 的 add_library(pi ...)），不再单独编译 cJSON.c 以免重复符号。
add_executable(preview_prefix_test
    tests/preview_prefix_test.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_solver.cc
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card/pi_card_preview_sig.cc
)
target_include_directories(preview_prefix_test PRIVATE
    tests
    ${CLAW_ROOT}/main/display/screen/pi_screen/pi_card
)
target_compile_options(preview_prefix_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
target_link_options(preview_prefix_test PRIVATE -fsanitize=address,undefined)
target_link_libraries(preview_prefix_test PRIVATE pi)

