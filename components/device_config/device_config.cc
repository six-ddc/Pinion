// device_config.cc — 运行期配置的 NVS 存取 + models JSON 合成 + 写入校验。
//
// 容错基调与 pi_card 的 ui/pin 一致：读到坏配置只告警并回落，绝不 assert——
// 一份坏 JSON 不能把设备卡在开机路上。

#include "device_config.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "cJSON.h"
#include "esp_log.h"

#include "device_config_models.h"
#include "device_config_radio.h"
#include "settings.h"

#define TAG "device_config"

namespace device_config {
namespace {

constexpr char kNs[] = "cfg";

// NVS key = 表单字段名，一处定义两处用（NVS key 上限 15 字符，都在范围内）。
const char* KeyOf(Field f) {
    switch (f) {
        case Field::LlmApiKey: return "llm_key";
        case Field::LlmBaseUrl: return "llm_base";
        case Field::LlmModelId: return "llm_model";
        case Field::LlmModelsJson: return "llm_json";
        case Field::VoiceAppKey: return "volc_app";
        case Field::VoiceAccessKey: return "volc_ak";
        case Field::RadioList: return "radio_json";
    }
    return "";
}

std::string GetRaw(Field f) { return Settings(kNs).GetString(KeyOf(f)); }

// 空值 = 删键（而非存空串），这样 GetString 的默认值语义与"未配置"一致。
void Put(Field f, const std::string& v) {
    Settings s(kNs, true);
    if (v.empty()) {
        s.EraseKey(KeyOf(f));
    } else {
        s.SetString(KeyOf(f), v);
    }
}

// 密钥/URL 会被原样拼进 HTTP(S) 请求头与 WSS 握手头，控制字符可造成头注入；
// sim 的 settings shim 又是按行存文件，换行会截断存储。两条理由都要求这里硬拒。
// 只用于密钥/URL：models JSON 里的换行是合法排版，靠 minify 去掉（cJSON 打印会把
// 串内控制字符转义，所以规范化后的入库串同样不含裸控制字符）。
bool IsCleanSecret(const std::string& v) {
    for (unsigned char c : v) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

// 解析 + 规范化（重新 minify）一份 models JSON。返回空串 = 非法。
// 规范化顺带保证入库是单行，彻底规避 sim settings shim 的换行截断。
std::string NormalizeModelsJson(const std::string& raw) {
    cJSON* root = cJSON_Parse(raw.c_str());
    if (root == nullptr) return "";
    std::string out;
    const cJSON* providers = cJSON_GetObjectItem(root, "providers");
    if (cJSON_IsObject(providers) && providers->child != nullptr) {
        char* min = cJSON_PrintUnformatted(root);
        if (min != nullptr) {
            out.assign(min);
            cJSON_free(min);
        }
    }
    cJSON_Delete(root);
    return out;
}

// agent 只用清单第一个模型（pi_agent_task 取 catalog[0]），所以"选模型"= 把选中项
// 排到 models 数组第一位。模板里没有的 ID 按第一个模型的参数克隆（DeepSeek 的
// compat/thinkingFormat 必须带上，否则流式直接失败）。
void ApplyModelChoice(cJSON* provider, const std::string& model_id) {
    cJSON* models = cJSON_GetObjectItem(provider, "models");
    if (!cJSON_IsArray(models) || models->child == nullptr) return;
    int idx = 0;
    for (cJSON* m = models->child; m != nullptr; m = m->next, idx++) {
        const cJSON* id = cJSON_GetObjectItem(m, "id");
        if (cJSON_IsString(id) && model_id == id->valuestring) {
            if (idx > 0) {
                cJSON* picked = cJSON_DetachItemFromArray(models, idx);
                cJSON_InsertItemInArray(models, 0, picked);
            }
            return;
        }
    }
    cJSON* clone = cJSON_Duplicate(models->child, /*recurse=*/1);
    if (clone == nullptr) return;
    cJSON_DeleteItemFromObject(clone, "id");
    cJSON_AddStringToObject(clone, "id", model_id.c_str());
    cJSON_DeleteItemFromObject(clone, "name");
    cJSON_AddStringToObject(clone, "name", model_id.c_str());
    cJSON_InsertItemInArray(models, 0, clone);
}

// 取模板并注入 API Key（及可选 baseUrl / 模型 ID 覆盖）到**第一个 provider**。
std::string TemplateWithKey(const std::string& api_key, const std::string& base_url,
                            const std::string& model_id) {
    cJSON* root = cJSON_Parse(ModelsTemplateJson());
    if (root == nullptr) {
        ESP_LOGE(TAG, "built-in models template is not valid JSON");  // 只可能是改坏了模板
        return "";
    }
    std::string out;
    cJSON* providers = cJSON_GetObjectItem(root, "providers");
    cJSON* provider = cJSON_IsObject(providers) ? providers->child : nullptr;
    if (provider != nullptr) {
        cJSON_DeleteItemFromObject(provider, "apiKey");
        cJSON_AddStringToObject(provider, "apiKey", api_key.c_str());
        if (!base_url.empty()) {
            cJSON_DeleteItemFromObject(provider, "baseUrl");
            cJSON_AddStringToObject(provider, "baseUrl", base_url.c_str());
        }
        if (!model_id.empty()) ApplyModelChoice(provider, model_id);
        char* min = cJSON_PrintUnformatted(root);
        if (min != nullptr) {
            out.assign(min);
            cJSON_free(min);
        }
    }
    cJSON_Delete(root);
    return out;
}

// 从一份 models JSON 里取第一个 provider 的第一个 model id（网页/引导页回显用）。
std::string FirstModelId(const std::string& json) {
    if (json.empty()) return "";
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) return "";
    std::string id;
    const cJSON* providers = cJSON_GetObjectItem(root, "providers");
    const cJSON* provider = cJSON_IsObject(providers) ? providers->child : nullptr;
    const cJSON* models = provider != nullptr ? cJSON_GetObjectItem(provider, "models") : nullptr;
    const cJSON* first = cJSON_IsArray(models) ? models->child : nullptr;
    const cJSON* item = first != nullptr ? cJSON_GetObjectItem(first, "id") : nullptr;
    if (cJSON_IsString(item) && item->valuestring != nullptr) id = item->valuestring;
    cJSON_Delete(root);
    return id;
}

// models JSON 的校验产物（规范化后的单行串）会被 Set 复用，避免解析两遍。
bool CheckModelsJson(const std::string& v, std::string* normalized) {
    if (v.size() > kMaxModelsJsonBytes * 3) return false;  // 早退，省下大串的解析开销
    std::string norm = NormalizeModelsJson(v);
    if (norm.empty()) return false;                        // 解析失败 / 缺 providers
    if (norm.size() > kMaxModelsJsonBytes) return false;   // 按 minify 后的真实入库体积算
    if (normalized != nullptr) *normalized = norm;
    return true;
}

// ---- 网络电台列表（JSON 数组，覆盖内置种子）--------------------------------
// 单字段体积上限，防单台把预算吃光；控制字符一律拒（会被原样拼进 HTTP 请求头）。
constexpr size_t kMaxRadioName = 64;
constexpr size_t kMaxRadioGenre = 32;
constexpr size_t kMaxRadioUrl = 512;

bool IsRadioUrl(const std::string& v) {
    if (v.empty() || v.size() > kMaxRadioUrl || !IsCleanSecret(v)) return false;
    return v.rfind("http://", 0) == 0 || v.rfind("https://", 0) == 0;
}

const char* RadioStr(const cJSON* obj, const char* key) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(it) && it->valuestring != nullptr) ? it->valuestring : "";
}

