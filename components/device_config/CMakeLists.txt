# device_config — 运行期配置（大模型 / 语音密钥）的 NVS 存取层。
#
# 独立组件：main（pi_agent_task.c / main.cc / pi_screen.cc）与 web_admin 都要用它，
# 放 main 里 web_admin 就取不到（组件不能反向依赖 main）。
#
# 只依赖 json(cJSON) + metalio_hal(Settings)，无 httpd / LVGL 依赖，故 sim 侧
# 直接把 device_config.cc 编进 pi_sim（见 sim/CMakeLists.txt）。
idf_component_register(
    SRCS "device_config.cc"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES metalio_hal)
