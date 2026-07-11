// ebook_models.h
// 电子书 app 公共数据结构：书籍信息 / 章节表 / 分页结果 / 排版参数 / 阅读设置。
// 纯数据，无 lv_obj；UI 层与逻辑层（book_source / paginator / ebook_worker）共用。

#ifndef EBOOK_MODELS_H
#define EBOOK_MODELS_H

#include "lvgl.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 书籍正文编码（sidecar 缓存里以 u8 存储，勿改枚举值）。
enum class TextEncoding : uint8_t {
    kUtf8 = 0,
    kGbk = 1,
};

// 书籍格式（由扩展名判定；sidecar 里以 u8 存储，勿改枚举值）。
enum class BookFormat : uint8_t {
    kTxt = 0,
    kEpub = 1,
};

// 书架条目（ScanBooks 产出）。
struct BookInfo {
    std::string name;       // 文件名去掉扩展名（UTF-8，作为书名展示）
    std::string path;       // 完整 POSIX 路径，如 /sdcard/books/xxx.txt
    uint32_t size = 0;      // 文件字节数
    uint32_t hash = 0;      // FNV1a(文件名 + size)：进度 NVS key / sidecar 文件名
    int progress_pct = -1;  // 0~100；-1 = 未读过
    BookFormat format = BookFormat::kTxt;
};

// 章节表条目。byte_start/byte_end 的坐标系按格式二选一，语义对下游完全同构：
//   - TXT：**原文件**（原始编码域）的字节偏移；
//   - EPUB：**虚拟线性偏移** —— 全书各 spine 项抽取出的 UTF-8 纯文本首尾相接后的
//     累计字节偏移（见 epub_source）。
// 阅读进度与该坐标系一致，编码转换/重分页都不影响它。
// POD、按原样 fwrite 进 sidecar（布局即格式，改字段须 bump kSidecarVersion）。
struct ChapterEntry {
    uint32_t byte_start = 0;
    uint32_t byte_end = 0;      // 开区间上界（下一章起点或全书尾）
    uint32_t src_off = 0;       // EPUB：本条目在其 spine 项抽取文本内的起始字节（超大项切块用）
    uint16_t spine_idx = 0xFFFF;  // EPUB：所属 spine 下标；TXT 恒 0xFFFF
    char title[64] = {0};       // UTF-8 章节名，超长按码点边界截断补 "…"
};

// 一本书的章节索引（OpenBook 产出，sidecar 缓存往返）。
struct ChapterIndex {
    TextEncoding encoding = TextEncoding::kUtf8;
    uint32_t file_size = 0;
    uint32_t virt_size = 0;  // 进度坐标系总长：TXT = file_size；EPUB = 全书抽取文本总字节
    uint32_t name_hash = 0;  // FNV1a(文件名)，与 file_size 一起做缓存失效校验
    bool has_real_chapters = false;  // false = 按固定大小切块的伪章节
    std::vector<ChapterEntry> chapters;
};

// 排版参数。字号/行距/边距设置的展开形态，paginator 的唯一输入维度。
struct PageMetrics {
    const lv_font_t* font = nullptr;
    bool ft_font = false;     // true = font/heading_font 为 FreeType 用户字体（生命周期非永生，
                              // 由 ebook_font 的 fence/graveyard 管理；测宽线程安全靠 lv_freetype
                              // 内部 face_lock）。false = 内置点阵 const 字体（永生、只读安全）。
    int16_t line_height = 0;  // 行高：内置取字号档硬编码；FT 取 lv_font_get_line_height()
    int16_t line_space = 0;   // 行间距（叠加在 line_height 上）
    int16_t para_space = 0;   // 段落间额外间距
    int16_t content_w = 0;    // 正文区宽 = 720 - 2*margin_h
    int16_t content_h = 0;    // 正文区高 = 720 - 页眉 - 页脚 - 2*margin_v
    int16_t indent_w = 0;     // 段首缩进宽（像素，= 1 个全角字宽）。paginator 预留、reader 据此左移
                              // 行首。⚠️ 字库里 U+3000 是零宽空字形，缩进不能用空格字符实现。
    // 标题行（EPUB h1/h2）：正文字号档 +1（30 封顶）。
    const lv_font_t* heading_font = nullptr;
    int16_t heading_line_height = 0;
    // 页面底色（主题 bg，0xRRGGBB）：插图 PNG alpha 合成底色用。注意像素在解码时烘焙，
    // 切主题不重载章节则沿用旧底色（翻章后恢复；v1 接受）。
    uint32_t bg888 = 0xFFFFFF;
};

// 一个排版行。off/len 相对本章 UTF-8 缓冲；行内不含 '\n'。
struct LineSpan {
    uint32_t off = 0;
    uint16_t len = 0;
    uint8_t flags = 0;  // kLineFlag* 组合
};

constexpr uint8_t kLineFlagParaFirst = 0x01;  // 段首行（其上方加 para_space）
constexpr uint8_t kLineFlagParaLast = 0x02;   // 段末行
constexpr uint8_t kLineFlagImage = 0x04;      // 图片行：off 指向正文中的 U+FFFC 哨兵（len=3）
constexpr uint8_t kLineFlagHeading = 0x08;    // 标题行：用 heading_font 测宽/渲染
constexpr uint8_t kLineFlagCenter = 0x10;     // 居中行（标题）
constexpr uint8_t kLineFlagIndent = 0x20;     // 段首缩进行：渲染时行首左移 indent_w（此行已在分页时预留该宽）