// 解析一份电台 JSON 数组 → out。strict=true：任一条目非法即整体失败（保存路径）；
// strict=false：跳过坏条目、保留合法项（加载路径，覆盖串已在保存时校验过）。
bool ParseRadioArray(const std::string& json, std::vector<RadioStation>* out, bool strict) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }
    bool ok = true;
    const cJSON* el = nullptr;
    cJSON_ArrayForEach(el, root) {
        std::string name = RadioStr(el, "name");
        std::string genre = RadioStr(el, "genre");
        std::string url = RadioStr(el, "url");
        bool valid = cJSON_IsObject(el) && !name.empty() && IsCleanSecret(name) &&
                     name.size() <= kMaxRadioName && genre.size() <= kMaxRadioGenre &&
                     IsCleanSecret(genre) && IsRadioUrl(url);
        if (!valid) {
            if (strict) {
                ok = false;
                break;
            }
            continue;
        }
        out->push_back({std::move(name), std::move(genre), std::move(url)});
    }
    cJSON_Delete(root);
    return ok && !out->empty();
}

// 规范化：只保留 name/genre/url 三键、固定次序、minify 单行——保证"内容相同 → 字节相同"
// （网页端字段次序无关），既规避 sim settings shim 的换行截断，也让"覆盖==默认"可比。
std::string BuildRadioJson(const std::vector<RadioStation>& list) {
    cJSON* arr = cJSON_CreateArray();
    if (arr == nullptr) return "";
    for (const RadioStation& s : list) {
        cJSON* o = cJSON_CreateObject();
        if (o == nullptr) break;
        cJSON_AddStringToObject(o, "name", s.name.c_str());
        cJSON_AddStringToObject(o, "genre", s.genre.c_str());
        cJSON_AddStringToObject(o, "url", s.url.c_str());
        cJSON_AddItemToArray(arr, o);
    }
    std::string out;
    char* min = cJSON_PrintUnformatted(arr);
    if (min != nullptr) {
        out.assign(min);
        cJSON_free(min);
    }
    cJSON_Delete(arr);
    return out;
}

