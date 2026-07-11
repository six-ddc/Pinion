idf_component_register(
    SRCS
        "src/volc_proto.c"
        "src/volc_asr.cc"
        "src/volc_tts.cc"
        "src/volc_selftest.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    # json: cJSON（请求构造/响应解析）；esp_ringbuf: TTS 播放抖动队列；
    # metalio_hal: mhal::audio 放音/拾音；esp-tls: 证书包 crt_bundle_attach。
    PRIV_REQUIRES json esp_ringbuf esp_event esp-tls metalio_hal)

# 自测入口不被任何组件引用也强制保留符号，保证本组件在链接期被完整校验，
# 且固件里始终带着可触发的自测路径（后续接线阶段直接调用）。
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u volc_speech_selftest")
