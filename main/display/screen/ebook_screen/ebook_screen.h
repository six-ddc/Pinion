// ebook_screen.h
// 电子书阅读 app（720x720）。单 screen 宿主：书架视图 ⇄ 阅读视图。
// 从 SD 卡 /sdcard/books/*.txt 读 TXT（UTF-8 / GBK 自动识别）。

#ifndef EBOOK_SCREEN_H
#define EBOOK_SCREEN_H

#include "lvgl.h"
#include "screen_util.h"

class EbookScreen {
public:
    // 构建并返回 720x720 screen（parent=NULL）。
    static lv_obj_t* Create();

    // 生命周期观察（LOAD/UNLOAD），由 home_screen 注入。
    static void LifecycleCallback(screen_lifecycle_event_t event);
};

#endif  // EBOOK_SCREEN_H
