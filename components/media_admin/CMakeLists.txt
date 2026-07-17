# media_admin — SD 卡音乐文件 Web 管理后台（esp_http_server 薄壳 + 可移植 core）。
#
# 独立组件而非塞进 main：重构时把 httpd 依赖删光，隐式依赖链已不存在；把
# esp_http_server 放在本组件的 PRIV_REQUIRES 里显式声明，既拉起 esp_http_server
# 进构建、又不破坏 main 组件"自动依赖全部组件"的特殊语义（一旦给 main 显式
# REQUIRES/PRIV_REQUIRES 就会关掉该自动依赖，代价过大）。
#
# core（media_admin_core.*）不依赖任何 httpd 类型，sim 侧另有 POSIX socket 薄壳
# 复用同一份 core + 内嵌网页（见 sim/shim/src/media_admin_httpd_sim.cc）。
idf_component_register(
    SRCS "media_admin_core.cc" "media_admin_httpd.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_timer metalio_hal)
