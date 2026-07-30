// pi_guide.h — 开机未配置引导卡（扫码进 Web 后台填密钥）。
//
// 固件不打包密钥，全新设备开机时 NVS 里既没有大模型 API Key 也没有语音密钥，
// agent 起不来。此时待机页把大时钟让位给这张卡：列出缺哪项，并按网络状态给出
// 下一步——
//   WiFi 已连  → 幂等起 Web 后台，显示地址二维码，手机扫码即进配置页
//   WiFi 未连  → 引导去配网（点按走"置 force_ap + 重启"，真机唯一可靠路径）；
//                已在配网态则显示热点二维码（扫码直连热点）
//   4G        → 只提示切 WiFi（运营商 NAT，手机连不进设备）
//
// 宿主与显隐由 pi_screen 管（与 pi_card 的 standby pin 卡同一块区域，二者互斥，
// 引导优先）；本模块只负责卡片内容与刷新。

#pragma once

#include "lvgl.h"

namespace pi_guide {

// 配置未齐（缺大模型 或 缺语音密钥）→ true。pi_screen 用它决定要不要显示引导。
bool Needed();

// 在 parent 里建卡片（幂等，重复调用只建一次）。
void Build(lv_obj_t* parent);

// 刷新文案 / 二维码；可见期间由 pi_screen 定时调（顺带幂等重启 Web 后台，
// 抵消后台的 10min 闲置自停）。LVGL 线程调用。
void Refresh();

}  // namespace pi_guide
