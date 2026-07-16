#include "metalio_hal/gps.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "IOExpander.hpp"

// NMEA-0183 阅读器，迁移自 Claw4 main/boards/common/gps_service.cc（GGA/RMC/GSV
// 解析、checksum 校验、坐标度分转十进制度、knots→km/h 换算逻辑原样保留）。
// 门控封装：Enable(true) 才上电+装驱动+起任务，见 gps.h 顶部说明与真机风险。

namespace mhal::gps {

namespace {

constexpr const char* TAG = "mhal_gps";

constexpr int kGpsRxPin = 37;   // ESP 收 GPS 模块 TX
constexpr int kGpsTxPin = 38;   // ESP 发给 GPS 模块 RX
constexpr uart_port_t kGpsUartNum = UART_NUM_0;
constexpr int kGpsBaudRate = 9600;

// ESP-IDF 要求 RX buffer > UART_HW_FIFO_LEN(128)；2KB 给解析延迟留足余量。
constexpr size_t kUartRxBufferBytes = 2048;
constexpr size_t kUartTxBufferBytes = 0;  // 只收不发
constexpr TickType_t kReadTimeoutTicks = pdMS_TO_TICKS(100);

// -----------------------------------------------------------------------
// 模块状态
// -----------------------------------------------------------------------
SemaphoreHandle_t g_mutex = nullptr;
TaskHandle_t g_rx_task = nullptr;
volatile bool g_enabled = false;
volatile bool g_task_should_run = false;

Fix g_fix;  // g_mutex 保护

// 行缓冲（RX 任务私有，无需加锁）
char g_line_buf[128] = {};
size_t g_line_len = 0;

// 各星座（talker）最近一次 GSV 报告的可见卫星数，逐桶累加求和。
enum { kTalkerGP = 0, kTalkerGL, kTalkerGA, kTalkerBD, kTalkerGN, kTalkerCount };
uint8_t g_sats_view_per_talker[kTalkerCount] = {};

int TalkerIndex(const char* talker) {
    if (talker == nullptr) return -1;
    if (std::strcmp(talker, "GP") == 0) return kTalkerGP;
    if (std::strcmp(talker, "GL") == 0) return kTalkerGL;
    if (std::strcmp(talker, "GA") == 0) return kTalkerGA;
    if (std::strcmp(talker, "BD") == 0) return kTalkerBD;
    if (std::strcmp(talker, "GN") == 0) return kTalkerGN;
    return -1;
}

bool ChecksumOk(const char* sentence, size_t len) {
    const char* star = nullptr;
    for (size_t i = 0; i < len; ++i) {
        if (sentence[i] == '*') {
            star = &sentence[i];
            break;
        }
    }
    if (star == nullptr || (star - sentence) < 1) return false;
    const size_t hex_off = static_cast<size_t>(star - sentence) + 1;
    if (hex_off + 2 > len) return false;

    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return -1;
    };
    const int hi = hexval(sentence[hex_off]);
    const int lo = hexval(sentence[hex_off + 1]);
    if (hi < 0 || lo < 0) return false;
    const uint8_t expected = static_cast<uint8_t>((hi << 4) | lo);

