// ebook_screen.cc — 电子书 app 宿主：书架 ⇄ 阅读；阅读态维护 3 章节滑窗（当前/预载上一
// 章/预载下一章）+ 全局翻页定位，驱动后台 worker 并把结果喂给 reader_view。

#include "ebook_screen.h"

#include "book_config_server.h"
#include "book_store.h"
#include "bookshelf_view.h"
#include "ebook_font.h"
#include "ebook_ui_theme.h"
#include "ebook_worker.h"
#include "home_screen/home_screen.h"
#include "manage_view.h"
#include "reader_menu_view.h"
#include "reader_view.h"

#include "SdCardManager.hpp"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

constexpr char TAG[] = "ebook_ui";

// worker load 标签：区分当前章与预载邻章。
constexpr uint8_t kTagCur = 0;
constexpr uint8_t kTagPrev = 1;
constexpr uint8_t kTagNext = 2;

enum class View { Bookshelf, Reader, Manage };

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_bookshelf_root = nullptr;
lv_obj_t* s_reader_root = nullptr;
lv_obj_t* s_manage_root = nullptr;
View s_view = View::Bookshelf;

ReaderSettings s_settings;
PageMetrics s_metrics;
std::vector<BookInfo> s_books;

// 书架封面缩略（与 s_books 等长；px 归宿主所有）。生成链：一次只向 worker 发一本，
// 回调后再发下一本（队列容量小，串行不挤占开书/翻页命令）。
struct CoverSlot {
    uint8_t* px = nullptr;
    lv_image_dsc_t dsc = {};
};
std::vector<CoverSlot> s_covers;
std::vector<size_t> s_cover_pending;  // 待取封面的书下标（尾部弹出）

// 当前书 + 章节结构
BookInfo s_cur_book;
std::vector<ChapterEntry> s_chapters;
uint32_t s_virt_size = 0;  // 进度坐标系总长（TXT=文件大小 / EPUB=抽取文本总字节）
bool s_book_open = false;

// 3 章节滑窗（宿主拥有）。chapter_idx = -1 表示无效。
PaginatedChapter s_cur;
PaginatedChapter s_prev;
PaginatedChapter s_next;
// 延迟释放位：滑窗轮转淘汰的章节先挪到这里、下一轮再 FreeBuf。原因：插图页的 lv_image
// 直接引用章节拥有的像素/dsc，而轮转时对应 slot 的重挂载（lv_obj_clean）发生在
// on_commit 返回之后——立刻 FreeBuf 会让 slot 里的 lv 对象短暂持有悬垂指针。
// 推迟一轮后，其 slot 必已被重挂载。
PaginatedChapter s_graveyard;
int s_page_idx = 0;
bool s_reading = false;

// 进度落盘防抖（翻页时标脏，5s 定时器或退出时落盘）。
bool s_progress_dirty = false;
// 首次进 app 创建后**常驻不删**（OnScreenUnloaded 不再销毁它）：除进度落盘外，它还负责在退出
// app 后继续补发滞留的退休字体 fence（见 s_pending_fence），故必须活过 screen 生命周期。
lv_timer_t* s_save_timer = nullptr;

// 退休字体 fence：EnqueueFence 入队失败（worker 队列瞬时满）时暂存，OnSaveTick 里重试。
// blob 现在挂 MB 级 PSRAM，丢 fence = 旧 blob 泄漏，代价大，必须补发。取 max（fence 单调递增，
// OnWorkerFence 释放所有 fence<=id）。跨 screen load/unload 保留；因定时器常驻，即便用户退出
// app 后不再进入，也能在 worker 队列腾出后补发、回收 blob（把 PSRAM 还给相机等其它 app）。
uint32_t s_pending_fence = 0;

// 前置声明（早期回调引用后文定义）。
void ExitReaderToShelf();
void RenderTriple();
void ApplyFontSettings();

// 入队退休字体 fence；队列瞬时满导致失败则暂存到 s_pending_fence（取 max）等 OnSaveTick 补发。
void EnqueueFenceTracked(uint32_t fence) {
    if (fence == 0) return;
    if (!ebook_worker::EnqueueFence(fence) && fence > s_pending_fence) s_pending_fence = fence;
}

// ---- 基础工具 --------------------------------------------------------------
bool HolderValid(const PaginatedChapter& h, int expected_chapter) {
    return h.chapter_idx == expected_chapter && !h.pages.empty();
}

