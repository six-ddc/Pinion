// stock_store.cc — 见头文件说明。

#include "stock_store.h"

#include "settings.h"

#include "esp_log.h"

#include <cstring>

namespace stock_store {
namespace {

constexpr char TAG[] = "stock_store";
constexpr char kNamespace[] = "stock";
constexpr char kKey[] = "stocks";

Entry s_list[kMaxStocks];
size_t s_size = 0;
size_t s_current_idx = 0;
const std::string kEmpty;

// TSV 文本 → entries。每行 `<symbol>\t<name>`。
size_t ParseTsv(const std::string& tsv, Entry* out, size_t cap) {
    size_t n = 0;
    size_t pos = 0;
    while (pos < tsv.size() && n < cap) {
        size_t nl = tsv.find('\n', pos);
        if (nl == std::string::npos) nl = tsv.size();
        std::string line = tsv.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        out[n].symbol = line.substr(0, tab);
        out[n].name = line.substr(tab + 1);
        if (!out[n].symbol.empty()) n++;
    }
    return n;
}

std::string BuildTsv(const Entry* entries, size_t count) {
    std::string tsv;
    for (size_t i = 0; i < count && i < kMaxStocks; i++) {
        if (entries[i].symbol.empty()) continue;
        tsv += entries[i].symbol;
        tsv += '\t';
        tsv += entries[i].name;
        tsv += '\n';
    }
    return tsv;
}

// NVS 为空时预置的默认自选股（首启体验，用户可在配置页增删）。
void SeedDebugIfEmpty() {
    if (s_size > 0) return;
    static const Entry kSeed[] = {
        {"sh600519", "贵州茅台"},
        {"hk00700", "腾讯控股"},
        {"usAAPL.OQ", "苹果"},
    };
    size_t n = sizeof(kSeed) / sizeof(kSeed[0]);
    for (size_t i = 0; i < n; i++) s_list[i] = kSeed[i];
    s_size = n;
    SaveToNvs(kSeed, n);
    ESP_LOGI(TAG, "seeded %u debug stocks", (unsigned)n);
}

}  // namespace

void Reload() {
    Settings s(kNamespace, false);
    std::string tsv = s.GetString(kKey, "");
    s_size = ParseTsv(tsv, s_list, kMaxStocks);
    SeedDebugIfEmpty();
    if (s_current_idx >= s_size) s_current_idx = 0;
    ESP_LOGI(TAG, "reload %u stocks", (unsigned)s_size);
}

size_t Count() { return s_size; }
bool Empty() { return s_size == 0; }

const std::string& SymbolAt(size_t idx) {
    return (idx < s_size) ? s_list[idx].symbol : kEmpty;
}
const std::string& NameAt(size_t idx) {
    return (idx < s_size) ? s_list[idx].name : kEmpty;
}

size_t CurrentIdx() { return s_current_idx; }
const std::string& CurrentSymbol() { return SymbolAt(s_current_idx); }
const std::string& CurrentName() { return NameAt(s_current_idx); }

bool SetCurrentIdx(size_t idx) {
    if (idx >= s_size) return false;
    s_current_idx = idx;
    return true;
}

void Next() {
    if (s_size == 0) return;
    s_current_idx = (s_current_idx + 1) % s_size;
}

void SaveToNvs(const Entry* entries, size_t count) {
    Settings s(kNamespace, true);
    std::string tsv = BuildTsv(entries, count);
    if (tsv.empty()) {
        s.EraseKey(kKey);
    } else {
        s.SetString(kKey, tsv);
    }
}

size_t LoadFromNvs(Entry* out, size_t cap) {
    Settings s(kNamespace, false);
    std::string tsv = s.GetString(kKey, "");
    return ParseTsv(tsv, out, cap);
}

}  // namespace stock_store