// 章内插图（EPUB）。off 指向 utf8_buf 中的 U+FFFC 哨兵（3 字节，独立成段）。
// 两阶段填充（均在 worker 线程）：LoadChapter 填 src_path/src_w/src_h；分页前
// PrepareImages 按排版参数定 disp_w/h 并解码填 px/dsc。
// px 为解码+缩放后的 RGB888 像素（PSRAM，归 PaginatedChapter 所有，FreeBuf 释放）；
// px == nullptr 表示解码失败/超预算 → 渲染 placeholder（disp_w/h 仍有效，用于占位排版）。
// dsc 内嵌于元素：images 装载后不再增删，PaginatedChapter 整体 std::move 时 vector 堆内存
// 不动、元素地址不变，lv_image 引用 &dsc 安全。
struct ImageRef {
    uint32_t off = 0;
    std::string src_path;   // zip 内全路径（已归一化；空 = 引用无效）
    uint16_t src_w = 0;     // 原始尺寸（ProbeSize；0 = 未知格式/探测失败）
    uint16_t src_h = 0;
    uint16_t disp_w = 0;    // 显示（=像素 buf）尺寸，按正文区适配缩放后的值
    uint16_t disp_h = 0;
    uint8_t* px = nullptr;  // RGB888，heap_caps_free 释放
    lv_image_dsc_t dsc = {};
};

// 图片行上下留白（paginator 排版与 reader_view 渲染共用）。
constexpr int16_t kImageVMargin = 12;

// 标题段旁表条目（EPUB h1/h2；off/len 相对 utf8_buf，按 off 升序）。
// paginator 据此给行打 kLineFlagHeading|kLineFlagCenter。
struct HeadingSpan {
    uint32_t off = 0;
    uint32_t len = 0;
};

// 强调段旁表条目（EPUB <b>/<strong>/<i>/<em>；off/len 相对 utf8_buf，按 off 升序、互不重叠）。
// reader_view 据此把该字节区间用主题强调色（accent）渲染。bitmap 模式 FreeType 无法真加粗/
// 斜体，故 v1 以「换色」呈现强调；不参与分页测宽（换色不改字宽）。
struct EmphasisSpan {
    uint32_t off = 0;
    uint32_t len = 0;
};

// 一页 = lines 数组里的一个连续区间。byte_start 是该页第一行对应的
// **原文件**字节偏移 —— 阅读进度锚点。
struct PageSpan {
    uint32_t first_line = 0;
    uint16_t line_count = 0;
    uint32_t byte_start = 0;
};

// 一章的完整分页结果（utf8_buf 由 heap_caps 分配，归 PaginatedChapter 所有）。
//
// 原文件字节偏移映射：utf8_buf 是本章正文的 UTF-8 形态；每个页面 first line 的
// PageSpan.byte_start 记录该行首字符在**原文件**中的字节偏移（进度锚点）。映射由
// paginator 在一次前向遍历里算出，规则：
//   - UTF-8 源：file 前进量 = 该码点的 UTF-8 字节数（buf 即文件切片）
//   - GBK  源：file 前进量 = 码点 <0x80 ? 1 : 2（GBK 非 ASCII 恒为双字节）
struct PaginatedChapter {
    int chapter_idx = -1;
    TextEncoding encoding = TextEncoding::kUtf8;
    uint32_t chapter_file_start = 0;  // 本章正文起始偏移（TXT=文件域 / EPUB=虚拟域）
    char title[64] = {0};             // 章节名（worker 从索引拷入，供页眉显示）
    char* utf8_buf = nullptr;         // 本章正文（UTF-8），heap_caps_free 释放
    size_t utf8_len = 0;
    std::vector<LineSpan> lines;
    std::vector<PageSpan> pages;
    std::vector<ImageRef> images;      // 章内插图旁表（按 off 升序；仅 EPUB）
    std::vector<HeadingSpan> headings;  // 标题段旁表（按 off 升序；仅 EPUB）
    std::vector<EmphasisSpan> emphasis;  // 强调段旁表（按 off 升序、不重叠；仅 EPUB）

    void FreeBuf();
    // 找到包含指定原文件字节偏移的页下标（重分页后定位）；越界返回最近页。
    int FindPageByFileOffset(uint32_t file_off) const;
    // 按哨兵偏移找插图（二分；无匹配返回 nullptr）。
    const ImageRef* FindImage(uint32_t off) const;
};

// 阅读设置（NVS namespace "ebook"）。各字段是档位下标，展开见 ebook_ui_theme.h。
struct ReaderSettings {
    uint8_t font_idx = 1;        // 字号档 0/1/2/3 -> 20/30/40/50 号（"特大"50 仅 FT 字体可用）
    uint8_t theme_idx = 1;       // 0 白 / 1 米黄 / 2 夜间
    uint8_t line_space_idx = 1;  // 行距档
    uint8_t margin_idx = 1;      // 边距档
    // 用户 FreeType 字体文件名（/sdcard/books/ 下的 *.ttf/*.otf；空串 = 内置点阵字体）。
    // 文件缺失/SD 未挂载/创建失败时本次会话回退内置，设置保留。
    char font_face[64] = {0};
};

#endif  // EBOOK_MODELS_H