int ProgressPct(uint32_t file_off) {
    if (s_virt_size == 0) return 0;
    uint64_t p = static_cast<uint64_t>(file_off) * 100 / s_virt_size;
    return static_cast<int>(p > 100 ? 100 : p);
}

int ChapterOfOffset(uint32_t file_off) {
    for (size_t i = 0; i < s_chapters.size(); i++) {
        if (file_off >= s_chapters[i].byte_start && file_off < s_chapters[i].byte_end)
            return static_cast<int>(i);
    }
    return 0;
}

// 组装一格内容（pc=null → 空格）。
reader_view::SlotContent MakeSlot(const PaginatedChapter* pc, int page_idx) {
    reader_view::SlotContent sc;
    sc.pc = nullptr;
    if (pc == nullptr || page_idx < 0 || page_idx >= static_cast<int>(pc->pages.size()))
        return sc;
    sc.pc = pc;
    sc.page_idx = page_idx;
    uint32_t off = pc->pages[page_idx].byte_start;
    snprintf(sc.footer, sizeof(sc.footer), "%d%%   ·   %d/%d", ProgressPct(off), page_idx + 1,
             static_cast<int>(pc->pages.size()));
    return sc;
}

// 当前定位的下一页 / 上一页引用（跨章借用 prev/next 滑窗）。out_pc/out_page 有效返回 true。
bool NextRef(const PaginatedChapter** out_pc, int* out_page) {
    if (s_page_idx + 1 < static_cast<int>(s_cur.pages.size())) {
        *out_pc = &s_cur;
        *out_page = s_page_idx + 1;
        return true;
    }
    if (HolderValid(s_next, s_cur.chapter_idx + 1)) {
        *out_pc = &s_next;
        *out_page = 0;
        return true;
    }
    return false;
}

bool PrevRef(const PaginatedChapter** out_pc, int* out_page) {
    if (s_page_idx - 1 >= 0) {
        *out_pc = &s_cur;
        *out_page = s_page_idx - 1;
        return true;
    }
    if (HolderValid(s_prev, s_cur.chapter_idx - 1)) {
        *out_pc = &s_prev;
        *out_page = static_cast<int>(s_prev.pages.size()) - 1;
        return true;
    }
    return false;
}

reader_view::SlotContent NextSlot() {
    const PaginatedChapter* pc;
    int pg;
    return NextRef(&pc, &pg) ? MakeSlot(pc, pg) : reader_view::SlotContent{};
}
reader_view::SlotContent PrevSlot() {
    const PaginatedChapter* pc;
    int pg;
    return PrevRef(&pc, &pg) ? MakeSlot(pc, pg) : reader_view::SlotContent{};
}

void SaveProgressNow() {
    if (!s_reading || s_cur.pages.empty()) return;
    if (s_page_idx < 0 || s_page_idx >= static_cast<int>(s_cur.pages.size())) return;
    uint32_t off = s_cur.pages[s_page_idx].byte_start;
    book_store::SaveProgress(s_cur_book.hash, off, ProgressPct(off));
}

void MarkProgressDirty() { s_progress_dirty = true; }
void FlushProgress() {
    if (s_progress_dirty) {
        SaveProgressNow();
        s_progress_dirty = false;
    }
}
void OnSaveTick(lv_timer_t* /*t*/) {
    // 补发上次入队失败的退休字体 fence（成功后清零）。定时器常驻，此项即便在退出 app 后（s_screen
    // 为空）也要继续跑——否则滞留的 MB 级 blob 无人回收。worker 常驻，EnqueueFence 始终有效。
    if (s_pending_fence != 0 && ebook_worker::EnqueueFence(s_pending_fence)) s_pending_fence = 0;
    if (s_screen == nullptr) return;  // 已退出 app：仅补发 fence，跳过依赖活动 screen 的工作

    FlushProgress();
    // 字体曾因 PSRAM 并存挤占而降级到 SD 直连，现预算已恢复 → 升级为 PSRAM 字体（消除卡顿）。
    // ⚠️ 仅在**非阅读态**（书架）升级：升级要同步整读字体文件（~1s 冻结 LVGL 线程）+ 重排当前章，
    // 放在阅读中会突兀打断翻页。升级后渲染与降级态逐字一致（同一份字节，仅改读取来源），故延到
    // 离开当前书（回书架 / 下次开书）再做对体验无损；代价是阅读中若降级则维持到回书架才自愈。
    if (!s_reading && ebook_font::ShouldUpgrade()) ApplyFontSettings();
}

