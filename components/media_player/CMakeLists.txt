idf_component_register(
    SRCS
        "src/media_controller.cc"
        "src/media_pump.cc"
        "src/media_resampler.cc"
        "src/media_file_source.cc"
        "src/media_http_esp.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    # metalio_hal: audio_pipeline 播放管线；esp_http_client + esp-tls: 网络电台流式拉取
    #（crt_bundle_attach）；pthread: std::thread 栈配置 esp_pthread_set_cfg。
    PRIV_REQUIRES metalio_hal esp_http_client esp-tls pthread)
