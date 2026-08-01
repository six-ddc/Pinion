// web_admin_config.cc — 见头文件。可移植核心，无 httpd 依赖。

#include "web_admin_config.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "device_config.h"
#include "web_admin_fs.h"  // FormField（表单解析，与文件管理页共用）

namespace web_admin::config {
namespace {

using device_config::Field;

constexpr std::array<Field, 7> kFields = {Field::LlmApiKey,      Field::LlmBaseUrl,
                                          Field::LlmModelId,     Field::LlmModelsJson,
                                          Field::VoiceAppKey,    Field::VoiceAccessKey,
                                          Field::RadioList};

// 粘贴密钥常带首尾空白/换行——裁掉，否则会被原样拼进 HTTP 头（IsCleanSecret 会拒
// 换行，用户只会看到一句"非法"而不知为何）。
std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::string StatusJson() { return device_config::StatusJson(); }

int Save(const std::string& form_body, std::string& msg) {
    if (form_body.size() > kMaxFormBytes) {
        msg = R"({"error":"请求体过大"})";
        return 413;
    }

    // 两趟：先全量校验，再统一落盘。任一字段非法 → 一个字节都不写，避免"改了三项
    // 成功两项"的半成功状态。
    std::vector<std::pair<Field, std::string>> pending;
    for (Field f : kFields) {
        std::string v;
        if (!web_admin::fs::FormField(form_body, device_config::FieldName(f), v)) continue;
        v = Trim(v);
        if (!device_config::Validate(f, v)) {
            msg = std::string(R"({"error":"字段校验失败","field":")") + device_config::FieldName(f) +
                  R"("})";
            return 400;
        }
        pending.emplace_back(f, std::move(v));
    }
    if (pending.empty()) {
        msg = R"({"error":"没有可写入的字段"})";
        return 400;
    }

    for (const auto& [f, v] : pending) device_config::Set(f, v);

    msg = R"({"ok":true,"applied":)" + std::to_string(pending.size()) + "}";
    return 200;
}

}  // namespace web_admin::config