// ---- 预载邻章 --------------------------------------------------------------
void KickPreload(int dir) {
    int target = s_cur.chapter_idx + dir;
    if (target < 0 || target >= static_cast<int>(s_chapters.size())) return;
    if (dir > 0 && HolderValid(s_next, target)) return;
    if (dir < 0 && HolderValid(s_prev, target)) return;
    ebook_worker::TargetMode mode = (dir > 0) ? ebook_worker::kTargetFirst : ebook_worker::kTargetLast;
    ebook_worker::EnqueueLoadChapter(target, s_metrics, mode, 0, dir > 0 ? kTagNext : kTagPrev);
}

void RenderTriple() {
    reader_view::SetInitial(PrevSlot(), MakeSlot(&s_cur, s_page_idx), NextSlot(), s_metrics);
}

// ---- 章节轮转（提交跨章时，均在 LVGL 线程调用）------------------------------
void AdvanceChapter() {
    s_graveyard.FreeBuf();  // 上一轮淘汰章（其 slot 早已重挂载），此刻释放安全
    s_graveyard = std::move(s_prev);
    s_prev = std::move(s_cur);
    s_cur = std::move(s_next);
    s_next = PaginatedChapter{};  // 置空（缓冲已归 s_cur，不释放）
    s_page_idx = 0;
    KickPreload(1);
}

void RetreatChapter() {
    s_graveyard.FreeBuf();
    s_graveyard = std::move(s_next);
    s_next = std::move(s_cur);
    s_cur = std::move(s_prev);
    s_prev = PaginatedChapter{};
    s_page_idx = s_cur.pages.empty() ? 0 : static_cast<int>(s_cur.pages.size()) - 1;
    KickPreload(-1);
}

// ---- reader_view 回调（LVGL 线程）-----------------------------------------
bool CanTurn(int dir) {
    const PaginatedChapter* pc;
    int pg;
    return dir > 0 ? NextRef(&pc, &pg) : PrevRef(&pc, &pg);
}

bool OnCommit(int dir, reader_view::SlotContent* out_incoming) {
    const PaginatedChapter* pc;
    int pg;
    if (dir > 0) {
        if (!NextRef(&pc, &pg)) return false;
        if (pc == &s_cur) {
            s_page_idx++;
        } else {
            AdvanceChapter();
        }
        *out_incoming = NextSlot();
    } else {
        if (!PrevRef(&pc, &pg)) return false;
        if (pc == &s_cur) {
            s_page_idx--;
        } else {
            RetreatChapter();
        }
        *out_incoming = PrevSlot();
    }
    MarkProgressDirty();
    return true;
}

// 用当前字节偏移以新排版参数重载当前章（设置变更后保持阅读位置）。
void ReloadCurrentPreservingOffset() {
    if (!s_reading || s_cur.pages.empty()) return;
    if (s_page_idx < 0 || s_page_idx >= static_cast<int>(s_cur.pages.size())) return;
    uint32_t off = s_cur.pages[s_page_idx].byte_start;
    ebook_worker::EnqueueLoadChapter(s_cur.chapter_idx, s_metrics, ebook_worker::kTargetByOffset,
                                     off, kTagCur);
}

// 按当前 s_settings（字号档 + font_face）配置 FreeType 字体、重建排版参数并重排当前章。
// 用户字体文件缺失（被删/换卡）时静默回退内置。切字体/字号时旧 FT 字体经 fence 延迟释放 ——
// fence 必须在重排 LOAD 命令“之后”入队，保证 worker 排空到 fence 时旧字体已无测宽命令、
// 且新分页结果已 remount 换掉引用旧字体的 label（见 ebook_font.h 生命周期铁律）。
void ApplyFontSettings() {
    const ebook_ui::FontTier& bt = ebook_ui::FontTierAt(s_settings.font_idx);
    const ebook_ui::FontTier& ht =
        ebook_ui::FontTierAt(static_cast<uint8_t>(s_settings.font_idx + 1));
    bool want_ft = s_settings.font_face[0] != '\0' && ebook_font::Exists(s_settings.font_face);
    uint32_t fence = ebook_font::Configure(want_ft ? s_settings.font_face : "", bt.px, ht.px,
                                           bt.builtin_font, ht.builtin_font);
    s_metrics = ebook_ui::BuildMetrics(s_settings);
    ReloadCurrentPreservingOffset();  // 先入队重排（新字体）
    EnqueueFenceTracked(fence);       // 再入队 fence（旧字体延迟释放；失败暂存补发）
}

