// 面板 + 触摸 + LVGL adapter 起显。
// 面板初始化自原 metalio-claw-4.cc（InitializeNV3051FLCD/InitializeFL7707NLCD/
// InitializeTouch），adapter 初始化自原 LVAdapterDisplay 构造函数——去掉了
// resources.bin mmap（pi 固件字体全部编译进固件，无 "A:" 资产）与 SetupUI
// （加载哪个 screen 是 main 的事，lib 不认识 screen）。
#include "metalio_hal/display.h"

#include <esp_check.h>
#include <esp_err.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_lcd_touch_gt911.h"
#include "esp_lv_adapter.h"

#include "config.h"
#include "esp_lcd_fl7707n.h"
#include "esp_lcd_nv3051f.h"
#include "hal_internal.h"
#include "touch_feed.h"

#define TAG "mhal_display"

// ========== LCD 屏幕选择 ==========
//   0 = NV3051F (36MHz DPI, RGB888, 24bpp)  ← 默认，量产屏
//   1 = FL7707N (48MHz DPI, RGB888, 16bpp)  ← 新备选屏
#ifndef METALIO_CLAW_4_USE_FL7707N
#define METALIO_CLAW_4_USE_FL7707N 0
#endif

namespace mhal {
namespace {

esp_lcd_touch_handle_t s_touch_handle = nullptr;
esp_lcd_panel_io_handle_t s_panel_io_handle = nullptr;
esp_lcd_panel_handle_t s_panel_handle = nullptr;
lv_display_t* s_lv_display = nullptr;

esp_err_t EnableDsiPhyPower() {
#if MIPI_DSI_PHY_PWR_LDO_CHAN > 0
    // 为 MIPI DSI PHY 上电，使其从「无电源」状态进入「关闭」状态
    static esp_ldo_channel_handle_t phy_pwr_chan = nullptr;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif
    return ESP_OK;
}

#if !METALIO_CLAW_4_USE_FL7707N
// ---------- NV3051F (TRULY HE396-040T2BZZ, 36MHz DPI, RGB888) ----------
void InitPanelImpl() {
    EnableDsiPhyPower();

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = nullptr;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 1000,
    };
    esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel (NV3051F)");
    esp_lcd_dbi_io_config_t dbi_config = NV3051F_PANEL_IO_DBI_CONFIG();
    esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &s_panel_io_handle);

    esp_lcd_dpi_panel_config_t dpi_config;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 36;  // NV3051F_DCLK_MHZ
    dpi_config.virtual_channel = 0;
    dpi_config.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
    // LVGL adapter 跑 TEAR_AVOID_MODE_TRIPLE_FULL，需要 3 张 panel FB
    // 当 LVGL 的 draw buffer，避免回退到 partial 模式抢内部 SRAM。
    dpi_config.num_fbs = 3;
    // 视频时序 (TRULY HE396-040T2BZZ + NV3051F, 20250708 datasheet)
    dpi_config.video_timing.h_size = 720;
    dpi_config.video_timing.v_size = 720;
    dpi_config.video_timing.hsync_back_porch = 44;
    dpi_config.video_timing.hsync_pulse_width = 2;
    dpi_config.video_timing.hsync_front_porch = 46;
    dpi_config.video_timing.vsync_back_porch = 14;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_front_porch = 16;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB888;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB888;
    dpi_config.flags.use_dma2d = true;

    nv3051f_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };

    const esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_nv3051f(s_panel_io_handle, &lcd_dev_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
}
#else
// ---------- FL7707N (48MHz DPI, RGB888) ----------
// 参数源自厂商 example (esp32-p4-fl7707n-gt911)。
void InitPanelImpl() {
    EnableDsiPhyPower();

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = nullptr;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 1000,
    };
    esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel (FL7707N)");
    esp_lcd_dbi_io_config_t dbi_config = FL7707N_PANEL_IO_DBI_CONFIG();
    esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &s_panel_io_handle);

    esp_lcd_dpi_panel_config_t dpi_config;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 48;  // FL7707N_DCLK_MHZ
    dpi_config.virtual_channel = 0;
    dpi_config.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
    // 厂商 example 用 2 张 FB，但本工程 LVGL 通路必须 3 张（TRIPLE_FULL），
    // 否则回退 partial 模式抢内部 SRAM，初始化失败。
    dpi_config.num_fbs = 3;
    dpi_config.video_timing.h_size = 720;
    dpi_config.video_timing.v_size = 720;
    dpi_config.video_timing.hsync_back_porch = 120;
    dpi_config.video_timing.hsync_pulse_width = 60;
    dpi_config.video_timing.hsync_front_porch = 106;
    dpi_config.video_timing.vsync_back_porch = 20;
    dpi_config.video_timing.vsync_pulse_width = 4;
    dpi_config.video_timing.vsync_front_porch = 20;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB888;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB888;
    dpi_config.flags.use_dma2d = true;

    fl7707n_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };

    const esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_fl7707n(s_panel_io_handle, &lcd_dev_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
}
#endif  // METALIO_CLAW_4_USE_FL7707N

