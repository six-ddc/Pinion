#include "pi_card_preview_sig.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace pi_card {
namespace preview_sig {

uint32_t Fnv1a(const char* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 16777619u;
    }
    return h;
}

uint32_t HashCompactJson(const cJSON* node) {
    if (!node) return 0;
    char* s = cJSON_PrintUnformatted(const_cast<cJSON*>(node));
    if (!s) return 0;
    uint32_t h = Fnv1a(s, std::strlen(s));
    cJSON_free(s);
    return h;
}

namespace {

// 递归收集 grid_json 子树里任意深度出现的 "bind_rows"/"bind_data" 字符串值（= data 里的
// key）。只看这两个键名，不关心它们出现在 cells/rows/item 里的哪一层——grid 深度浅（§1.1 树深
// 恒 2），遍历成本可忽略。
void CollectDataKeys(const cJSON* node, std::vector<std::string>& keys) {
    if (!node) return;
    if (cJSON_IsObject(node)) {
        for (const cJSON* child = node->child; child; child = child->next) {
            if (child->string != nullptr &&
                (std::strcmp(child->string, "bind_rows") == 0 ||
                 std::strcmp(child->string, "bind_data") == 0) &&
                cJSON_IsString(child) && child->valuestring != nullptr) {
                keys.emplace_back(child->valuestring);
            }
            CollectDataKeys(child, keys);
        }
    } else if (cJSON_IsArray(node)) {
        for (const cJSON* c = node->child; c; c = c->next) CollectDataKeys(c, keys);
    }
}

}  // namespace

uint32_t GridSignature(const cJSON* grid_json, const cJSON* data) {
    uint32_t sig = HashCompactJson(grid_json);
    if (!data) return sig;
    std::vector<std::string> keys;
    CollectDataKeys(grid_json, keys);
    if (keys.empty()) return sig;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (const auto& k : keys) {
        const cJSON* slice = cJSON_GetObjectItem(data, k.c_str());
        sig ^= HashCompactJson(slice);
    }
    return sig;
}

}  // namespace preview_sig
}  // namespace pi_card