// ---- 菜单回调（LVGL 线程）--------------------------------------------------
void MenuBackToShelf() {
    reader_menu_view::Hide();
    ExitReaderToShelf();
}
void MenuSeek(int pct) {
    if (s_chapters.empty() || s_virt_size == 0) return;
    uint32_t off = static_cast<uint32_t>(static_cast<uint64_t>(pct) * s_virt_size / 100);
    int cidx = ChapterOfOffset(off);
    ebook_worker::EnqueueLoadChapter(cidx, s_metrics, ebook_worker::kTargetByOffset, off, kTagCur);
}
void MenuPrevChapter() {
    if (s_cur.chapter_idx > 0)
        ebook_worker::EnqueueLoadChapter(s_cur.chapter_idx - 1, s_metrics,
                                         ebook_worker::kTargetFirst, 0, kTagCur);
}
void MenuNextChapter() {
    if (s_cur.chapter_idx + 1 < static_cast<int>(s_chapters.size()))
        ebook_worker::EnqueueLoadChapter(s_cur.chapter_idx + 1, s_metrics,
                                         ebook_worker::kTargetFirst, 0, kTagCur);
}
void MenuSelectChapter(int idx) {
    if (idx >= 0 && idx < static_cast<int>(s_chapters.size()))
        ebook_worker::EnqueueLoadChapter(idx, s_metrics, ebook_worker::kTargetFirst, 0, kTagCur);
}
void MenuSetTheme(int idx) {
    s_settings.theme_idx = static_cast<uint8_t>(idx);
    book_store::SaveSettings(s_settings);
    reader_view::SetThemeIdx(static_cast<uint8_t>(idx));
    RenderTriple();  // 仅换色，无需重排版
}
void MenuSetFont(int idx) {
    s_settings.font_idx = static_cast<uint8_t>(idx);
    book_store::SaveSettings(s_settings);
    ApplyFontSettings();  // 字号档变化：FT 需按新 px 重建字体
}
void MenuSetFontFace(const char* filename) {
    strlcpy(s_settings.font_face, filename ? filename : "", sizeof(s_settings.font_face));
    book_store::SaveSettings(s_settings);
    ApplyFontSettings();
}
void MenuSetLineSpace(int idx) {
    s_settings.line_space_idx = static_cast<uint8_t>(idx);
    book_store::SaveSettings(s_settings);
    s_metrics = ebook_ui::BuildMetrics(s_settings);
    ReloadCurrentPreservingOffset();
}
void MenuSetMargin(int idx) {
    s_settings.margin_idx = static_cast<uint8_t>(idx);
    book_store::SaveSettings(s_settings);
    s_metrics = ebook_ui::BuildMetrics(s_settings);
    ReloadCurrentPreservingOffset();
}

void OnMenu() {
    // 载入失败/空内容态：菜单无法打开（s_reading=false），此时中区轻点即返回书架，
    // 给用户一个逃生出口（阅读视图不响应右滑返回）。
    if (!s_reading) {
        ExitReaderToShelf();
        return;
    }
    int pct = s_cur.pages.empty() ? 0 : ProgressPct(s_cur.pages[s_page_idx].byte_start);
    reader_menu_view::SetChapters(s_chapters, s_cur.chapter_idx);
    reader_menu_view::Show(s_settings, s_cur_book.name.c_str(), pct);
}

// ---- worker 回调（worker 线程、持 LVGL 锁、session 已校验）------------------
void OnBookReady(const ChapterIndex* idx, bool ok) {
    if (!ok || idx == nullptr) {
        reader_view::ShowLoading("打开失败\n请检查文件");
        return;
    }
    s_chapters = idx->chapters;
    s_virt_size = idx->virt_size;
    s_book_open = true;

    uint32_t off = book_store::LoadProgress(s_cur_book.hash);
    int chapter = 0;
    if (off > 0 && !s_chapters.empty()) {
        chapter = ChapterOfOffset(off);
    } else if (!s_chapters.empty()) {
        off = s_chapters[0].byte_start;
    }
    ebook_worker::EnqueueLoadChapter(chapter, s_metrics, ebook_worker::kTargetByOffset, off,
                                     kTagCur);
}