    uint8_t computed = 0;
    for (size_t i = 1; i < static_cast<size_t>(star - sentence); ++i) {
        computed ^= static_cast<uint8_t>(sentence[i]);
    }
    return computed == expected;
}

double ParseCoord(const char* raw, char hemi, int deg_digits) {
    if (raw == nullptr || *raw == '\0') return 0.0;
    const size_t raw_len = std::strlen(raw);
    if (static_cast<int>(raw_len) <= deg_digits) return 0.0;

    char deg_buf[4] = {};
    if (deg_digits >= static_cast<int>(sizeof(deg_buf))) return 0.0;
    std::memcpy(deg_buf, raw, deg_digits);
    deg_buf[deg_digits] = '\0';

    const int degrees = std::atoi(deg_buf);
    const double minutes = std::atof(raw + deg_digits);
    double value = static_cast<double>(degrees) + (minutes / 60.0);

    if (hemi == 'S' || hemi == 'W') value = -value;
    if (std::isnan(value) || std::isinf(value)) return 0.0;
    return value;
}

void ParseGGA(const char* fields[], int n) {
    if (n < 10) return;
    const char* lat_raw = fields[2];
    const char* ns = fields[3];
    const char* lon_raw = fields[4];
    const char* ew = fields[5];
    const char* fix_q_raw = fields[6];
    const char* sats_raw = fields[7];
    const char* alt_raw = fields[9];

    const int fix_quality = (fix_q_raw && *fix_q_raw) ? std::atoi(fix_q_raw) : 0;
    const double altitude = (alt_raw && *alt_raw) ? std::atof(alt_raw) : 0.0;
    const int sats_used = (sats_raw && *sats_raw) ? std::atoi(sats_raw) : 0;

    const double lat = ParseCoord(lat_raw, (ns && *ns) ? *ns : '\0', 2);
    const double lon = ParseCoord(lon_raw, (ew && *ew) ? *ew : '\0', 3);

    if (xSemaphoreTake(g_mutex, portMAX_DELAY) != pdTRUE) return;
    g_fix.sats = sats_used;
    if (fix_quality > 0) {
        g_fix.valid = true;
        g_fix.lat = lat;
        g_fix.lon = lon;
        g_fix.alt_m = static_cast<float>(altitude);
    }
    xSemaphoreGive(g_mutex);
}

void ParseRMC(const char* fields[], int n) {
    if (n < 10) return;
    const char* status = fields[2];
    const char* lat_raw = fields[3];
    const char* ns = fields[4];
    const char* lon_raw = fields[5];
    const char* ew = fields[6];
    const char* spd_raw = fields[7];

    const bool active = (status && status[0] == 'A');
    const double knots = (spd_raw && *spd_raw) ? std::atof(spd_raw) : 0.0;
    const double lat = ParseCoord(lat_raw, (ns && *ns) ? *ns : '\0', 2);
    const double lon = ParseCoord(lon_raw, (ew && *ew) ? *ew : '\0', 3);

    if (xSemaphoreTake(g_mutex, portMAX_DELAY) != pdTRUE) return;
    g_fix.speed_kmh = static_cast<float>(knots * 1.852);  // knots -> km/h
    if (active) {
        g_fix.valid = true;
        g_fix.lat = lat;
        g_fix.lon = lon;
    }
    xSemaphoreGive(g_mutex);
}

void ParseGSV(const char* talker, const char* fields[], int n) {
    if (n < 4) return;
    const char* sats_view_raw = fields[3];
    if (sats_view_raw == nullptr || *sats_view_raw == '\0') return;
    const int sats_view = std::atoi(sats_view_raw);
    if (sats_view < 0 || sats_view > 64) return;

    const int slot = TalkerIndex(talker);
    if (slot < 0) return;
    g_sats_view_per_talker[slot] = static_cast<uint8_t>(sats_view);

    int total = 0;
    for (int i = 0; i < kTalkerCount; ++i) total += g_sats_view_per_talker[i];
    if (total > 255) total = 255;

    if (xSemaphoreTake(g_mutex, portMAX_DELAY) != pdTRUE) return;
    g_fix.sats = total;
    xSemaphoreGive(g_mutex);
}

void HandleSentence(const char* sentence, size_t len) {
    if (len < 6 || sentence[0] != '$') return;
    if (!ChecksumOk(sentence, len)) return;  // 坏 checksum（开机/掉电毛刺）静默丢弃

    char buf[160];
    if (len >= sizeof(buf)) return;
    std::memcpy(buf, sentence, len);
    buf[len] = '\0';

    char* star = std::strchr(buf, '*');
    if (star != nullptr) *star = '\0';

    constexpr int kMaxFields = 24;
    const char* fields[kMaxFields] = {};
    int field_count = 0;

    char* cursor = buf;
    fields[field_count++] = cursor;
    while (*cursor != '\0' && field_count < kMaxFields) {
        if (*cursor == ',') {
            *cursor = '\0';
            fields[field_count++] = cursor + 1;
        }
        ++cursor;
    }

    const char* head = fields[0];
    if (std::strlen(head) < 6) return;
    const char talker[3] = {head[1], head[2], '\0'};
    const char* sentence_type = head + 3;

    if (std::strcmp(sentence_type, "GGA") == 0) {
        ParseGGA(fields, field_count);
    } else if (std::strcmp(sentence_type, "RMC") == 0) {
        ParseRMC(fields, field_count);
    } else if (std::strcmp(sentence_type, "GSV") == 0) {
        ParseGSV(talker, fields, field_count);
    }
}

void RxTaskLoop(void* /*arg*/) {
    uint8_t chunk[256];

    while (g_task_should_run) {
        int n = uart_read_bytes(kGpsUartNum, chunk, sizeof(chunk), kReadTimeoutTicks);
        if (n <= 0) continue;

        for (int i = 0; i < n; ++i) {
            const char c = static_cast<char>(chunk[i]);
            if (c == '\r' || c == '\n') {
                if (g_line_len > 0) {
                    g_line_buf[g_line_len] = '\0';
                    HandleSentence(g_line_buf, g_line_len);
                    g_line_len = 0;
                }
                continue;
            }
            if (g_line_len + 1 >= sizeof(g_line_buf)) {
                g_line_len = 0;  // 溢出/垃圾数据，丢弃当前行重新同步
                continue;
            }
            g_line_buf[g_line_len++] = c;
        }
    }

    g_rx_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool Enable(bool on) {
    if (on == g_enabled) return true;

    if (on) {
        if (g_mutex == nullptr) {
            g_mutex = xSemaphoreCreateMutex();
            if (g_mutex == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate GPS mutex");
                return false;
            }
        }

        IOExpander::getInstance().setLevel(IOExpander::Pin::GPS_POWER, true);

        uart_config_t uart_cfg = {
            .baud_rate = kGpsBaudRate,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 0,
            .source_clk = UART_SCLK_DEFAULT,
            .flags = 0,
        };

        esp_err_t err = uart_driver_install(kGpsUartNum, kUartRxBufferBytes, kUartTxBufferBytes, 0, nullptr, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
            IOExpander::getInstance().setLevel(IOExpander::Pin::GPS_POWER, false);
            return false;
        }

        err = uart_param_config(kGpsUartNum, &uart_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
            uart_driver_delete(kGpsUartNum);
            IOExpander::getInstance().setLevel(IOExpander::Pin::GPS_POWER, false);
            return false;
        }

        err = uart_set_pin(kGpsUartNum, kGpsTxPin, kGpsRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
            uart_driver_delete(kGpsUartNum);
            IOExpander::getInstance().setLevel(IOExpander::Pin::GPS_POWER, false);
            return false;
        }

        g_line_len = 0;
        std::memset(g_sats_view_per_talker, 0, sizeof(g_sats_view_per_talker));
        if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
            g_fix = Fix{};
            xSemaphoreGive(g_mutex);
        }

        g_task_should_run = true;
        BaseType_t task_ok = xTaskCreate(&RxTaskLoop, "gps_rx", 4 * 1024, nullptr, 4, &g_rx_task);
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to spawn GPS RX task");
            g_task_should_run = false;
            uart_driver_delete(kGpsUartNum);
            IOExpander::getInstance().setLevel(IOExpander::Pin::GPS_POWER, false);
            return false;
        }

        g_enabled = true;
        ESP_LOGI(TAG, "GPS enabled (UART%d, %d bps, RX=%d, TX=%d)", (int)kGpsUartNum, kGpsBaudRate, kGpsRxPin,
                 kGpsTxPin);
        return true;
    }

    // 关闭：先停任务，再卸驱动，最后断电。
    g_task_should_run = false;
    // 任务是异步 self-delete 的（RX 任务在超时后检查 g_task_should_run），
    // 给它一个读超时窗口的时间退出，避免和 uart_driver_delete 竞争。
    vTaskDelay(kReadTimeoutTicks + pdMS_TO_TICKS(20));

    uart_driver_delete(kGpsUartNum);
    IOExpander::getInstance().setLevel(IOExpander::Pin::GPS_POWER, false);
    g_enabled = false;
    ESP_LOGI(TAG, "GPS disabled");
    return true;
}

bool IsEnabled() { return g_enabled; }

bool GetFix(Fix& out) {
    if (!g_enabled || g_mutex == nullptr) {
        out = Fix{};
        return false;
    }
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        out = g_fix;
        xSemaphoreGive(g_mutex);
        return true;
    }
    out = Fix{};
    return false;
}

}  // namespace mhal::gps
