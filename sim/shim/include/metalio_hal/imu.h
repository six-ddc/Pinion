#pragma once

// sim 桩：签名与真机 components/metalio_hal/include/metalio_hal/imu.h 一致，
// 假装设备平放桌面（x=0,y=0,z=1000mg，pitch/roll≈0）。
namespace mhal::imu {

bool Init();
bool ReadAccel(int& x_mg, int& y_mg, int& z_mg);
bool GetSnapshot(int& x_mg, int& y_mg, int& z_mg, int& pitch_deg, int& roll_deg);

}  // namespace mhal::imu