// 校验产物（规范化单行串）会被 Set 复用；同 CheckModelsJson 的套路。
bool CheckRadioJson(const std::string& v, std::string* normalized) {
    if (v.size() > kMaxRadioJsonBytes * 3) return false;  // 早退
    std::vector<RadioStation> list;
    if (!ParseRadioArray(v, &list, /*strict=*/true)) return false;
    std::string norm = BuildRadioJson(list);
    if (norm.empty() || norm.size() > kMaxRadioJsonBytes) return false;
    if (normalized != nullptr) *normalized = norm;
    return true;
}

std::vector<RadioStation> DefaultStations() {
    std::vector<RadioStation> out;
    out.reserve(kRadioDefaultCount);
    for (size_t i = 0; i < kRadioDefaultCount; i++) {
        out.push_back({kRadioDefaults[i].name, kRadioDefaults[i].genre, kRadioDefaults[i].url});
    }
    return out;
}

// 生效列表（未缓存）：覆盖串能解析出 ≥1 台就用它，否则回落种子。
std::vector<RadioStation> LoadEffectiveRadio() {
    std::string override_json = GetRaw(Field::RadioList);
    if (!override_json.empty()) {
        std::vector<RadioStation> list;
        if (ParseRadioArray(override_json, &list, /*strict=*/false)) return list;
        ESP_LOGW(TAG, "stored radio json is corrupt, falling back to defaults");
    }
    return DefaultStations();
}

}  // namespace

const char* FieldName(Field f) { return KeyOf(f); }

std::string Get(Field f) { return GetRaw(f); }

bool Validate(Field f, const std::string& v) {
    if (v.empty()) return true;  // 清除该项，总是合法
    switch (f) {
        case Field::LlmApiKey:
        case Field::VoiceAppKey:
        case Field::VoiceAccessKey: return IsCleanSecret(v) && v.size() <= kMaxKeyBytes;
        case Field::LlmBaseUrl:
            if (!IsCleanSecret(v) || v.size() > kMaxBaseUrlBytes) return false;
            return v.rfind("http://", 0) == 0 || v.rfind("https://", 0) == 0;
        case Field::LlmModelId: return IsCleanSecret(v) && v.size() <= kMaxModelIdBytes;
        case Field::LlmModelsJson: return CheckModelsJson(v, nullptr);
        case Field::RadioList: return CheckRadioJson(v, nullptr);
    }
    return false;
}

bool Set(Field f, const std::string& v) {
    if (v.empty()) {
        Put(f, "");
        return true;
    }
    if (f == Field::LlmModelsJson) {
        std::string norm;
        if (!CheckModelsJson(v, &norm)) return false;
        Put(f, norm);  // 入库存规范化后的单行串
        return true;
    }
    if (f == Field::RadioList) {
        std::string norm;
        if (!CheckRadioJson(v, &norm)) return false;
        // 覆盖==默认：不入库，回落种子——网页「恢复默认」保存后 is_custom 干净归零。
        Put(f, norm == DefaultRadioJson() ? std::string() : norm);
        return true;
    }
    if (!Validate(f, v)) return false;
    Put(f, v);
    return true;
}

const std::vector<RadioStation>& RadioStations() {
    // magic static：开机首次调用解析并缓存，运行期不可变（Web 后台保存即重启后重载）。
    static const std::vector<RadioStation> cache = LoadEffectiveRadio();
    return cache;
}

std::string DefaultRadioJson() { return BuildRadioJson(DefaultStations()); }

std::string GetVoiceAppKey() { return GetRaw(Field::VoiceAppKey); }
std::string GetVoiceAccessKey() { return GetRaw(Field::VoiceAccessKey); }

bool LlmReady() {
    if (!GetRaw(Field::LlmModelsJson).empty()) return true;
    return !GetRaw(Field::LlmApiKey).empty() && !GetRaw(Field::LlmModelId).empty();
}

bool VoiceReady() { return !GetVoiceAppKey().empty() && !GetVoiceAccessKey().empty(); }

