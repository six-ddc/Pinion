# web_admin — 设备 Web 后台（esp_http_server 薄壳 + 可移植 core）。一个服务两页：
# 「配置」大模型 / 语音密钥（web_admin_config），「文件」SD 卡音乐管理（web_admin_fs）。
#
# 独立组件而非塞进 main：把 esp_http_server 放在本组件的 PRIV_REQUIRES 里显式声明，
# 既拉起 esp_http_server 进构建、又不破坏 main 组件"自动依赖全部组件"的特殊语义
# （一旦给 main 显式 REQUIRES/PRIV_REQUIRES 就会关掉该自动依赖，代价过大）。
#
# 两个 core（web_admin_fs.* / web_admin_config.*）不依赖任何 httpd 类型，sim 侧另有
# POSIX socket 薄壳复用同一份 core + 内嵌网页（见 sim/shim/src/web_admin_httpd_sim.cc）。
idf_component_register(
    SRCS "web_admin_fs.cc" "web_admin_config.cc" "web_admin_httpd.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_timer metalio_hal device_config)
