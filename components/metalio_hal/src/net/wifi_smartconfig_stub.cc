// 链接期补桩：真机 P4 侧走 esp_wifi_remote（ESP-Hosted 代理 C5 WiFi）——
// esp_wifi 组件在 `NOT CONFIG_ESP_WIFI_ENABLED` 分支下只编译
// wifi_default[_ap]/wifi_netif 三个文件（见 .esp-idf/components/esp_wifi/
// CMakeLists.txt 开头的 early return），src/smartconfig.c 整个不参与编译，
// 故 SC_EVENT / esp_smartconfig_stop() 这两个符号在本项目的链接产物里
// 原本不存在。
//
// managed_components/78__esp-wifi-connect 的
// WifiConfigurationAp::Stop()（wifi_configuration_ap.cc:818 起）无条件引用
// 了这两个符号——即使 StartSmartConfig() 从未被调用过（本仓库 network.cc
// 也确实从未调用它），sc_event_instance_ 恒为 nullptr，那段代码在运行时是
// 死路径，但链接器仍要求符号存在。组件本身不允许改动，这里在 metalio_hal
// 自己的源码里补最小桩满足链接：不实现真正的 SmartConfig 语义，
// esp_smartconfig_stop() 在从未 start 的前提下调用等价 no-op。
#include "esp_smartconfig.h"

ESP_EVENT_DEFINE_BASE(SC_EVENT);

esp_err_t esp_smartconfig_stop(void) { return ESP_OK; }
