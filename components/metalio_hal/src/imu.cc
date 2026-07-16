// SC7A20H 加速度计驱动，从 metalio-claw-4 已删除的水平仪 App 抽出
// （level_screen.cc 内嵌的 Sc7a20h : public I2cDevice），去 UI 化。
#include "metalio_hal/imu.h"

#include <atomic>
#include <cmath>
#include <mutex>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/i2c_master.h>

#include "hal_internal.h"

#define TAG "mhal::imu"

namespace mhal::imu {

namespace {

// ---------------------------------------------------------------------------
// SC7A20H (I2C 0x19) -- LIS2DH12-compatible register map. 上电默认 power-down，
// 必须显式写 CTRL_REG1 才会出数据；OUT_X_L 连读 6 字节需把寄存器地址 MSB 置 1
// 触发自增，否则只会反复读到同一个寄存器。
// ---------------------------------------------------------------------------
constexpr uint8_t kAddr        = 0x19;
constexpr uint8_t kRegWhoAmI   = 0x0F;
constexpr uint8_t kRegCtrlReg1 = 0x20;
constexpr uint8_t kRegCtrlReg4 = 0x23;
constexpr uint8_t kRegOutXL    = 0x28;
constexpr uint8_t kAutoIncMask = 0x80;

// CTRL_REG1 = 0x57 -> ODR=100Hz, Z/Y/X enable.
constexpr uint8_t kCtrlReg1Val = 0x57;
// CTRL_REG4 = 0x88 -> BDU=1, FS=00(±2g), HR=1.
constexpr uint8_t kCtrlReg4Val = 0x88;
// ±2g + HR(12-bit 左对齐) 模式下，1 LSB ≈ 1 mg。
constexpr float   kMgPerLsb    = 1.0f;

constexpr uint32_t kI2cSpeedHz   = 400 * 1000;
constexpr int       kI2cTimeoutMs = 100;
constexpr int       kProbeTimeoutMs = 50;

// 后台采样周期：5Hz，数据面够用，不打扰 I2C 总线上的电量计轮询。
constexpr uint32_t kSamplePeriodMs = 200;

constexpr float kRadToDeg = 57.2958f;

i2c_master_dev_handle_t s_dev = nullptr;
bool s_init_started = false;  // 幂等守卫：只允许起一次任务
bool s_init_ok = false;

// 快照：mg + 整数度，短锁保护。
struct Snapshot {
    bool valid = false;
    int x_mg = 0;
    int y_mg = 0;
    int z_mg = 0;
    int pitch_deg = 0;
    int roll_deg = 0;
};

std::mutex s_snap_mu;
Snapshot s_snap;

bool WriteReg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), kI2cTimeoutMs) == ESP_OK;
}

bool ReadReg(uint8_t reg, uint8_t& out) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, &out, 1, kI2cTimeoutMs) == ESP_OK;
}

bool ReadRegs(uint8_t reg, uint8_t* buf, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, kI2cTimeoutMs) == ESP_OK;
}

void SamplingTask(void*) {
    while (true) {
        int x, y, z;
        if (ReadAccel(x, y, z)) {
            // pitch: 绕 Y 轴前后倾；roll: 绕 X 轴左右倾。只做数学运算+存 int，
            // 不打印浮点，newlib-nano 无 %f/%lld 的坑在这里不适用。
            float pitch = std::atan2(static_cast<float>(x),
                                      std::sqrt(static_cast<float>(y) * y + static_cast<float>(z) * z)) *
                          kRadToDeg;
            float roll = std::atan2(static_cast<float>(y), static_cast<float>(z)) * kRadToDeg;

            std::lock_guard<std::mutex> lk(s_snap_mu);
            s_snap.valid = true;
            s_snap.x_mg = x;
            s_snap.y_mg = y;
            s_snap.z_mg = z;
            s_snap.pitch_deg = static_cast<int>(pitch + (pitch >= 0 ? 0.5f : -0.5f));
            s_snap.roll_deg = static_cast<int>(roll + (roll >= 0 ? 0.5f : -0.5f));
        }
        // 读失败保留上一帧快照，不崩溃、不清空。
        vTaskDelay(pdMS_TO_TICKS(kSamplePeriodMs));
    }
}

}  // namespace

bool Init() {
    if (s_init_started) {
        return s_init_ok;
    }
    s_init_started = true;

    i2c_master_bus_handle_t bus = internal::I2cBus();
    if (bus == nullptr) {
        ESP_LOGW(TAG, "Init() called with null I2C bus");
        return false;
    }

    esp_err_t probe = i2c_master_probe(bus, kAddr, kProbeTimeoutMs);
    if (probe != ESP_OK) {
        ESP_LOGW(TAG, "SC7A20H @0x%02X probe NACK (err=0x%x)，未焊接或未上电，跳过", kAddr, probe);
        return false;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kAddr,
        .scl_speed_hz = kI2cSpeedHz,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_master_bus_add_device(SC7A20H) failed: 0x%x", err);
        s_dev = nullptr;
        return false;
    }

    // best-effort 校验 WHO_AM_I：SC7A20H 跟 ST 家系列共用多个值，不强判，
    // 只要 ACK 通过就当作在线，读不到/不认识的值仅记日志。
    uint8_t who = 0;
    if (ReadReg(kRegWhoAmI, who)) {
        ESP_LOGI(TAG, "SC7A20H online, WHO_AM_I=0x%02X", who);
    } else {
        ESP_LOGW(TAG, "SC7A20H ACK 通过但读 WHO_AM_I 失败，先继续配置");
    }

    if (!WriteReg(kRegCtrlReg1, kCtrlReg1Val) || !WriteReg(kRegCtrlReg4, kCtrlReg4Val)) {
        ESP_LOGW(TAG, "SC7A20H CTRL_REG 写入失败");
        return false;
    }

    xTaskCreate(SamplingTask, "imu_sample", 3072, nullptr, tskIDLE_PRIORITY + 1, nullptr);
    s_init_ok = true;
    return true;
}

bool ReadAccel(int& x_mg, int& y_mg, int& z_mg) {
    if (s_dev == nullptr) {
        return false;
    }
    uint8_t buf[6] = {0};
    if (!ReadRegs(kRegOutXL | kAutoIncMask, buf, sizeof(buf))) {
        return false;
    }
    // 12-bit 左对齐到 16-bit：按 int16_t 拼出来再算术右移 4。
    int16_t rx = static_cast<int16_t>((buf[1] << 8) | buf[0]);
    int16_t ry = static_cast<int16_t>((buf[3] << 8) | buf[2]);
    int16_t rz = static_cast<int16_t>((buf[5] << 8) | buf[4]);
    x_mg = static_cast<int>((rx >> 4) * kMgPerLsb);
    y_mg = static_cast<int>((ry >> 4) * kMgPerLsb);
    z_mg = static_cast<int>((rz >> 4) * kMgPerLsb);
    return true;
}

bool GetSnapshot(int& x_mg, int& y_mg, int& z_mg, int& pitch_deg, int& roll_deg) {
    std::lock_guard<std::mutex> lk(s_snap_mu);
    x_mg = s_snap.x_mg;
    y_mg = s_snap.y_mg;
    z_mg = s_snap.z_mg;
    pitch_deg = s_snap.pitch_deg;
    roll_deg = s_snap.roll_deg;
    return s_snap.valid;
}

}  // namespace mhal::imu
