// device_config.h — 设备运行期配置（大模型 / 语音密钥）的唯一存取门面。
//
// 固件里不打包任何密钥：大模型 API Key 与火山语音 App Key/Access Key 都存 NVS
// （namespace "cfg"），由 Web 后台的配置页写入（见 components/web_admin）。开机
// 未配置时 pi_screen 显示扫码引导页。
//
// 三个消费方：
//   pi_agent_task.c（C）  → device_config_build_models_json()，喂 pi-c 的 pi_models_load
//   main.cc              → GetVoiceAppKey/GetVoiceAccessKey，注入 volc_speech
//   web_admin_config.cc  → Validate/Set/StatusJson，网页读写
//
// 双端可用：只依赖 Settings（sim 有对齐 shim）与 cJSON，无 httpd / LVGL 依赖。

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
#include <string>

namespace device_config {

// NVS 单值上限 4000B。models JSON 覆盖留足余量（与 pi_card 的 ui/pin 3072B 同量级）。
inline constexpr size_t kMaxKeyBytes = 512;
inline constexpr size_t kMaxBaseUrlBytes = 256;
inline constexpr size_t kMaxModelsJsonBytes = 3500;

enum class Field {
    LlmApiKey,       // 大模型 API Key
    LlmBaseUrl,      // 可选：覆盖模板里的 baseUrl（须 http(s)://）
    LlmModelsJson,   // 可选：整份 models JSON，覆盖内置模板（高级模式）
    VoiceAppKey,     // 火山语音 App Key
    VoiceAccessKey,  // 火山语音 Access Key
};

// HTTP 表单字段名（也是网页上的字段名）。
const char* FieldName(Field f);

std::string Get(Field f);

// 只校验、不落盘。空串（= 清除该项）总是合法。
bool Validate(Field f, const std::string& v);

// 校验并落盘；空串 = 清除该项。返回 false = 未过校验，未落盘。
bool Set(Field f, const std::string& v);

// main.cc 注入 volc_speech 用。
std::string GetVoiceAppKey();
std::string GetVoiceAccessKey();

bool LlmReady();    // 有整份 JSON 覆盖，或有 API Key
bool VoiceReady();  // 两个火山密钥都非空

// 合成交给 pi-c 的 models JSON（单行）。优先整份覆盖；否则内置模板注入 API Key /
// baseUrl。未配置 → 空串。覆盖串解析失败只告警并回落模板路径（坏配置不能卡开机）。
std::string BuildModelsJson();

// 密钥掩码，只保留尾 4 位（如 "****803d"）；空串 → 空串。给网页回显用。
std::string MaskSecret(const std::string& v);

// 配置状态 + 掩码，**不含明文密钥**。GET /api/config 的 body。
std::string StatusJson();

}  // namespace device_config

extern "C" {
#endif /* __cplusplus */

// C 桥（pi_agent_task.c 用）。返回 malloc 的单行 JSON，调用方负责 free；
// 未配置返回 NULL。
char* device_config_build_models_json(void);
bool device_config_llm_ready(void);
bool device_config_voice_ready(void);

#ifdef __cplusplus
}
#endif
