// pi_sys_info.h 的设备侧实现（唯一允许碰 IDF 头的位置在 main/ 根，
// main/display/** 保持平台无关）。
#include <esp_app_desc.h>

#include "pi_screen/pi_sys_info.h"

extern "C" const char* pi_sys_fw_version(void) { return esp_app_get_description()->version; }