void OnChapterReady(PaginatedChapter* pc, int target_page, bool ok, uint8_t tag) {
    if (!ok || pc == nullptr) {
        if (tag == kTagCur) {
            s_reading = false;
            reader_view::ShowLoading("打开失败\n轻点屏幕中央返回书架");
        }
        // 预载失败：忽略（用户翻到边界时会重试）
        if (pc != nullptr) {
            pc->FreeBuf();
            delete pc;
        }
        return;
    }

    if (tag == kTagCur) {
        // 三格可能正挂着这些章节的 lv 对象（含引用章节像素的 lv_image）：先清 slot 再释放。
        reader_view::ClearSlots();
        s_graveyard.FreeBuf();
        s_prev.FreeBuf();
        s_prev = PaginatedChapter{};
        s_next.FreeBuf();
        s_next = PaginatedChapter{};
        s_cur.FreeBuf();
        s_cur = std::move(*pc);
        delete pc;
        if (s_cur.pages.empty()) {  // 空章/空文件
            s_reading = false;
            reader_view::ShowLoading("本章内容为空\n轻点屏幕中央返回书架");
            return;
        }
        s_page_idx = target_page;
        s_reading = true;
        RenderTriple();
        MarkProgressDirty();
        KickPreload(1);
        KickPreload(-1);
        return;
    }

    if (tag == kTagNext) {
        if (pc->chapter_idx == s_cur.chapter_idx + 1) {
            // 旧 s_next 理论上未挂载（HolderValid 不匹配才会预载），走墓地稳妥释放
            s_graveyard.FreeBuf();
            s_graveyard = std::move(s_next);
            s_next = std::move(*pc);
            delete pc;
            // 若用户正停在本章末页（右格原为空），就地补渲右邻页
            if (s_page_idx + 1 >= static_cast<int>(s_cur.pages.size()))
                reader_view::SetNeighbor(1, MakeSlot(&s_next, 0));
            return;
        }
    } else if (tag == kTagPrev) {
        if (pc->chapter_idx == s_cur.chapter_idx - 1) {
            s_graveyard.FreeBuf();
            s_graveyard = std::move(s_prev);
            s_prev = std::move(*pc);
            delete pc;
            if (s_page_idx == 0)
                reader_view::SetNeighbor(-1, MakeSlot(&s_prev,
                                                      static_cast<int>(s_prev.pages.size()) - 1));
            return;
        }
    }
    // 过期/不匹配：丢弃
    pc->FreeBuf();
    delete pc;
}

// ---- 导航 -------------------------------------------------------------------
void GoHome() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) lv_obj_delete_async(old_scr);
}

void ShowBookshelf() {
    s_view = View::Bookshelf;
    if (s_reader_root) lv_obj_add_flag(s_reader_root, LV_OBJ_FLAG_HIDDEN);
    if (s_bookshelf_root) lv_obj_remove_flag(s_bookshelf_root, LV_OBJ_FLAG_HIDDEN);
}

// ---- 书架封面（LVGL 线程；on_cover_ready 为 worker 持锁回调）-----------------
void FreeCovers() {
    // 调用前提：封面 lv_image 已随卡片删除（SetBooks/Clear 之后）
    for (auto& c : s_covers) {
        if (c.px != nullptr) heap_caps_free(c.px);
    }
    s_covers.clear();
    s_cover_pending.clear();
}

void KickNextCover() {
    if (s_view != View::Bookshelf) return;  // 阅读中暂停生成，回书架时重建链
    while (!s_cover_pending.empty()) {
        size_t idx = s_cover_pending.back();
        s_cover_pending.pop_back();
        if (idx >= s_books.size()) continue;
        const BookInfo& b = s_books[idx];
        ebook_worker::BookReq req = {};
        strlcpy(req.path, b.path.c_str(), sizeof(req.path));
        strlcpy(req.name, b.name.c_str(), sizeof(req.name));
        req.size = b.size;
        req.hash = b.hash;
        req.format = b.format;
        if (ebook_worker::EnqueueCover(req)) return;  // 串行：一次一本
        // 队满（罕见）：放回并放弃本轮，下次刷新书架时重启生成链
        s_cover_pending.push_back(idx);
        return;
    }
}

