#include "lv_adapter_display.h"

#include <cstring>
#include <memory>

#include <esp_lcd_panel_io.h>
#include <esp_log.h>

#include "esp_lv_adapter.h"
#include "touch_feed.h"

#include "screen/pi_screen/pi_screen.h"
#include "screen_util.h"

static const char* TAG = "LVAdapterDisplay";

LVAdapterDisplay::LVAdapterDisplay(const esp_lcd_panel_handle_t panel,
                                   const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width,
                                   const int height) {
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.stack_in_psram = true;
    // LVGL 任务栈默认 8KB 太小：电子书用 FreeType 用户字体时，OTF/CFF 字体的 Adobe charstring
    // 解释器(cf2_*)在 lvgl 任务上做字形度量/布局要吃 ~18-28KB 栈（简单字形亦然），8KB 直接爆栈
    // (Stack protection fault)。因 stack_in_psram=true，加大只占 PSRAM，不耗内部 RAM。
    adapter_cfg.task_stack_size = 65536;
    adapter_cfg.task_priority = 1;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    // 性能调优要点（720x720 RGB565 屏）：
    //   - enable_ppa_accel: 打开 ESP32-P4 内置 PPA 硬件单元，专门做 alpha
    //     混合 / 像素格式转换 / 旋转，过去全靠软件遍历像素。开了之后，主屏
    //     滑动翻页这种大面积半透明叠加最受益（用户场景里 9 个磁贴 × 20%
    //     alpha + 圆角 + 文字）。
    //   - tear_avoid_mode = TRIPLE_FULL：直接把 LCD 驱动里 num_fbs=3 的 3 张
    //     panel 帧缓冲（PSRAM 上 3×720×720×2 ≈ 3MB）当成 LVGL 的 draw buffer
    //     用，渲染→DMA 三级流水，无撕裂。
    //     之前用 DEFAULT_MIPI_DSI（= TRIPLE_PARTIAL）会额外要一块
    //     720×buffer_height×2 ≈ 280KB 的内部 SRAM partial buffer，而片上 SRAM
    //     被 FreeRTOS / WiFi / SDIO 吃掉后根本剩不下，导致启动日志里报
    //     「alloc partial draw buffer failed」+「tear mode 4 setup failed」，
    //     adapter 还会再 fallback 申请 ~576KB PSRAM 当双缓冲，3MB+576KB 双重
    //     浪费。TRIPLE_FULL 彻底避开这条 fallback 路径。
    //   - buffer_height / require_double_buffer 在 TRIPLE_FULL 模式下不再生效
    //     （buffer 直接用 panel FB），保留是为了将来切回 partial 模式方便。
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = panel_io,
        .profile =
            {
                .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
                .hor_res = static_cast<uint16_t>(width),
                .ver_res = static_cast<uint16_t>(height),
                .buffer_height = 200,
                .use_psram = true,
                .enable_ppa_accel = true,
                .require_double_buffer = true,
            },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL,
    };

    lv_display_t* disp = esp_lv_adapter_register_display(&disp_cfg);
    esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
    lv_indev_t* touch_indev = esp_lv_adapter_register_touch(&touch_cfg);
    touch_feed_init(touch_handle, 20);
    touch_feed_attach_indev(touch_indev);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    // No resources.bin mmap/fs_mount here: pi has no lv_image/"A:" assets,
    // its fonts are all compiled-in C symbols (blueprint 0/b).

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetupUI();
        esp_lv_adapter_unlock();
    }
}

// Single-app firmware: no boot screen, no home menu. Load pi_screen
// directly and attach its lifecycle hook (equivalent to inlining
// home_screen's old LaunchPi()) so LOAD fires pi_agent_task_start() and
// registers PWR_KEY (blueprint b).
void LVAdapterDisplay::SetupUI() {
    lv_obj_t* pi = PiScreen::Create();
    screen_attach_lifecycle(pi, [](screen_lifecycle_event_t e) { PiScreen::LifecycleCallback(e); });
    lv_screen_load(pi);
}

LVAdapterDisplay::~LVAdapterDisplay() = default;

void LVAdapterDisplay::SetStatus(const char* const status) {}

void LVAdapterDisplay::ShowNotification(const char* notification, int duration_ms) {}

void LVAdapterDisplay::UpdateStatusBar(bool update_all) {}

void LVAdapterDisplay::SetPowerSaveMode(bool on) {}

void LVAdapterDisplay::SetPreviewImage(const void* image) {}

void LVAdapterDisplay::SetTheme(Theme* const theme) { ESP_LOGI(TAG, "SetTheme: %p", theme); }

bool LVAdapterDisplay::Lock(const int timeout_ms) { return true; }

void LVAdapterDisplay::Unlock() {}
