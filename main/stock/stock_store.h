// stock_store.h
// 自选股数据层：NVS 持久化（TSV）+ 跨视图共享的内存镜像 + 当前选中索引。
//
// 持久化：Settings("stock") 的 key "stocks"，格式 `<symbol>\t<name>\n...`，上限 16。
// 线程约定：仅 LVGL 线程读写内存镜像；配置服务器只写 NVS，回列表时 Reload。

#ifndef STOCK_STORE_H
#define STOCK_STORE_H

#include <cstddef>
#include <string>

namespace stock_store {

constexpr size_t kMaxStocks = 16;

// 从 NVS 重载内存镜像。currentIdx 越界时归 0。
void Reload();

// 查询。
size_t Count();
bool Empty();
const std::string& SymbolAt(size_t idx);  // 越界返回空串
const std::string& NameAt(size_t idx);

// 当前选中。
size_t CurrentIdx();
const std::string& CurrentSymbol();
const std::string& CurrentName();
bool SetCurrentIdx(size_t idx);  // 越界返 false
void Next();                     // 循环切下一支

// 写入 NVS（配置服务器用）；本函数不刷新内存镜像，需再 Reload。
// entries 为 [(symbol,name)]；count 上限 kMaxStocks。
struct Entry {
    std::string symbol;
    std::string name;
};
void SaveToNvs(const Entry* entries, size_t count);

// 直接读 NVS 到 out（配置服务器用，不碰内存镜像）。返回条数。
size_t LoadFromNvs(Entry* out, size_t cap);

}  // namespace stock_store

#endif  // STOCK_STORE_H
