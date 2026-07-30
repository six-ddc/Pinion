# pi 专属固件：main 只剩 UI（pi_screen + screen_util + pi 字体）与启动入口，
# 硬件能力全部在 components/metalio_hal。
set(SOURCES "display/screen/screen_util.cc"
            "display/screen/pi_screen/pi_screen.cc"
            "display/screen/pi_screen/pi_quick_panel.cc"
            "display/screen/pi_screen/pi_settings.cc"
            "display/screen/pi_screen/pi_media.cc"
            "display/screen/pi_screen/pi_media_focus.cc"
            "display/screen/pi_screen/pi_guide.cc"
            "display/screen/pi_screen/pi_net_events.cc"
            "display/screen/pi_screen/pi_sleep.cc"
            "display/screen/pi_screen/pi_theme.cc"
            "display/screen/pi_screen/pi_agent_task.c"
            "display/screen/pi_screen/pi_mock_paced.c"
            "display/screen/pi_screen/pi_card/pi_card_data.cc"
            "display/screen/pi_screen/pi_card/pi_card_icons.cc"
            "display/screen/pi_screen/pi_card/pi_card_actions.cc"
            "display/screen/pi_screen/pi_card/pi_card_render.cc"
            "display/screen/pi_screen/pi_card/pi_card_solver.cc"
            "display/screen/pi_screen/pi_card/pi_card_host.cc"
            "display/screen/pi_screen/pi_card/pi_card_preview.cc"
            "display/screen/pi_screen/pi_card/pi_card_preview_sig.cc"
            "display/screen/pi_screen/pi_card/pi_card_cmd.cc"
            "stock/stock_api.cc"
            "stock/stock_http_esp.cc"
            "stock/stock_tool.cc"
            "stock/stock_fetch_worker.cc"
            "display/screen/pi_screen/pi_card/pi_card_stock.cc"
            "display/screen/pi_screen/pi_card/pi_card_media.cc"
            "display/screen/pi_screen/pi_card/stock_chart_renderer.cc"
            "display/font/font_pi_mono_14.c"
            "display/font/font_pi_mono_17.c"
            "display/font/font_pi_mono_20.c"
            "display/font/font_pi_clock_132.c"
            "display/font/font_puhui_24_4.c"
            "display/font/font_pi_icons_16.c"
            "display/font/font_pi_icons_22.c"
            "display/font/font_pi_icons_28.c"
            "main.cc"
            "sys_info.cc"
            )

set(INCLUDE_DIRS "." "display" "display/font" "display/screen" "display/screen/pi_screen"
                 "display/screen/pi_screen/pi_card" "stock")

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    )
