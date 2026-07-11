set(SRCS
    "src/hal.cc"
    "src/settings.cc"
    "src/sysmon.cc"
    "src/backlight.cc"
    "src/backlight_facade.cc"
    "src/audio/audio_codec.cc"
    "src/audio/bt_audio_codec.cc"
    "src/audio/audio_facade.cc"
    "src/bt/bt_module.cc"
    "src/display/display_hal.cc"
    "src/display/touch_feed.cc"
    "src/display/esp_lcd_nv3051f.c"
    "src/display/esp_lcd_fl7707n.c"
    "src/net/network.cc"
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
    PRIV_REQUIRES esp_lcd esp_event esp_netif esp_pm fatfs sdmmc lwip esp_hw_support)
