# pi 专属固件：main 只剩 UI（pi_screen + screen_util + pi 字体）与启动入口，
# 硬件能力全部在 components/metalio_hal。
set(SOURCES "display/screen/screen_util.cc"
            "display/screen/pi_screen/pi_screen.cc"
            "display/screen/pi_screen/pi_agent_task.c"
            "display/screen/pi_screen/pi_mock_paced.c"
            "display/font/font_pi_mono_14.c"
            "display/font/font_pi_mono_17.c"
            "display/font/font_pi_clock_132.c"
            "main.cc"
            )

set(INCLUDE_DIRS "." "display" "display/font" "display/screen" "display/screen/pi_screen")

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    )
