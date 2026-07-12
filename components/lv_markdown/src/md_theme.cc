#include "lv_markdown/md_theme.h"

namespace lvmd {

const MdTheme& MdThemeDefaultDark() {
    static const MdTheme theme = [] {
        MdTheme t;  // colors/layout carry their dark defaults from the struct
        t.body = lv_font_get_default();
        t.heading = lv_font_get_default();
        t.mono = lv_font_get_default();
        t.mono_cjk = lv_font_get_default();
        return t;
    }();
    return theme;
}

}  // namespace lvmd