std::string BuildModelsJson() {
    std::string override_json = GetRaw(Field::LlmModelsJson);
    if (!override_json.empty()) {
        std::string norm = NormalizeModelsJson(override_json);
        if (!norm.empty()) return norm;
        ESP_LOGW(TAG, "stored models json is corrupt, falling back to template");
    }
    std::string key = GetRaw(Field::LlmApiKey);
    std::string model = GetRaw(Field::LlmModelId);
    if (key.empty() || model.empty()) return "";  // 模型无默认值：不知道用户用什么模型
    return TemplateWithKey(key, GetRaw(Field::LlmBaseUrl), model);
}

std::string MaskSecret(const std::string& v) {
    if (v.empty()) return "";
    if (v.size() <= 4) return "****";
    return "****" + v.substr(v.size() - 4);
}

std::string StatusJson() {
    const std::string key = GetRaw(Field::LlmApiKey);
    const std::string base = GetRaw(Field::LlmBaseUrl);
    const std::string model_id = GetRaw(Field::LlmModelId);
    const std::string json = GetRaw(Field::LlmModelsJson);
    const std::string app = GetVoiceAppKey();
    const std::string ak = GetVoiceAccessKey();

    cJSON* root = cJSON_CreateObject();
    cJSON* llm = cJSON_AddObjectToObject(root, "llm");
    cJSON_AddBoolToObject(llm, "configured", LlmReady());
    cJSON_AddStringToObject(llm, "key_mask", MaskSecret(key).c_str());
    cJSON_AddStringToObject(llm, "base_url", base.c_str());
    cJSON_AddStringToObject(llm, "model_id", model_id.c_str());
    cJSON_AddBoolToObject(llm, "json_override", !json.empty());
    cJSON_AddNumberToObject(llm, "json_bytes", static_cast<double>(json.size()));
    cJSON_AddStringToObject(llm, "model", FirstModelId(BuildModelsJson()).c_str());

    cJSON* voice = cJSON_AddObjectToObject(root, "voice");
    cJSON_AddBoolToObject(voice, "configured", VoiceReady());
    cJSON_AddStringToObject(voice, "app_mask", MaskSecret(app).c_str());
    cJSON_AddStringToObject(voice, "ak_mask", MaskSecret(ak).c_str());

    // 电台：当前生效列表（覆盖 or 默认，供编辑器载入）+ 默认种子（供「恢复默认」）。
    const std::string rjson = GetRaw(Field::RadioList);
    const std::vector<RadioStation> eff = LoadEffectiveRadio();
    cJSON* radio = cJSON_AddObjectToObject(root, "radio");
    cJSON_AddBoolToObject(radio, "is_custom", !rjson.empty());
    cJSON_AddNumberToObject(radio, "count", static_cast<double>(eff.size()));
    cJSON_AddNumberToObject(radio, "bytes", static_cast<double>(rjson.size()));
    cJSON_AddNumberToObject(radio, "max_bytes", static_cast<double>(kMaxRadioJsonBytes));
    auto add_stations = [](cJSON* parent, const char* key, const std::vector<RadioStation>& list) {
        cJSON* arr = cJSON_AddArrayToObject(parent, key);
        for (const RadioStation& s : list) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", s.name.c_str());
            cJSON_AddStringToObject(o, "genre", s.genre.c_str());
            cJSON_AddStringToObject(o, "url", s.url.c_str());
            cJSON_AddItemToArray(arr, o);
        }
    };
    add_stations(radio, "stations", eff);
    add_stations(radio, "defaults", DefaultStations());

    cJSON* limits = cJSON_AddObjectToObject(root, "limits");
    cJSON_AddNumberToObject(limits, "key_max", static_cast<double>(kMaxKeyBytes));
    cJSON_AddNumberToObject(limits, "base_url_max", static_cast<double>(kMaxBaseUrlBytes));
    cJSON_AddNumberToObject(limits, "model_max", static_cast<double>(kMaxModelIdBytes));
    cJSON_AddNumberToObject(limits, "models_json_max", static_cast<double>(kMaxModelsJsonBytes));

    std::string out;
    char* text = cJSON_PrintUnformatted(root);
    if (text != nullptr) {
        out.assign(text);
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return out;
}

}  // namespace device_config

extern "C" char* device_config_build_models_json(void) {
    std::string json = device_config::BuildModelsJson();
    if (json.empty()) return nullptr;
    char* out = static_cast<char*>(malloc(json.size() + 1));
    if (out == nullptr) return nullptr;
    memcpy(out, json.c_str(), json.size() + 1);
    return out;
}

extern "C" bool device_config_llm_ready(void) { return device_config::LlmReady(); }

extern "C" bool device_config_voice_ready(void) { return device_config::VoiceReady(); }
