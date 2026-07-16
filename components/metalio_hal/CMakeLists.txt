set(SRCS
    "src/hal.cc"
    "src/gps.cc"
    "src/imu.cc"
    "src/settings.cc"
    "src/storage.cc"
    "src/sysmon.cc"
    "src/backlight.cc"
    "src/motor.cc"
    "src/backlight_facade.cc"
    "src/audio/audio_codec.cc"
    "src/audio/bt_audio_codec.cc"
    "src/audio/audio_facade.cc"
    "src/audio/audio_pipeline.cc"
    "src/bt/bt_module.cc"
    "src/display/display_hal.cc"
    "src/display/touch_feed.cc"
    "src/display/esp_lcd_nv3051f.c"
    "src/display/esp_lcd_fl7707n.c"
    "src/net/network.cc"
    "src/net/wifi_smartconfig_stub.cc"
    "src/net/nt26_modem.cc"
    "src/net/uart_eth_modem.cc"
    "src/power/power.cc"
    "src/power/bq27220_gauge.cc"
    "src/power/i2c_device.cc")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "src/display" "src/net" "src/bt" "src/audio" "src/power"
    # 公共头用到：driver(i2c/gpio/i2s)、esp_timer(IOExpander)、nvs_flash(settings)
    REQUIRES driver esp_timer nvs_flash
    PRIV_REQUIRES esp_lcd esp_event esp_netif esp_pm fatfs sdmmc lwip esp_hw_support
                  esp_ringbuf)

# src/net/wifi_smartconfig_stub.cc 补的 SC_EVENT/esp_smartconfig_stop 桩符号
# 只有在链接器扫描本组件归档文件（libmetalio_hal.a）时已经"待解析"才会被拉
# 取；而完整链接行里 78__esp-wifi-connect.a 引用这两个符号的最后一次扫描发
# 生在 libmetalio_hal.a 最后一次出现之后（多组件循环依赖用重复列出归档文件
# 而非 --start-group 处理，顺序由 ESP-IDF 生成，改不了），实测会漏拉取导致
# undefined reference。用 -u 在链接一开始就把这两个符号标记为"已引用"，
# 使其在本组件归档文件的第一次扫描即被拉取，不再依赖后续扫描顺序。
# 注意：必须用 target_link_options + SHELL: 把 "-u <sym>" 作为整体传递；若走
# target_link_libraries 逐项传，裸符号名会被 CMake 当成库变成 -l<sym>（报错
# cannot find -lSC_EVENT）。
target_link_options(${COMPONENT_LIB} INTERFACE
    "SHELL:-u esp_smartconfig_stop" "SHELL:-u SC_EVENT")
