idf_component_register(
    SRCS
        "src/media_controller.cc"
        "src/media_pump.cc"
        "src/media_resampler.cc"
        "src/media_file_source.cc"
        "src/media_http_esp.cc"
        "src/media_id3.cc"
        "src/media_textconv_device.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    # metalio_hal: audio_pipeline 播放管线；esp_http_client + esp-tls: 网络电台流式拉取
    #（crt_bundle_attach）；pthread: std::thread 栈配置 esp_pthread_set_cfg；fatfs: ID3
    # GBK 文本转 UTF-8 复用其 CP936 表（ff_oem2uni，media_textconv_device.cc）。
    PRIV_REQUIRES metalio_hal esp_http_client esp-tls pthread fatfs)