void OnCoverReady(uint32_t hash, uint8_t* px, uint16_t w, uint16_t h) {
    bool used = false;
    for (size_t i = 0; i < s_books.size(); i++) {
        if (s_books[i].hash != hash) continue;
        if (px != nullptr && w > 0 && h > 0 && i < s_covers.size()) {
            CoverSlot& slot = s_covers[i];
            if (slot.px != nullptr) heap_caps_free(slot.px);
            slot.px = px;
            slot.dsc = lv_image_dsc_t{};
            slot.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            slot.dsc.header.cf = LV_COLOR_FORMAT_RGB888;
            slot.dsc.header.w = w;
            slot.dsc.header.h = h;
            slot.dsc.header.stride = static_cast<uint32_t>(w) * 3;
            slot.dsc.data = px;
            slot.dsc.data_size = static_cast<uint32_t>(w) * h * 3;
            bookshelf_view::SetCover(i, &slot.dsc);
            used = true;
        }
        break;
    }
    if (!used && px != nullptr) heap_caps_free(px);
    KickNextCover();
}

void ShowReader() {
    s_view = View::Reader;
    if (s_bookshelf_root) lv_obj_add_flag(s_bookshelf_root, LV_OBJ_FLAG_HIDDEN);
    if (s_reader_root) lv_obj_remove_flag(s_reader_root, LV_OBJ_FLAG_HIDDEN);
}

void RefreshBookshelf() {
    s_books = book_store::ScanBooks();
    bookshelf_view::SetBooks(s_books, SdCardManager::GetInstance().IsMounted());  // 先删旧卡片
    FreeCovers();                                                                  // 再释放旧像素
    s_covers.resize(s_books.size());
    for (size_t i = s_books.size(); i > 0; i--) {  // 倒序压栈 → 正序弹出
        if (s_books[i - 1].format == BookFormat::kEpub) s_cover_pending.push_back(i - 1);
    }
    KickNextCover();
}

void ShowManage() {
    s_view = View::Manage;
    if (s_bookshelf_root) lv_obj_add_flag(s_bookshelf_root, LV_OBJ_FLAG_HIDDEN);
    if (s_reader_root) lv_obj_add_flag(s_reader_root, LV_OBJ_FLAG_HIDDEN);
    if (s_manage_root) lv_obj_remove_flag(s_manage_root, LV_OBJ_FLAG_HIDDEN);
    manage_view::Show(static_cast<int>(s_books.size()));
}

void ExitManageToShelf() {
    manage_view::Hide();  // 停止 Web 服务
    ShowBookshelf();
    RefreshBookshelf();  // 反映上传/删除后的书目
}

void FreeAllChapters() {
    reader_view::ClearSlots();  // 先删 slot 里的 lv 对象（可能引用章节像素），再释放
    s_cur.FreeBuf();
    s_prev.FreeBuf();
    s_next.FreeBuf();
    s_graveyard.FreeBuf();
    s_cur = PaginatedChapter{};
    s_prev = PaginatedChapter{};
    s_next = PaginatedChapter{};
    s_graveyard = PaginatedChapter{};
}

void ExitReaderToShelf() {
    reader_menu_view::Hide();
    FlushProgress();
    ebook_worker::BumpSession();  // 作废在途加载/预载
    s_reading = false;
    FreeAllChapters();
    ShowBookshelf();
    RefreshBookshelf();  // 回写进度到卡片
}

// ---- 书架回调 ---------------------------------------------------------------
void StartReading(const BookInfo& b) {
    s_cur_book = b;
    s_book_open = false;
    s_reading = false;
    s_page_idx = 0;
    FreeAllChapters();
    book_store::SaveLastBook(b.path);

    ShowReader();
    reader_view::ShowLoading("打开中…");

    ebook_worker::BookReq req = {};
    strlcpy(req.path, b.path.c_str(), sizeof(req.path));
    strlcpy(req.name, b.name.c_str(), sizeof(req.name));
    req.size = b.size;
    req.hash = b.hash;
    req.format = b.format;
    ebook_worker::EnqueueOpenBook(req);
}

void OnOpenBook(size_t idx) {
    if (idx >= s_books.size()) return;
    StartReading(s_books[idx]);
}

void OnBookshelfBack() { GoHome(); }
void OnBookshelfManage() { ShowManage(); }
void OnManageBack() { ExitManageToShelf(); }

