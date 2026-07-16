#pragma once

// sim 桩：与设备侧 components/metalio_hal/include/metalio_hal/gps.h 签名一致。
// 桌面环境没有 UART0/TCA9555，Enable 只记一个内存标志；GetFix 返回固定的
// 演示定位点（上海），便于 sim 里可视化 GPS 相关 UI。

namespace mhal::gps {

struct Fix {
    bool valid = false;
    double lat = 0, lon = 0;
    float alt_m = 0;
    float speed_kmh = 0;
    int sats = 0;
};

bool Enable(bool on);
bool IsEnabled();
bool GetFix(Fix& out);

}  // namespace mhal::gps
