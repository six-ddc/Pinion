#pragma once

// 平台信息小接口 -- main/display/** 必须保持平台无关（不得直接 include
// esp_app_desc 等 IDF 头），固件版本这类平台事实经此处声明、由平台侧实现：
//   设备: main/sys_info.cc (esp_app_get_description)
//   sim:  sim/shim/src/mhal_shim.cc (固定 "sim")
#ifdef __cplusplus
extern "C" {
#endif

// 固件版本串（静态存储，勿 free），如 "2.0.5"。
const char* pi_sys_fw_version(void);

#ifdef __cplusplus
}
#endif