void OnSwipeBack() {
    if (s_view == View::Reader) {
        ExitReaderToShelf();
        return;
    }
    if (s_view == View::Manage) {
        ExitManageToShelf();
        return;
    }
    GoHome();
}

// ---- 生命周期 ---------------------------------------------------------------
void OnScreenLoaded(lv_event_t* /*e*/) {
    s_settings = book_store::LoadSettings();

    ebook_worker::Callbacks cb;
    cb.on_book_ready = OnBookReady;
    cb.on_chapter_ready = OnChapterReady;
    cb.on_cover_ready = OnCoverReady;
    cb.on_fence = ebook_font::OnWorkerFence;
    ebook_worker::Begin(cb);

    ApplyFontSettings();  // 按设置激活用户 FT 字体（未设/缺失则内置）；此时 s_reading=false，无重排/fence

    if (s_save_timer == nullptr) s_save_timer = lv_timer_create(OnSaveTick, 5000, nullptr);

    s_view = View::Bookshelf;
    ShowBookshelf();
    RefreshBookshelf();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    FlushProgress();
    ebook_worker::BumpSession();
    // 注意：s_save_timer 不在此删除——它常驻以便退出 app 后继续补发滞留的退休字体 fence
    // （见 s_pending_fence / OnSaveTick）。OnSaveTick 内以 s_screen==nullptr 跳过 screen 相关工作。
    bookshelf_view::Clear();  // 先删卡片（含封面 lv_image），再释放像素
    bookshelf_view::Reset();
    reader_view::Reset();  // 删除所有引用 FT 字体的 label，之后退休字体才安全
    reader_menu_view::Reset();
    manage_view::Reset();  // 停止 Web 服务
    // 退出阅读器：释放用户 FT 字体（连同其 MB 级 PSRAM blob），把内存还给相机等其它 app。
    // BumpSession 已作废在途 LOAD 命令，但它们仍会在 worker 上完成测宽（引用退休字体）——
    // fence 入队在这些命令“之后”，保证 worker 排空到 fence 时已无测宽引用，deinit 才安全。
    EnqueueFenceTracked(ebook_font::Configure("", 0, 0, nullptr, nullptr));
    FreeAllChapters();
    FreeCovers();
    s_reading = false;
    s_book_open = false;
    s_books.clear();
    s_chapters.clear();
    s_virt_size = 0;
    s_screen = nullptr;
    s_bookshelf_root = nullptr;
    s_reader_root = nullptr;
    s_manage_root = nullptr;
}

}  // namespace

lv_obj_t* EbookScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, ebook_ui::kPanelW, ebook_ui::kPanelH);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_settings = book_store::LoadSettings();
    s_metrics = ebook_ui::BuildMetrics(s_settings);
    lv_obj_set_style_bg_color(scr, ebook_ui::Hex(ebook_ui::ThemeAt(s_settings.theme_idx).bg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    bookshelf_view::Callbacks bcb;
    bcb.on_open_book = OnOpenBook;
    bcb.on_back = OnBookshelfBack;
    bcb.on_manage = OnBookshelfManage;
    s_bookshelf_root = bookshelf_view::Build(scr, bcb, s_settings.theme_idx);

    manage_view::Callbacks vcb;
    vcb.on_back = OnManageBack;
    s_manage_root = manage_view::Build(scr, vcb, s_settings.theme_idx);

    reader_view::Callbacks rcb;
    rcb.on_commit = OnCommit;
    rcb.can_turn = CanTurn;
    rcb.on_menu = OnMenu;
    s_reader_root = reader_view::Build(scr, rcb, s_settings.theme_idx);

    reader_menu_view::Callbacks mcb;
    mcb.on_back_to_shelf = MenuBackToShelf;
    mcb.on_seek = MenuSeek;
    mcb.on_prev_chapter = MenuPrevChapter;
    mcb.on_next_chapter = MenuNextChapter;
    mcb.on_select_chapter = MenuSelectChapter;
    mcb.on_set_theme = MenuSetTheme;
    mcb.on_set_font = MenuSetFont;
    mcb.on_set_font_face = MenuSetFontFace;
    mcb.on_set_line_space = MenuSetLineSpace;
    mcb.on_set_margin = MenuSetMargin;
    reader_menu_view::Build(scr, mcb);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    return scr;
}

void EbookScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    ESP_LOGI(TAG, "%s: ebook_screen", event == SCREEN_LIFECYCLE_LOAD ? "load" : "unload");
}
