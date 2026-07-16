#pragma once

// GPS/GNSS NMEA-0183 解析（迁移自 Claw4 main/boards/common/gps_service.{cc,h}）。
//
// 默认门控关闭：本头文件的 Enable(true) 之前，不会有任何代码上电 GPS 模块
// 或安装 UART0 驱动——mhal::Init()/boot 链完全不碰这个模块。只有显式调用
// Enable(true) 才会：(a) 把 TCA9555 P0（IOExpander::Pin::GPS_POWER）拉高
// 给模块供电；(b) 在 UART_NUM_0 上装 9600 8N1 无流控驱动并起 RX 解析任务。
// Enable(false) 逆序收尾：停任务、卸驱动、P0 断电。
//
// 引脚/波特率以 Claw4 代码常量为准（其头文件注释写的 34/31/UART1 是错的，
// 已通过 `git show a05e412^:main/boards/common/gps_service.cc` 核实）：
//   RX(ESP 收 GPS TX) = GPIO 37
//   TX(ESP 发给 GPS RX) = GPIO 38
//   UART = UART_NUM_0
//   9600 8N1，无流控
//
// ⚠️ 两个真机验证风险（本迁移不解决，仅标注）：
//   (a) UART_NUM_0 在本固件上是否被用作 console/日志口未最终确认——本机
//       烧录走 USB-JTAG，console 大概率不占用 UART0，但需要真机核实；如果
//       console 确实在 UART0，Enable(true) 会和它抢串口。
//   (b) 这颗 GPS 模块在本批次板子上是否真的贴片焊接未知——Claw4 有对应
//       代码不代表每批板子都装了这颗料。
//
// 因此默认保持关闭，只有显式 Enable(true) 才会触碰 UART0/P0。

namespace mhal::gps {

struct Fix {
    bool valid = false;      // 是否有效定位
    double lat = 0, lon = 0;  // 十进制度（+N/-S, +E/-W）
    float alt_m = 0;          // 海拔（米，来自 GGA）
    float speed_kmh = 0;      // 地速 km/h（来自 RMC，knots 换算）
    int sats = 0;             // 可见/使用卫星数
};

// 门控开关。on=true：P0 上电 + 装 UART0 驱动（9600 8N1）+ 起 RX 解析任务；
// on=false：停任务 + 卸驱动 + P0 断电。幂等（重复调用同一状态直接返回 true）。
bool Enable(bool on);

// 当前是否已 Enable(true)。
bool IsEnabled();

// 非阻塞读最近一次解析快照。未 Enable 或尚未收到有效定位时 valid=false。
bool GetFix(Fix& out);

}  // namespace mhal::gps
