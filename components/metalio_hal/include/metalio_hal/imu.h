#pragma once

// SC7A20H 三轴加速度计门面（I2C 0x19，LIS2DH12 兼容寄存器映射）。
// 从 metalio-claw-4 已删除的水平仪 App 抽出（原 level_screen.cc 内嵌的
// Sc7a20h : public I2cDevice），去 UI 化为 mhal::imu 常驻能力。
//
// 用法：
//   mhal::Init() 内部调一次 imu::Init()（幂等，重复调不会重起后台任务）；
//   探测/配置失败（未焊、probe NACK）只返回 false，不影响开机。
//   数据面一律走 GetSnapshot() —— 非阻塞、任意线程安全，读的是后台采样任务
//   (5Hz) 发布的最近一帧；ReadAccel() 是阻塞直读，仅供调试/一次性用途，
//   不要在 UI 线程或高频路径调用。
namespace mhal::imu {

// 探测 0x19 + 配置 CTRL_REG1(100Hz/XYZ使能)/CTRL_REG4(BDU/±2g/HR) + 起后台
// 采样任务(5Hz)。幂等：重复调用直接返回上次结果，不会重复起任务。
// 未焊接 / probe NACK 时返回 false，不会崩溃。
bool Init();

// 阻塞读一帧三轴加速度，单位 mg。返回 false 表示总线读失败或未 Init 成功。
bool ReadAccel(int& x_mg, int& y_mg, int& z_mg);

// 非阻塞读后台任务发布的最近一次快照：三轴加速度(mg) + 由加速度算出的俯仰
// (pitch，绕 Y 轴前后倾)/横滚(roll，绕 X 轴左右倾)，单位整数度。
// 从未成功采样过时返回 false（各字段为 0）。任意线程安全。
bool GetSnapshot(int& x_mg, int& y_mg, int& z_mg, int& pitch_deg, int& roll_deg);

}  // namespace mhal::imu
