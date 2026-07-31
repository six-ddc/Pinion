// device_config.h — 设备运行期配置（大模型 / 语音密钥）的唯一存取门面。
//
// 固件里不打包任何密钥：大模型 API Key 与火山语音 App Key/Access Key 都存 NVS
// （namespace "cfg"），由 Web 后台的配置页写入（见 components/web_admin）。开机
// 未配置时 pi_screen 显示扫码引导页。
//
// 消费方：
//   pi_agent_task.c（C）  → device_config_build_models_json()，喂 pi-c 的 pi_models_load
//   main.cc              → GetVoiceAppKey/GetVoiceAccessKey，注入 volc_speech
//   web_admin_config.cc  → Validate/Set/StatusJson，网页读写
//   pi_card_media.cc / pi_media.cc → RadioStations()，运行时生效的网络电台列表
//
// 双端可用：只依赖 Settings（sim 有对齐 shim）与 cJSON，无 httpd / LVGL 依赖。

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
#include <string>
#include <vector>

namespace device_config {

// NVS 单值上限 4000B。models JSON 覆盖留足余量（与 pi_card 的 ui/pin 3072B 同量级）。
inline constexpr size_t kMaxKeyBytes = 512;
inline constexpr size_t kMaxBaseUrlBytes = 256;
inline constexpr size_t kMaxModelsJsonBytes = 3500;
// 电台列表（JSON 数组）入库上限。默认 32 台约 3.6KB，留到 NVS 4000B 上限附近——
// 想加更多台需删减默认台（网页有实时字节计与超限报错）。
inline constexpr size_t kMaxRadioJsonBytes = 3960;

enum class Field {
    LlmApiKey,       // 大模型 API Key
    LlmBaseUrl,      // 可选：覆盖模板里的 baseUrl（须 http(s)://）
    LlmModelsJson,   // 可选：整份 models JSON，覆盖内置模板（高级模式）
    VoiceAppKey,     // 火山语音 App Key
    VoiceAccessKey,  // 火山语音 Access Key
    RadioList,       // 可选：整份网络电台 JSON 数组，覆盖内置种子；空 = 用默认种子
};

// 运行时电台条目（覆盖 / 种子解析后的产物）。
struct RadioStation {
    std::string name;   // 电台名（展示 + 模糊匹配）
    std::string genre;  // 分组（可空）：新闻 / 交通 / 音乐 / 综合
    std::string url;    // HLS m3u8 或 http(s) 直播流 URL
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

// 运行时生效的网络电台列表：NVS "radio_json" 有合法覆盖则解析它，否则回落内置种子
// （device_config_radio.h）。**开机首次调用时解析并缓存，之后返回同一份**——与 models
// 一致（开机加载一次、运行期不可变；Web 后台保存即重启后重新加载）。绝不返回空表：
// 覆盖损坏/为空都回落种子。线程安全（首帧构造由 magic static 保护）。
const std::vector<RadioStation>& RadioStations();

// 内置默认种子序列化成规范化 JSON（网页「恢复默认」的来源；也用于判定"覆盖==默认"）。
std::string DefaultRadioJson();

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
