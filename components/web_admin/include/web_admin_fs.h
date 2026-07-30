// web_admin_fs.h — SD 卡音乐后台的**可移植**核心逻辑（不含任何 httpd 类型）。
//
// 设备端由 esp_http_server 薄壳（web_admin_httpd.cc）调用，sim 端由 POSIX
// socket 薄壳（sim/shim/src/web_admin_httpd_sim.cc）调用，同一套逻辑双端复用。
// 只依赖 <string> + POSIX 文件 API + mhal::storage（挂载点 / 剩余空间查询，双端均有）。
//
// 目录模型：SD 挂载点下两个根 Music/ 与 Podcasts/，浏览器只在这两棵子树里
// 列表 / 建目录 / 上传 / 删除。所有相对路径每一段都经 sanitize（禁
// ..、\\、控制字符、内嵌斜杠、空段；UTF-8 中文放行），首段必须是 Music|Podcasts。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace web_admin::fs {

inline constexpr const char* kRoots[2] = {"Music", "Podcasts"};

inline constexpr uint64_t kMaxUploadBytes = 200ull * 1024 * 1024;  // 单文件上限
inline constexpr uint64_t kSpaceSafetyBytes = 16ull * 1024 * 1024;  // 空间安全垫
inline constexpr size_t kUploadChunk = 8192;                        // 流式写盘分块
inline constexpr int kDeleteMaxDepth = 24;                          // 递归删深度上限（防环）
inline constexpr int kSweepMaxDirs = 4096;                          // 起服务孤儿清扫的目录数上限

// 校验相对路径 rel（形如 "Music/专辑A/01.mp3"）并解析为 SD 绝对路径。
// 合法 → true 并写 out_abs；非法（空、越权、控制字符、非根）→ false。
bool ResolvePath(const std::string& rel, std::string& out_abs);

// 相对路径末段（文件名）是否为 .mp3（大小写不敏感）。
bool HasMp3Suffix(const std::string& name);

// GET /api/list?dir=... 的 JSON body。dir 空 → 返回两个根目录。
// status 出参：200 正常 / 400 路径非法 / 404 目录不存在 / 503 未挂载。
std::string ListJson(const std::string& dir, int& status);

// GET /api/space 的 JSON body（total/free/mounted）。
std::string SpaceJson();

// POST /api/mkdir。逐级建父目录。返回 HTTP 状态码，msg 写 JSON body。
int Mkdir(const std::string& rel, std::string& msg);

// POST /api/delete。文件直接 unlink；目录仅空目录或 recursive=1 递归删。
// 返回 HTTP 状态码，msg 写 JSON body。
int Delete(const std::string& rel, bool recursive, std::string& msg);

// 流式上传。reader(buf,max) 拉取 body 字节：>0=读到的字节数，0=EOF，<0=出错/断开。
// content_len 为声明长度（空间/上限预检用）。overwrite 决定重名策略（0 → 已存在返 409）。
// 原子性：写同目录 <name>.part，fflush+fclose 落盘后再 rename 落位。覆盖已存在文件时先把
// 旧文件挪到 <name>.old 备份，rename(.part→目标) 成功才删备份、失败则把备份转回复原旧
// 文件——不存在"旧文件已毁、新文件未成"的窗口；传输/写盘出错则 unlink .part。
// 返回 HTTP 状态码，msg 写 JSON body。调用方须先 TryBeginUpload() 成功。
using ReadFn = std::function<int(char* buf, size_t max)>;
int Upload(const std::string& rel, uint64_t content_len, bool overwrite, const ReadFn& reader,
           std::string& msg);

// 起服务时调用：清扫 Music/Podcasts 子树下遗留的 <name>.part / <name>.old 孤儿
// （上次上传中途崩溃/掉电的半成品与覆盖备份）。显式栈迭代、无深递归。
void SweepOrphans();

// 并发互斥：同一时刻只允许一个 upload 在写。TryBeginUpload 返回 false → 调用方回 429。
// 成功须配对 EndUpload。
bool TryBeginUpload();
void EndUpload();

// 把裸字符串按 JSON 字符串内容转义（不含外层引号）追加到 out。工具函数，薄壳复用。
void JsonEscapeInto(std::string& out, const std::string& s);

// application/x-www-form-urlencoded 解码 / 取字段（双端薄壳复用）。
std::string UrlDecode(const std::string& s);
bool FormField(const std::string& body, const char* key, std::string& out);

}  // namespace web_admin::fs
