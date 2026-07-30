// volc_keys_store.cc — volc_speech_keys.h 的实现：固定缓冲存放运行期注入的密钥。
//
// 固定缓冲（而非 std::string）是为了让 volc_speech_app_key() 返回的指针永不失效：
// 注入只发生在开机一次，ASR/TTS 任务在建连时读，二者不重叠；即便将来重复注入，
// 最坏也只是读到撕裂的字符串，而不会拿到悬垂指针。

#include "volc_speech_keys.h"

#include <cstddef>
#include <cstring>

#include "esp_log.h"

namespace {

constexpr char TAG[] = "volc_keys";
constexpr size_t kMax = 512;  // 与 device_config::kMaxKeyBytes 对齐

char s_app_key[kMax + 1];
char s_access_key[kMax + 1];

void CopyKey(char* dst, const char* src) {
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, kMax);
    dst[kMax] = '\0';
}

}  // namespace

extern "C" void volc_speech_set_keys(const char* app_key, const char* access_key) {
    CopyKey(s_app_key, app_key);
    CopyKey(s_access_key, access_key);
    // 只报是否齐备，绝不打印 key 值本身。
    ESP_LOGI(TAG, "keys %s", volc_speech_keys_ready() ? "configured" : "MISSING");
}

extern "C" bool volc_speech_keys_ready(void) {
    return s_app_key[0] != '\0' && s_access_key[0] != '\0';
}

extern "C" const char* volc_speech_app_key(void) { return s_app_key; }

extern "C" const char* volc_speech_access_key(void) { return s_access_key; }