// GT911 的 I2C 地址取决于上电时 INT 引脚的电平（由模组固定的上拉/下拉决定），
// 而 rst/int 两根线本工程尚未接管驱动，无法在软件侧确定性选址——这是已知
// 取舍，正路是接管 rst/int 走驱动主动选址。当前只能靠探测：每个候选地址
// 探 3 次、间隔 20ms 再判失败，降低单次总线抖动误判成"没接"的概率；两地址
// 都探测失败时不再静默 fallback 到默认地址装作没事，而是 ESP_LOGE 醒目报错
// （触摸可能不可用，实际地址取决于上次上电遗留的 INT 电平），但仍按现状继续
// 用默认地址 fallback 初始化，不比现在更糟。
uint8_t ProbeGT911I2CAddress() {
    const uint8_t addrs[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
    };
    for (uint8_t addr : addrs) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (i2c_master_probe(internal::I2cBus(), addr, 100) == ESP_OK) {
                ESP_LOGI(TAG, "GT911 found at I2C address 0x%02X", addr);
                return addr;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    ESP_LOGE(TAG,
             "GT911 I2C probe failed at both 0x%02X/0x%02X, touch may be unavailable "
             "(actual address depends on INT level left over from last power-up); "
             "fallback to default 0x%02X anyway",
             ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
             ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS);
    return ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
}

}  // namespace

namespace internal {

void InitPanel() { InitPanelImpl(); }

void InitTouch() {
    if (s_touch_handle != nullptr) {
        return;
    }

    const uint8_t dev_addr = ProbeGT911I2CAddress();

    esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {};
    tp_io_config.dev_addr = dev_addr;
    tp_io_config.scl_speed_hz = 400 * 1000;
    tp_io_config.control_phase_bytes = 1;
    tp_io_config.dc_bit_offset = 0;
    tp_io_config.lcd_cmd_bits = 16;
    tp_io_config.flags.disable_control_phase = 1;

    esp_err_t err = esp_lcd_new_panel_io_i2c(I2cBus(), &tp_io_config, &tp_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Touch panel IO create failed: 0x%x", err);
        return;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_WIDTH,
        .y_max = DISPLAY_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };

    ESP_LOGI(TAG, "Initialize GT911 touch controller");
    err = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_touch_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed: 0x%x", err);
        esp_lcd_panel_io_del(tp_io_handle);
        s_touch_handle = nullptr;
        return;
    }
}

void StartLvglAdapter() {
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.stack_in_psram = true;
    // 64KB LVGL 任务栈：stack_in_psram=true 只占 PSRAM。历史教训是 8KB
    // 会在复杂字形布局时爆栈（Stack protection fault），保留大栈。
    adapter_cfg.task_stack_size = 65536;
    adapter_cfg.task_priority = 1;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    // 性能调优要点（720x720 屏）：
    //   - enable_ppa_accel: ESP32-P4 PPA 硬件做 alpha 混合/格式转换/旋转。
    //   - tear_avoid_mode = TRIPLE_FULL：直接把 LCD 驱动 num_fbs=3 的 3 张
    //     panel 帧缓冲当 LVGL draw buffer 用，渲染→DMA 三级流水无撕裂；
    //     TRIPLE_PARTIAL 会额外要 ~280KB 内部 SRAM partial buffer（已被
    //     FreeRTOS/WiFi/SDIO 吃光）并再 fallback ~576KB PSRAM 双缓冲，
    //     TRIPLE_FULL 彻底避开这条 fallback 路径。
    //   - buffer_height / require_double_buffer 在 TRIPLE_FULL 下不生效，
    //     保留是为了将来切回 partial 模式方便。
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel_handle,
        .panel_io = s_panel_io_handle,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI,
            .hor_res = static_cast<uint16_t>(DISPLAY_WIDTH),
            .ver_res = static_cast<uint16_t>(DISPLAY_HEIGHT),
            .buffer_height = 200,
            .use_psram = true,
            // PPA 加速暂关（A/B 验证蓝闪）：真机日志抓到 stock 图卡重绘触发
            // PPA blend 前 esp_cache_msync 越界窗口报错，且 PPA 2D-DMA 与 DSI
            // 扫描抢 PSRAM 带宽疑似 underrun 闪蓝——关掉观察蓝闪是否消失。
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL,
    };

    s_lv_display = esp_lv_adapter_register_display(&disp_cfg);
    if (s_touch_handle != nullptr) {
        esp_lv_adapter_touch_config_t touch_cfg =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_lv_display, s_touch_handle);
        lv_indev_t* touch_indev = esp_lv_adapter_register_touch(&touch_cfg);
        touch_feed_init(s_touch_handle, 20);
        touch_feed_attach_indev(touch_indev);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());
}

}  // namespace internal

namespace display {

lv_display_t* GetLvDisplay() { return s_lv_display; }

bool Lock(int timeout_ms) { return esp_lv_adapter_lock(timeout_ms) == ESP_OK; }

void Unlock() { esp_lv_adapter_unlock(); }

int Width() { return DISPLAY_WIDTH; }

int Height() { return DISPLAY_HEIGHT; }

}  // namespace display
}  // namespace mhal
