// pi_sim — desktop (SDL2) simulator for the pi_screen UI.
//
// Replicates main/main.cc's boot chain minus hardware: an LVGL SDL window
// stands in for mhal::Init()'s display bring-up; screen creation, lifecycle
// attach and screen load are verbatim. The agent path is the real one —
// pi_agent_task.c + pi-c POSIX port (libcurl) → real DeepSeek API.
//
// Controls:
//   F1 (hold)        PWR_KEY 按住说话 — hold to record, release to send; a
//                    quick tap does nothing; tap during generation = interrupt
//   typing           speech while listening (types the "utterance")
//   Backspace        delete last codepoint of the "utterance"
//   F12              screenshot (BMP, path from PI_SIM_SHOT or pi_sim_shot.bmp)
//   mouse            touch (drag down from the status bar = quick panel)
//
// Env knobs: PI_SIM_SAY=1 (speak TTS via macOS `say`), PI_SIM_AUTODEMO=<text>
// (scripted demo: press key, type text, send), PI_SIM_SHOT / PI_SIM_SHOT_MS /
// PI_SIM_EXIT_MS (unattended screenshot + exit, for CI/self-test).
#include <SDL.h>
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "lvgl.h"

#include "cJSON.h"
#include "IOExpander.hpp"
#include "settings.h"  // P1 grid rehydrate 测试：直写 standby pin 封套
#include "pi_theme.h"  // T1 主题往返测试：pi_theme::Set
#include "media_player/media_player.h"
#include "pi_card/pi_card_data.h"
#include "pi_card/pi_card_host.h"
#include "pi_card/pi_card_media.h"
#include "pi_card/pi_card_preview.h"
#include "pi_card/pi_card_tools.h"
#include "pi_ui_bridge.h"
#include "stock/stock_tool.h"
#include "media_admin_httpd.h"
#include "pi_screen.h"
#include "screen_util.h"
#include "sim_hooks.h"

namespace {

std::atomic<bool> g_quit{false};
std::atomic<bool> g_pwr_key_held{false};  // F1 held = PWR_KEY pressed (press-and-hold)
std::atomic<bool> g_shot_pending{false};
std::atomic<bool> g_demo_pending{false};  // F9 = 渲染一张 pi_card 演示卡（overlay，任何视图可见）

// pi_card 声明式 UI 演示卡：覆盖 icon/slider/bar/label(mono+puhui)/switch/button/
// divider/spacer + 双向 bind + tone 语义色 + 自适应布局。走完整管道（校验→入队→
// drain 渲染），是渲染器 + 主题 + 图标 + 自适应的单图核验。
constexpr const char* kCard0 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":22,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"PI CONTROL\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"设备控制\"}]},"
    "{\"type\":\"column\",\"gap\":18,\"children\":["
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"volume\"},"
    "{\"type\":\"slider\",\"bind\":\"audio.volume\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"audio.volume\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"sun\"},"
    "{\"type\":\"slider\",\"bind\":\"display.brightness\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"display.brightness\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"battery\"},"
    "{\"type\":\"bar\",\"bind\":\"battery.level\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\",\"bind\":\"battery.level\","
    "\"fmt\":\"%d%%\"}]}]},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"wifi\",\"tone\":\"ok\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"网络\"},{\"type\":\"spacer\"},"
    "{\"type\":\"switch\",\"checked\":true}]},"
    "{\"type\":\"row\",\"gap\":12,\"children\":["
    "{\"type\":\"button\",\"variant\":\"ghost\",\"text\":\"取消\",\"on_click\":[{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"确认\","
    "\"on_click\":[{\"do\":\"report\",\"text\":\"确认\"},{\"do\":\"close\"}]}]}"
    "]}}";

// ---- 稳定性压力测试语料：多样 + 对抗性用例，验证"怎么拼都不崩不溢出不难看" ----
// 1 确认框（长正文换行 + ghost/primary 层级）
constexpr const char* kCard1 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"CONFIRM\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"切换到 4G?\"}]},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"当前使用 WiFi。切换到蜂窝网络会断开连接"
    "并重启设备,大约需要 30 秒,期间无法使用。确定继续吗?\"},"
    "{\"type\":\"row\",\"gap\":12,\"children\":["
    "{\"type\":\"button\",\"variant\":\"ghost\",\"text\":\"取消\",\"on_click\":[{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"切换并重启\","
    "\"on_click\":[{\"do\":\"report\",\"text\":\"切换到4G\"},{\"do\":\"close\"}]}]}]}}";
// 2 菜单/列表（一列全宽按钮）
constexpr const char* kCard2 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"SELECT\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"选择操作\"},"
    "{\"type\":\"spacer\",\"h\":4},"
    "{\"type\":\"button\",\"text\":\"新建对话\",\"on_click\":[{\"do\":\"report\",\"text\":\"新建对话\"},{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"text\":\"导出记录\",\"on_click\":[{\"do\":\"report\",\"text\":\"导出记录\"},{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"text\":\"清空历史\",\"on_click\":[{\"do\":\"report\",\"text\":\"清空历史\"},{\"do\":\"close\"}]},"
    "{\"type\":\"button\",\"variant\":\"ghost\",\"text\":\"关闭\",\"on_click\":[{\"do\":\"close\"}]}]}}";
// 3 信息/状态卡（键值行 + spacer 对齐 + 只读 bind + 字符串 bind）
constexpr const char* kCard3 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"STATUS\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"设备状态\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"battery\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"电量\"},{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"battery.level\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"volume\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"音量\"},{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"audio.volume\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"wifi\",\"tone\":\"ok\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"网络\"},{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"net.type\"}]},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"section\",\"text\":\"SIGNAL\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\",\"bind\":\"net.rssi\","
    "\"fmt\":\"%d dBm\"}]}]}}";
// 4 换行/分配压力（超长正文 + 一排 6 按钮）
constexpr const char* kCard4 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":14,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"换行与分配压力测试\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"这是一段刻意写得很长很长很长的说明文字,"
    "用来验证固定宽度卡片里长文本会正确折行显示完整,而不是溢出边界或被裁剪,连续无空格的中文"
    "也应当逐字换行铺满可用宽度。\"},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"button\",\"text\":\"一\"},{\"type\":\"button\",\"text\":\"二\"},"
    "{\"type\":\"button\",\"text\":\"三\"},{\"type\":\"button\",\"text\":\"四\"},"
    "{\"type\":\"button\",\"text\":\"五\"},{\"type\":\"button\",\"text\":\"六\"}]}]}}";
// 5 对抗/退化（min>max 滑块 / 无文字按钮 / 未知图标→圆点 / 空容器 / 极简）
constexpr const char* kCard5 =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"退化输入兜底\"},"
    "{\"type\":\"slider\",\"min\":80,\"max\":20,\"value\":50},"
    "{\"type\":\"button\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"wibblewobble\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"未知图标 → 圆点\"}]},"
    "{\"type\":\"column\",\"children\":[]},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"以上均应安全渲染,不崩不溢出\"}]}}";

// 6 对抗/崩溃根因：数值路径 label 误用 %s。真机(newlib-nano)上 lv_label_bind_text 立即
// 回调 → clib vsnprintf 把 net.rssi(断网=0) 当指针 strlen((char*)0) → Load access fault
// （即 docs 记录的那次渲染崩溃）。修复后 Validate 在工具期直接拒绝整卡：pi_card_tool_render
// 返回 (ERROR)，绝不进入渲染。注：本 sim 用 macOS libc，对 %s 套整数比 newlib 宽容、不复现
// 该崩溃，故此卡在 sim 里只核验「修复=Validate 同步拒绝」这一层（真机崩溃已由 panic 日志确证）。
constexpr const char* kCardBadFmt =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"畸形 fmt 对抗\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"net.rssi\",\"fmt\":\"%s\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"数值路径误用 %s: 应被拒绝而非崩溃\"}]}}";

// pi_card v1 新能力演示卡（arc / qrcode / choice / patch）——各覆盖一条能力的核心路径，
// 校验+入队+drain 走完整真管道。
// 8 arc：绑 audio.volume 的环形旋钮 + 同绑路径的 value label（联动验证）。
constexpr const char* kCardArc =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"音量旋钮\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"arc\",\"bind\":\"audio.volume\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"audio.volume\",\"fmt\":\"%d%%\"}]}]}}";
// 9 qrcode：标题 + 居中二维码（两侧 spacer）。
constexpr const char* kCardQr =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"扫码\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"spacer\"},"
    "{\"type\":\"qrcode\",\"text\":\"https://metalio.example\"},{\"type\":\"spacer\"}]}]}}";
// 10 choice：三档选择 + primary 按钮回传选中值。
constexpr const char* kCardChoice =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"难度\"},"
    "{\"type\":\"choice\",\"id\":\"level\",\"options\":[\"低\",\"中\",\"高\"],\"value\":1},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"确认\","
    "\"on_click\":[{\"do\":\"report\",\"text\":\"确认 {v} 档\"},{\"do\":\"close\"}]}]}}";
// 11 patch：拖 slider 本地实时 patch 邻近 label 的文本，零往返（stderr 应无 report ->）。
constexpr const char* kCardPatch =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"本地 patch\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"slider\",\"id\":\"s\",\"min\":0,\"max\":100,"
    "\"value\":0,\"grow\":1,\"on_change\":[{\"do\":\"patch\",\"target\":\"lbl\","
    "\"props\":{\"text\":\"{v}%\"}}]},"
    "{\"type\":\"label\",\"role\":\"value\",\"id\":\"lbl\",\"text\":\"0%\"}]}]}}";

// P4-a 数据面扩容验收卡（临时）：绑定新增只读遥测路径，核验 Register→bind→subject→
// render→活性刷新全链路。信息卡（电池扩展/网络/存储）+ chart 卡（功耗/性能历史曲线）。
constexpr const char* kCardP4aInfo =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":14,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"TELEMETRY\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"系统遥测\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"battery\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"温度\"},{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"battery.temp_c10\",\"fmt\":\"%d x0.1C\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"健康\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"ok\","
    "\"bind\":\"battery.soh_pct\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"续航\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"bind\":\"battery.tte_min\","
    "\"fmt\":\"%d min\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"循环\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\","
    "\"bind\":\"battery.cycles\",\"fmt\":\"%d\"}]},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"wifi\",\"tone\":\"ok\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"运营商\"},{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"net.operator\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"IP\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\","
    "\"bind\":\"net.ip\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"SD 剩余\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"bind\":\"storage.free_mb\","
    "\"fmt\":\"%d MB\"}]}]}}";
constexpr const char* kCardP4aChart =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"CHARTS\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"功耗 / 性能\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"section\",\"text\":\"电压 mV\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\","
    "\"bind\":\"battery.voltage_mv\",\"fmt\":\"%d\"}]},"
    "{\"type\":\"chart\",\"bind_history\":\"battery.voltage_mv\",\"points\":60,\"h\":120},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"section\",\"text\":\"CPU %\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\","
    "\"bind\":\"sys.cpu\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"chart\",\"bind_history\":\"sys.cpu\",\"points\":60,\"h\":120}]}}";

// P4-b 事件与触觉验收卡（临时）：imu 姿态路径 + usb/无线充在场路径 + device.vibrate invoke 按钮。
// 按钮能渲染即证明 device.vibrate 已注册且过 ValidateActions（真机是否真震动另需设备烟测）。
constexpr const char* kCardP4bSensors =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":14,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"SENSORS\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"传感器 / 触觉\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"俯仰\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"bind\":\"imu.pitch\","
    "\"fmt\":\"%d deg\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"横滚\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"bind\":\"imu.roll\","
    "\"fmt\":\"%d deg\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"battery\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"USB 插入\"},{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"tone\":\"ok\",\"bind\":\"power.usb_in\",\"fmt\":\"%d\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"无线充\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\","
    "\"bind\":\"power.wireless_charging\",\"fmt\":\"%d\"}]},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"震动一下\","
    "\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"device.vibrate\"}]}]}}";

// P4-c GPS 验收卡（临时）：定位路径 + 启用/停用 invoke。默认门控关时全 --/0；
// PI_SIM_GPS=1 跑 sim 可见上海演示坐标（验证 gps.lat/lon 的 FormatDegE5 手动格式化）。
constexpr const char* kCardP4cGps =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":14,\"children\":["
    "{\"type\":\"column\",\"gap\":2,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"LOCATION\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"卫星定位\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"定位\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"ok\","
    "\"bind\":\"gps.fix\",\"fmt\":\"%d\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"纬度\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"bind\":\"gps.lat\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"经度\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"bind\":\"gps.lon\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"海拔\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"tone\":\"dim\","
    "\"bind\":\"gps.alt_m\",\"fmt\":\"%d m\"}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"卫星\"},"
    "{\"type\":\"spacer\"},{\"type\":\"label\",\"role\":\"value\",\"bind\":\"gps.sats\",\"fmt\":\"%d 颗\"}]},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"row\",\"gap\":12,\"children\":["
    "{\"type\":\"button\",\"variant\":\"ghost\",\"text\":\"停用\","
    "\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"gps.disable\"}]},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"启用 GPS\","
    "\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"gps.enable\"}]}]}]}}";

// 一行多列标签测试（用户反馈设备端 LLM 始终写不对）：上半部分 = 无 grow 的朴素多列
//（列宽随内容、各行不对齐）；下半部分 = 每列 grow:1 的表格式多列（列对齐）。
constexpr const char* kCardMultiCol =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"多列标签\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"A. row 直排(无 grow, 列不对齐)\"},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"text\":\"北京\"},{\"type\":\"label\",\"text\":\"32C\"},"
    "{\"type\":\"label\",\"text\":\"晴\"}]},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"text\":\"乌鲁木齐\"},{\"type\":\"label\",\"text\":\"28C\"},"
    "{\"type\":\"label\",\"text\":\"多云\"}]},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"B. 每列 grow:1(表格式对齐)\"},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"城市\",\"grow\":1},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"温度\",\"grow\":1},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"天气\",\"grow\":1}]},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"text\":\"北京\",\"grow\":1},"
    "{\"type\":\"label\",\"text\":\"32C\",\"grow\":1},"
    "{\"type\":\"label\",\"text\":\"晴\",\"grow\":1}]},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"text\":\"乌鲁木齐\",\"grow\":1},"
    "{\"type\":\"label\",\"text\":\"28C\",\"grow\":1},"
    "{\"type\":\"label\",\"text\":\"多云\",\"grow\":1}]},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"text\":\"上海\",\"grow\":1},"
    "{\"type\":\"label\",\"mono\":true,\"text\":\"30C\",\"grow\":1},"
    "{\"type\":\"label\",\"tone\":\"ok\",\"text\":\"小雨\",\"grow\":1}]}]}}";

// Phase4 stock 动态绑定验收卡（临时）：两 symbol 各绑若干 stock.<sym>.<field> 路径，
// 覆盖 price/pct（±号）、pe/pb（HK 的 pb 应显 "--"）、market_cap（万亿 CJK → 验 SafeFont
// mono 兜底）、amount 人性化、time。渲染先全 "--"，报价落地（~1-3s）后经 subject 自动填。
constexpr const char* kCardStockBind =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"STOCK BIND\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"动态行情绑定\"},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"贵州茅台\",\"grow\":1},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"stock.sh600519.price\",\"grow\":1},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"stock.sh600519.pct\",\"grow\":1}]},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"PE / PB\",\"grow\":1},"
    "{\"type\":\"label\",\"bind\":\"stock.sh600519.pe\",\"grow\":1},"
    "{\"type\":\"label\",\"bind\":\"stock.sh600519.pb\",\"grow\":1}]},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"市值/换手\",\"grow\":1},"
    "{\"type\":\"label\",\"bind\":\"stock.sh600519.market_cap\",\"grow\":1},"
    "{\"type\":\"label\",\"bind\":\"stock.sh600519.turnover\",\"grow\":1}]},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"腾讯控股\",\"grow\":1},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"stock.hk00700.price\",\"grow\":1},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"stock.hk00700.pct\",\"grow\":1}]},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"市值/PB\",\"grow\":1},"
    "{\"type\":\"label\",\"bind\":\"stock.hk00700.market_cap\",\"grow\":1},"
    "{\"type\":\"label\",\"bind\":\"stock.hk00700.pb\",\"grow\":1}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"bind\":\"stock.sh600519.time\"}]}]}}";

// Stage B（media）：正在播放 + 进度条 + 控制排 + 曲目列表 的媒体控制卡。title/state/进度
// 全 bind media.* 路径（1Hz PublishLive 自动刷新）；三个按钮 invoke media.prev/toggle/next；
// list 行 tap → set media.play_index {i} 切曲（value 用字符串 "{i}"，行替换后 atoi）。
constexpr const char* kCardMediaCtl =
    "{\"display\":\"overlay\",\"data\":{\"tracks\":[{\"title\":\"曲目 1\"},{\"title\":\"曲目 2\"},"
    "{\"title\":\"曲目 3\"}]},\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"MEDIA\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"bind\":\"media.title\"},"
    "{\"type\":\"row\",\"gap\":10,\"children\":["
    "{\"type\":\"label\",\"role\":\"caption\",\"bind\":\"media.state\",\"grow\":1},"
    "{\"type\":\"label\",\"role\":\"caption\",\"bind\":\"media.position_s\",\"fmt\":\"%ds\","
    "\"mono\":true,\"grow\":1}]},"
    "{\"type\":\"bar\",\"min\":0,\"max\":100,\"bind\":\"media.progress_pct\"},"
    "{\"type\":\"row\",\"gap\":8,\"children\":["
    "{\"type\":\"button\",\"text\":\"上一\",\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"media.prev\"}]},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"播放/暂停\",\"on_click\":[{\"do\":"
    "\"invoke\",\"cmd\":\"media.toggle\"}]},"
    "{\"type\":\"button\",\"text\":\"下一\",\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"media.next\"}]}]},"
    "{\"type\":\"list\",\"bind_data\":\"tracks\",\"max\":8,\"item\":{\"type\":\"button\",\"text\":"
    "\"{item.title}\",\"on_click\":[{\"do\":\"set\",\"path\":\"media.play_index\",\"value\":"
    "\"{i}\"}]}}]}}";

// stock_chart 控件验收卡（idx 18）：真网拉行情；验周期分段按钮 + 图面按住十字线取值。
// w 520 收进 overlay wrapper（80% 屏宽 - 卡片 pad）不裁边；不给 h——节点 h 会被通用
// ApplySizing 打在控件根上把脚部裁掉（canvas 高度用默认 260 即可）。
constexpr const char* kCardStockChart =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"stock_chart\",\"symbol\":\"sh600519\",\"name\":\"贵州茅台\",\"w\":520}]}}";

// 样式收敛验收卡 S1（idx 19，Commit 1 硬验收）：四 variant 按钮 + 全控件家福，一张卡
// 覆盖所有走共享/局部几何样式的入口——primary/default/ghost/plain 按钮（s_btn_base /
// s_ghost_extra / s_transp_bg）、slider（s_round_track/s_track_indic/s_knob）、bar、
// switch、choice（choice-box 局部 radius/pad + s_choice_seg）、divider、fill 容器
// （ApplyFill 局部 radius）、卡面（局部 radius/pad/border）。全静态值 → 完全确定，
// 收敛前后逐像素可比。
constexpr const char* kCardStyleFam =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":14,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"STYLE FAMILY\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"样式家福\"},"
    "{\"type\":\"row\",\"gap\":10,\"children\":["
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"主\"},"
    "{\"type\":\"button\",\"variant\":\"default\",\"text\":\"默认\"},"
    "{\"type\":\"button\",\"variant\":\"ghost\",\"text\":\"描边\"},"
    "{\"type\":\"button\",\"variant\":\"plain\",\"text\":\"纯文\"}]},"
    "{\"type\":\"slider\",\"value\":40},"
    "{\"type\":\"bar\",\"value\":65},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"开关\"},"
    "{\"type\":\"spacer\"},{\"type\":\"switch\",\"checked\":true}]},"
    "{\"type\":\"choice\",\"id\":\"seg\",\"options\":[\"低\",\"中\",\"高\"],\"value\":1},"
    "{\"type\":\"divider\"},"
    "{\"type\":\"column\",\"fill\":\"card2\",\"gap\":6,\"children\":["
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"fill 容器（圆角 radius 局部设）\"}]}]}}";

// justify 全枚举验收卡 A1（idx 20，Commit 2）：6 行分别 justify=start/center/end/between/
// around/evenly，每行 3 个自然宽 icon（row 内 icon 不 grow）→ 主轴分布差异可见。全静态。
constexpr const char* kCardJustify =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"justify 全枚举\"},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"START\"},"
    "{\"type\":\"row\",\"justify\":\"start\",\"children\":[{\"type\":\"icon\",\"icon\":\"circle\"},"
    "{\"type\":\"icon\",\"icon\":\"circle\"},{\"type\":\"icon\",\"icon\":\"circle\"}]},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"CENTER\"},"
    "{\"type\":\"row\",\"justify\":\"center\",\"children\":[{\"type\":\"icon\",\"icon\":\"circle\"},"
    "{\"type\":\"icon\",\"icon\":\"circle\"},{\"type\":\"icon\",\"icon\":\"circle\"}]},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"END\"},"
    "{\"type\":\"row\",\"justify\":\"end\",\"children\":[{\"type\":\"icon\",\"icon\":\"circle\"},"
    "{\"type\":\"icon\",\"icon\":\"circle\"},{\"type\":\"icon\",\"icon\":\"circle\"}]},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"BETWEEN\"},"
    "{\"type\":\"row\",\"justify\":\"between\",\"children\":[{\"type\":\"icon\",\"icon\":\"circle\"},"
    "{\"type\":\"icon\",\"icon\":\"circle\"},{\"type\":\"icon\",\"icon\":\"circle\"}]},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"AROUND\"},"
    "{\"type\":\"row\",\"justify\":\"around\",\"children\":[{\"type\":\"icon\",\"icon\":\"circle\"},"
    "{\"type\":\"icon\",\"icon\":\"circle\"},{\"type\":\"icon\",\"icon\":\"circle\"}]},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"EVENLY\"},"
    "{\"type\":\"row\",\"justify\":\"evenly\",\"children\":[{\"type\":\"icon\",\"icon\":\"circle\"},"
    "{\"type\":\"icon\",\"icon\":\"circle\"},{\"type\":\"icon\",\"icon\":\"circle\"}]}]}}";

// align 全枚举验收卡 A2（idx 21，Commit 2）：3 行分别 align=start/center/end，每行含
// 大 icon(size40)+文字+小 icon(size18) 高差 → 交叉轴（竖向）对齐差异可见。全静态。
constexpr const char* kCardAlign =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"align 交叉轴\"},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"row align=start\"},"
    "{\"type\":\"row\",\"align\":\"start\",\"children\":[{\"type\":\"icon\",\"icon\":\"sun\",\"size\":40},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"顶对齐\"},{\"type\":\"icon\",\"icon\":\"dot\",\"size\":18}]},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"row align=center\"},"
    "{\"type\":\"row\",\"align\":\"center\",\"children\":[{\"type\":\"icon\",\"icon\":\"sun\",\"size\":40},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"中对齐\"},{\"type\":\"icon\",\"icon\":\"dot\",\"size\":18}]},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"row align=end\"},"
    "{\"type\":\"row\",\"align\":\"end\",\"children\":[{\"type\":\"icon\",\"icon\":\"sun\",\"size\":40},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"底对齐\"},{\"type\":\"icon\",\"icon\":\"dot\",\"size\":18}]}]}}";

// grid 验收卡（Commit 3）。全静态，可视验证行主序自动放置 + span + auto 轨道 + col_align 分派。
// G1（idx22）：基础表格 cols[2,1,1] + 表头 section + divider span3 全宽 + 数据行。
constexpr const char* kCardGridBasic =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"网格表格\"},"
    "{\"type\":\"grid\",\"cols\":[2,1,1],\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"项目\"},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"今日\"},"
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"昨日\"},"
    "{\"type\":\"divider\",\"span\":3},"
    "{\"type\":\"label\",\"text\":\"温度\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"24\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"text\":\"22\"},"
    "{\"type\":\"label\",\"text\":\"湿度\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"60\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"text\":\"55\"},"
    "{\"type\":\"label\",\"text\":\"气压\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"1013\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"text\":\"1009\"}]}]}}";
// G2（idx23）：auto 轨道——首列按内容宽（LV_GRID_CONTENT），次列 fr:1 铺满。
constexpr const char* kCardGridAuto =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"auto 轨道\"},"
    "{\"type\":\"grid\",\"cols\":[\"auto\",1],\"gap\":10,\"children\":["
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"名称\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"Metalio Claw\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"固件\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"Claw6 v1\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"网络\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"4G\"}]}]}}";
// G3（idx24）：控件混排——icon/switch 靠列首(START)、slider/label 铺满(STRETCH)。
constexpr const char* kCardGridCtl =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"控件混排\"},"
    "{\"type\":\"grid\",\"cols\":[1,2],\"gap\":12,\"children\":["
    "{\"type\":\"icon\",\"icon\":\"volume\"},{\"type\":\"slider\",\"value\":60},"
    "{\"type\":\"icon\",\"icon\":\"sun\"},{\"type\":\"slider\",\"value\":40},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"开关\"},{\"type\":\"switch\",\"checked\":true}]}]}}";
// G5（idx25）：超高 grid（根 grid，得卡面）——22 行溢出 overlay 86% 高封顶，触发
// ReflowOverlay 固定高度 + 竖向滚动，验 grid 布局与 overlay 滚动兼容不裁不崩。
constexpr const char* kCardGridTall =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"grid\",\"cols\":[1,1],\"gap\":8,\"children\":["
    "{\"type\":\"label\",\"role\":\"section\",\"text\":\"KEY\"},{\"type\":\"label\",\"role\":\"section\",\"text\":\"VAL\"},"
    "{\"type\":\"label\",\"text\":\"行01\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v01\"},"
    "{\"type\":\"label\",\"text\":\"行02\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v02\"},"
    "{\"type\":\"label\",\"text\":\"行03\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v03\"},"
    "{\"type\":\"label\",\"text\":\"行04\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v04\"},"
    "{\"type\":\"label\",\"text\":\"行05\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v05\"},"
    "{\"type\":\"label\",\"text\":\"行06\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v06\"},"
    "{\"type\":\"label\",\"text\":\"行07\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v07\"},"
    "{\"type\":\"label\",\"text\":\"行08\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v08\"},"
    "{\"type\":\"label\",\"text\":\"行09\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v09\"},"
    "{\"type\":\"label\",\"text\":\"行10\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v10\"},"
    "{\"type\":\"label\",\"text\":\"行11\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v11\"},"
    "{\"type\":\"label\",\"text\":\"行12\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v12\"},"
    "{\"type\":\"label\",\"text\":\"行13\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v13\"},"
    "{\"type\":\"label\",\"text\":\"行14\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v14\"},"
    "{\"type\":\"label\",\"text\":\"行15\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v15\"},"
    "{\"type\":\"label\",\"text\":\"行16\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v16\"},"
    "{\"type\":\"label\",\"text\":\"行17\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v17\"},"
    "{\"type\":\"label\",\"text\":\"行18\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v18\"},"
    "{\"type\":\"label\",\"text\":\"行19\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v19\"},"
    "{\"type\":\"label\",\"text\":\"行20\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v20\"},"
    "{\"type\":\"label\",\"text\":\"行21\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v21\"},"
    "{\"type\":\"label\",\"text\":\"行22\"},{\"type\":\"label\",\"role\":\"value\",\"text\":\"v22\"}]}}";

constexpr const char* kCards[] = {kCard0,        kCard1,          kCard2,       kCard3,
                                  kCard4,        kCard5,          kCardBadFmt,  kCardArc,
                                  kCardQr,       kCardChoice,     kCardPatch,   kCardP4aInfo,
                                  kCardP4aChart, kCardP4bSensors, kCardP4cGps,  kCardMultiCol,
                                  kCardStockBind, kCardMediaCtl,  kCardStockChart, kCardStyleFam,
                                  kCardJustify,  kCardAlign,      kCardGridBasic, kCardGridAuto,
                                  kCardGridCtl,  kCardGridTall};

// TEMP SCAFFOLD（B 验收 §4 断言 3/5 的负向用例）：qrcode text 超 256 字节 / choice 只给 1 项，
// 均应在 worker 侧同步被 Validate 拒绝，而非渲染出半张卡。
std::string BuildLongQrCard() {
    std::string text(300, 'a');
    return "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"children\":["
           "{\"type\":\"qrcode\",\"text\":\"" + text + "\"}]}}";
}
constexpr const char* kCardChoiceBad =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"choice\",\"options\":[\"仅一项\"]}]}}";

// TEMP SCAFFOLD（verifier minor：数值控件 bind 到 String 路径的负向用例）：slider 绑
// net.ssid（String）应被 ValidateNode 的新增类型检查同步拒绝，而不是渲染出读垃圾值的滑条。
constexpr const char* kCardSliderStringBind =
    "{\"root\":{\"type\":\"column\",\"children\":[{\"type\":\"slider\",\"bind\":\"net.ssid\"}]}}";

// TEMP SCAFFOLD（B 验收 §4 断言 7）：2 个 primary 按钮 + 无 label + on_change 挂 report 的
// 死 slider——一次触发 Lint 的多条规则，验证 hints 数组非阻断地搭在 render 返回值里。
constexpr const char* kCardHints =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"slider\",\"on_change\":[{\"do\":\"report\",\"text\":\"{v}\"}]},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"A\"},"
    "{\"type\":\"button\",\"variant\":\"primary\",\"text\":\"B\"}]}}";

// TEMP SCAFFOLD（B 验收 §4 断言 18 的可写 bind 半支）：choice 绑到可写 bool 路径 ui.theme
// （0=dark/1=light，正好落在 choice 的 2 项区间内），点第二段应立即回写主题、屏幕跟着变色。
constexpr const char* kCardChoiceBind =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"主题\"},"
    "{\"type\":\"choice\",\"bind\":\"ui.theme\",\"options\":[\"深色\",\"浅色\"]}]}}";

// 7 超高卡片（overlay 高度封顶 + 内部滚动的稳定性验证），运行时拼多行。
std::string BuildTallCard() {
    static const char* icons[] = {"volume", "sun", "battery", "wifi", "gear", "clock", "info", "music"};
    std::string s = "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":[";
    s += "{\"type\":\"label\",\"role\":\"title\",\"text\":\"很高的卡片 / 滚动\"},";
    const int rows = 20;  // 每行 3 节点 → 20 行 61 节点，压进 64 上限；总高超屏 → 滚动
    for (int i = 0; i < rows; i++) {
        char row[200];
        std::snprintf(row, sizeof(row),
                      "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"%s\"},"
                      "{\"type\":\"label\",\"role\":\"value\",\"text\":\"ITEM %02d      OK\"}]}%s",
                      icons[i % 8], i + 1, i < rows - 1 ? "," : "");
        s += row;
    }
    s += "]}}";
    return s;
}

// TEMP SCAFFOLD (overlay reflow re-entrancy check): a 20-row overlay card where every
// row carries an id + starts individually hidden/visible, so a single `showrows <n>`
// command can grow/shrink the rendered content across the 86%-height cap through the
// real ui_update tool path — exercising pi_card::ReflowOverlay's reentrant branch
// (fixed-height+scroll <-> SIZE_CONTENT) the same way ApplyProps would trigger it.
std::string BuildGrowCard() {
    static const char* icons[] = {"volume", "sun", "battery", "wifi", "gear", "clock", "info", "music"};
    std::string s = "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":[";
    s += "{\"type\":\"label\",\"role\":\"title\",\"text\":\"reflow 重入测试\"},";
    const int rows = 20;  // 全部展开时自然高度 > 86% 屏高（同 BuildTallCard）；前 8 行默认可见时远低于封顶。
    for (int i = 0; i < rows; i++) {
        char row[220];
        std::snprintf(row, sizeof(row),
                      "{\"type\":\"row\",\"id\":\"row%d\",\"hidden\":%s,\"children\":"
                      "[{\"type\":\"icon\",\"icon\":\"%s\"},{\"type\":\"label\",\"role\":\"value\","
                      "\"text\":\"ROW %02d\"}]}%s",
                      i, i < 8 ? "false" : "true", icons[i % 8], i, i < rows - 1 ? "," : "");
        s += row;
    }
    s += "]}}";
    return s;
}

void RenderGrowCard() {
    std::string spec = BuildGrowCard();
    cJSON* args = cJSON_Parse(spec.c_str());
    if (!args) {
        std::fprintf(stderr, "[sim] growcard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] growcard render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD: show rows [0,n) and hide [n,20) of the growcard via the real
// ui_update tool path (pi_ui_queue -> DrainQueueTick on the LVGL thread, same route
// the LLM's ui_update calls take) — not a shortcut into pi_card_host internals.
void ShowRows(int n) {
    for (int i = 0; i < 20; i++) {
        cJSON* args = cJSON_CreateObject();
        char id[16];
        std::snprintf(id, sizeof(id), "row%d", i);
        cJSON_AddStringToObject(args, "id", id);
        cJSON* props = cJSON_AddObjectToObject(args, "props");
        cJSON_AddBoolToObject(props, "hidden", i >= n);
        bool is_err = false;
        char* res = pi_card_tool_update(args, &is_err);
        free(res);
        cJSON_Delete(args);
    }
}

// TEMP SCAFFOLD (改造10 acceptance): render a card with a label bound to the writable Int path
// audio.volume — used with `numset` below to observe the manual-observer + 250ms interpolation
// added in pi_card_render.cc's ApplyBind (replacing the native lv_label_bind_text one-shot jump
// for non-String binds) — plus a second label bound to the String path net.ssid, to confirm the
// String branch still uses the untouched native lv_label_bind_text (no animation, no crash/tofu).
// Also exercises the entrance fade+slide (PlayCardEntrance, pi_card_render.cc) since it's an
// overlay render, with id "vol" left on the card root so `ui_close` can target it for the
// mid-animation-delete regression check.
void RenderNumAnimCard() {
    static const char* kSpec =
        "{\"display\":\"overlay\",\"card\":\"vol\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":["
        "{\"type\":\"label\",\"role\":\"title\",\"text\":\"数值滚动测试\"},"
        "{\"type\":\"label\",\"id\":\"vol\",\"role\":\"value\",\"bind\":\"audio.volume\","
        "\"fmt\":\"%d%%\",\"mono\":true},"
        "{\"type\":\"label\",\"role\":\"caption\",\"bind\":\"net.ssid\",\"fmt\":\"%s\"}]}}";
    cJSON* args = cJSON_Parse(kSpec);
    if (!args) {
        std::fprintf(stderr, "[sim] numanimcard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] numanimcard render: %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD (改造10 acceptance): numset <path> <value> — writes the DataHub subject
// directly (bypassing touch/slider drag) so the manual observer's 250ms interpolation fires
// deterministically at a known moment for screenshot timing.
void ExecNumSet(const std::string& path, int value) {
    lv_subject_t* subj = pi_card::DataHub::Instance().Acquire(path);
    if (!subj) {
        std::fprintf(stderr, "[sim] numset: unknown path '%s'\n", path.c_str());
        return;
    }
    lv_subject_set_int(subj, value);
    std::fprintf(stderr, "[sim] numset %s -> %d\n", path.c_str(), value);
}

// TEMP SCAFFOLD (改造10 acceptance c): strset <path> <text> — direct subject write for String
// paths (net.ssid has no setter, so ExecNumSet's DataHub::Write path doesn't apply here);
// confirms the untouched native lv_label_bind_text branch still renders String binds correctly
// (including CJK, exercising the SafeFont mono->puhui fallback) with no animation.
void ExecStrSet(const std::string& path, const std::string& text) {
    lv_subject_t* subj = pi_card::DataHub::Instance().Acquire(path);
    if (!subj) {
        std::fprintf(stderr, "[sim] strset: unknown path '%s'\n", path.c_str());
        return;
    }
    lv_subject_copy_string(subj, text.c_str());
    std::fprintf(stderr, "[sim] strset %s -> '%s'\n", path.c_str(), text.c_str());
}

// TEMP SCAFFOLD (改造1 acceptance #2): previewfeed <file> — feeds a sequence of partial-JSON
// snapshots (one per line, each the FULL accumulated args-so-far, matching how
// pi_agent_task.c's UI_TOOL_ARGS actually behaves) straight into
// pi_card::PreviewOnArgs, bypassing the queue/drain-tick entirely — deterministic, no real LLM
// needed. Calls PreviewOnToolStart("ui_render") once up front (NOT per line — doing it per line
// would tear down the very session we're trying to grow). After each line, logs the tree root
// and its child-0 pointer so a human/script can diff across frames and confirm committed
// subtrees keep the same lv_obj_t* (the acceptance criterion).
void DumpPreviewNode(lv_obj_t* obj, int depth);  // 前向声明：定义见下方（迟到属性取证要逐帧转储）

void ExecPreviewFeed(const std::string& path) {
    std::ifstream f(path);
    if (!f.good()) {
        std::fprintf(stderr, "[sim] previewfeed: can't open '%s'\n", path.c_str());
        return;
    }
    pi_card::PreviewOnToolStart("ui_render");
    uint32_t gen = pi_agent_task_session_gen();  // 传真实当前代次，避免下个 drain tick 被判过期撤除
    std::string line;
    int frame = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        frame++;
        pi_card::PreviewOnArgs(line.c_str(), gen);
        lv_obj_t* tree = pi_card::PreviewDebugTree();
        lv_obj_t* child0 = (tree && lv_obj_get_child_count(tree) > 0) ? lv_obj_get_child(tree, 0) : nullptr;
        std::fprintf(stderr, "[sim][previewfeed] frame %d: tree=%p child0=%p children=%u\n", frame,
                     static_cast<void*>(tree), static_cast<void*>(child0),
                     tree ? lv_obj_get_child_count(tree) : 0);
        // 迟到属性修复取证（任务分配 #2）：每帧转储一次，逐帧 diff 才能看出 role/grow/justify
        // 这些容器/叶子属性是不是"到帧就生效"而不是憋到 adopt 才跳变。
        std::fprintf(stderr, "[sim][previewfeed] frame %d dump:\n", frame);
        DumpPreviewNode(tree, 0);
    }
}

// TEMP SCAFFOLD (verify-m1 counterexample; extended for the 迟到属性 fix acceptance): recursively
// dump the live preview tree so a human can eyeball structure (order/duplicates/rebuilds) AND the
// layout-affecting properties that are supposed to now update mid-growth instead of only at
// adopt: label font pointer (role → font swap), computed pixel width + flex_grow (grow:1), and
// the container's flex main/cross place enum (justify/align).
void DumpPreviewNode(lv_obj_t* obj, int depth) {
    if (!obj) return;
    const char* kind = "obj";
    char extra[192] = "";
    int off = 0;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        kind = "label";
        off += std::snprintf(extra + off, sizeof(extra) - off, " text=\"%s\" font=%p",
                              lv_label_get_text(obj),
                              static_cast<const void*>(lv_obj_get_style_text_font(obj, LV_PART_MAIN)));
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
        kind = "button";
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        kind = "slider";
    }
    off += std::snprintf(extra + off, sizeof(extra) - off,
                          " w=%d grow=%u main=%d cross=%d radius=%d hidden=%d",
                          static_cast<int>(lv_obj_get_width(obj)),
                          static_cast<unsigned>(lv_obj_get_style_flex_grow(obj, LV_PART_MAIN)),
                          static_cast<int>(lv_obj_get_style_flex_main_place(obj, LV_PART_MAIN)),
                          static_cast<int>(lv_obj_get_style_flex_cross_place(obj, LV_PART_MAIN)),
                          static_cast<int>(lv_obj_get_style_radius(obj, LV_PART_MAIN)),
                          lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) ? 1 : 0);
    std::fprintf(stderr, "[sim][previewdump] %*s%s %p%s\n", depth * 2, "", kind,
                 static_cast<void*>(obj), extra);
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) DumpPreviewNode(lv_obj_get_child(obj, i), depth + 1);
}
void ExecPreviewDump() {
    lv_obj_t* tree = pi_card::PreviewDebugTree();
    std::fprintf(stderr, "[sim][previewdump] === tree root=%p ===\n", static_cast<void*>(tree));
    DumpPreviewNode(tree, 0);
}

// TEMP SCAFFOLD (改造1 acceptance #4/#5): previewend / bargein — drive the two non-adopt
// teardown paths without needing a real LLM round-trip. previewend calls PreviewOnToolEnd
// directly (simulates "tool execute finished, no UI_CARD_RENDER followed" — e.g. validation
// failed); bargein calls pi_agent_task_new_session() directly (simulates a barge-in/new session
// bumping the session gen mid-stream) — the actual teardown then happens on the next
// DrainQueueTick via PreviewCheckGen, same as the real path.
void ExecPreviewEnd() {
    pi_card::PreviewOnToolEnd();
    std::fprintf(stderr, "[sim] previewend: tree=%p\n", static_cast<void*>(pi_card::PreviewDebugTree()));
}
void ExecBargeIn() {
    pi_agent_task_new_session();
    std::fprintf(stderr, "[sim] bargein: session gen bumped\n");
}

// TEMP SCAFFOLD (merge regression: preview vs. formal render visual A/B): rendercard <file> —
// read one raw ui_render JSON spec (a "card" id + "root", same shape RenderBadCard uses) from a
// file and push it through the real (non-preview) pi_card_tool_render path, so a previewfeed
// frame's final JSON can be screenshotted twice — once via the preview tree, once via the formal
// tree — for a pixel comparison.
void ExecRenderJson(const std::string& path) {
    std::ifstream f(path);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    cJSON* args = cJSON_Parse(text.c_str());
    if (!args) {
        std::fprintf(stderr, "[sim] rendercard: JSON parse failed for '%s'\n", path.c_str());
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim][rendercard] -> %s (%s)\n", res ? res : "(null)", is_err ? "ERR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD (改造10 acceptance): closecard <id> — real ui_close, used to check that
// deleting a card while its entrance/num-scroll lv_anim_t is still in flight doesn't crash
// (regression for PlayCardEntrance/NumScrollObserverCb using the label/tree object itself as
// the anim var, relying on LVGL's auto-cleanup-on-delete instead of a manual DELETE callback).
void ExecCloseCard(const std::string& id) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", id.c_str());
    bool is_err = false;
    char* res = pi_card_tool_close(args, &is_err);
    std::fprintf(stderr, "[sim] closecard %s: %s (%s)\n", id.c_str(), res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD (ui_update batch `patches` acceptance): one real ui_update call against
// BuildGrowCard()'s row0..row19 with a `patches` array — 2 existing ids (row0/row1, both
// currently visible) + 1 missing id, to see 2 nodes flip hidden in a single call plus one
// aggregated async error, via the same real tool path ShowRows() exercises per-id.
void ExecPatchTest() {
    cJSON* args = cJSON_CreateObject();
    cJSON* patches = cJSON_AddArrayToObject(args, "patches");
    auto add_patch = [&](const char* id, bool hidden) {
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", id);
        cJSON* props = cJSON_AddObjectToObject(p, "props");
        cJSON_AddBoolToObject(props, "hidden", hidden);
        cJSON_AddItemToArray(patches, p);
    };
    add_patch("row0", true);
    add_patch("row1", true);
    add_patch("rowMissing", true);  // does not exist -> aggregated async error
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] patchtest ui_update: %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD (P1 report 状态快照 + P2 本地 toggle 披露)：一张表单卡——
//   * qty(slider) / urgent(switch)：无 bind 只有 id 的**纯本地表单控件**。既验证它们没被
//     「死控件兜底」DIM 掉（pi_card_render.cc 的 live 判据第 4 条），也验证 report 会自动
//     带回它们的值。
//   * vol(slider)：bind 到硬件路径，验证 bind 控件的值也搭 report 顺风车回传。
//   * 「查看详情」按钮 → {do:'toggle',target:'detail'}：纯本地展开/收起，零 LLM 往返，且会
//     触发 overlay 卡的 ReflowOverlay 重入。
//   * 「确认下单」按钮 → {do:'report'}：看注入文本是否带全状态。
constexpr const char* kFormCard =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"eyebrow\",\"text\":\"订单\"},"
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"确认下单\"},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"数量\",\"grow\":1},"
    "{\"type\":\"slider\",\"id\":\"qty\",\"min\":1,\"max\":10,\"value\":3,\"grow\":2}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"加急\",\"grow\":1},"
    "{\"type\":\"switch\",\"id\":\"urgent\",\"checked\":false}]},"
    "{\"type\":\"row\",\"children\":[{\"type\":\"label\",\"role\":\"label\",\"text\":\"音量\",\"grow\":1},"
    "{\"type\":\"slider\",\"id\":\"vol\",\"bind\":\"audio.volume\",\"grow\":2}]},"
    "{\"type\":\"button\",\"text\":\"查看详情\",\"variant\":\"ghost\","
    "\"on_click\":[{\"do\":\"toggle\",\"target\":\"detail\"}]},"
    // 详情块特意做大：展开后自然高度**跨过** 86% 屏高的封顶，好让 toggle 走一遍
    // ReflowOverlay 的「SIZE_CONTENT ↔ 钉死+开滚动」切换。注意这条链路是
    // DispatchCb → ReflowOverlay，与 ui_update → OnUpdateEvent 那条（growcard/showrows 测的）
    // 是**不同的调用点**，两边都得验。
    "{\"type\":\"column\",\"id\":\"detail\",\"hidden\":true,\"gap\":6,\"children\":["
    "{\"type\":\"divider\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"预计送达 30 分钟\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"配送费 5 元\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"支持 7 天无理由退换\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"商家：茶话弄（软件园店）\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"骑手：王师傅 · 距您 1.2 公里\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"下单时间：今天 14:32\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"优惠：满 20 减 3\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"备注：少糖、去冰\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"发票：电子普通发票\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"客服电话：400-123-4567\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"订单号：2026071500391\"},"
    "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"配送方式：专送\"}]},"
    "{\"type\":\"button\",\"text\":\"确认下单\",\"variant\":\"primary\","
    "\"on_click\":[{\"do\":\"report\",\"text\":\"确认下单\"}]}]}}";

// TEMP SCAFFOLD: 校验器负向用例——target 指向一个卡里根本没声明的 id。应在 worker 侧**同步**
// 被拒（错误直接回给 LLM 重试），而不是渲染出一个点了没反应的按钮。
constexpr const char* kBadToggleCard =
    "{\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"button\",\"text\":\"坏按钮\",\"on_click\":[{\"do\":\"toggle\",\"target\":\"nope\"}]},"
    "{\"type\":\"label\",\"id\":\"real\",\"text\":\"我才是存在的 id\"}]}}";

// TEMP SCAFFOLD: target 缺失的负向用例。
constexpr const char* kNoTargetCard =
    "{\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"button\",\"text\":\"无 target\",\"on_click\":[{\"do\":\"show\"}]}]}}";

// ============================================================================
// Phase2 演示卡：spec/data 分离 + list 控件 + preset + 字符串回流。
// ============================================================================

// T1/T4：data 驱动的 list——5 条待办，行模板 {n}. {item.name} + role=value 的 {item.price}。
// 固定 card id "datacard" 供 dataop 命令后续 ui_update。
constexpr const char* kCardList =
    "{\"card\":\"datacard\",\"display\":\"overlay\",\"data\":{\"items\":["
    "{\"name\":\"苹果\",\"price\":\"12\"},{\"name\":\"香蕉\",\"price\":\"6\"},"
    "{\"name\":\"橙子\",\"price\":\"9\"},{\"name\":\"葡萄\",\"price\":\"20\"},"
    "{\"name\":\"西瓜\",\"price\":\"30\"}]},"
    "\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"购物清单\"},"
    // max:8 显式高于初始 5 条：eff_max 在 render 期一次性算定（D2/D3），append 到第 6/7/8 条
    // 才会真正多出一行；不给 max 则默认 eff_max=初始长度，append 会被截断（这是设计使然，
    // 见 spec D3），但那样就演示不出"截图行数 +1"，故这里显式留出余量。
    "{\"type\":\"list\",\"bind_data\":\"items\",\"max\":8,\"empty\":\"清单为空\",\"item\":{\"type\":\"row\","
    "\"children\":[{\"type\":\"label\",\"text\":\"{n}. {item.name}\",\"grow\":1},"
    "{\"type\":\"label\",\"role\":\"value\",\"text\":\"¥{item.price}\"}]}}]}}";

// 改造4：行模板不含 {i}/{n}（只用 {item.*}）——tpl_uses_index 应算 false，走行级 fast path。
// max:4、初始 2 条：append 到 3/4 条正常新增行，第 3 次 append（会让底层数组到 5 条）应该
// "截断区不补行"（可见行数钉在 4，不再新增）。固定 card id "datacard2"，供 listfastop 命令用。
constexpr const char* kCardListFast =
    "{\"card\":\"datacard2\",\"display\":\"overlay\",\"data\":{\"items\":["
    "{\"name\":\"苹果\",\"price\":\"12\"},{\"name\":\"香蕉\",\"price\":\"6\"}]},"
    "\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"快路径清单\"},"
    "{\"type\":\"list\",\"bind_data\":\"items\",\"max\":4,\"empty\":\"清单为空\",\"item\":{\"type\":\"row\","
    "\"children\":[{\"type\":\"label\",\"text\":\"{item.name}\",\"grow\":1},"
    "{\"type\":\"label\",\"role\":\"value\",\"text\":\"¥{item.price}\"}]}}]}}";

// T2：list max:20 × 5 节点/行的行模板 = 100 > 64，validate 应同步拒绝（"list reserves"）。
constexpr const char* kCardListOverflow =
    "{\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"list\",\"bind_data\":\"items\",\"max\":20,\"item\":{\"type\":\"row\",\"children\":["
    "{\"type\":\"icon\",\"icon\":\"dot\"},{\"type\":\"label\",\"text\":\"{item.name}\"},"
    "{\"type\":\"label\",\"text\":\"{item.price}\"},{\"type\":\"button\",\"text\":\"A\"},"
    "{\"type\":\"button\",\"text\":\"B\"}]}}]}}";

// T3：行模板里挂 toggle——validate 应同步拒绝（"list row templates can't use"）。
constexpr const char* kCardListBadAction =
    "{\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"list\",\"bind_data\":\"items\",\"item\":{\"type\":\"row\",\"id\":\"row\",\"children\":["
    "{\"type\":\"label\",\"text\":\"{item.name}\"},{\"type\":\"button\",\"text\":\"展开\","
    "\"on_click\":[{\"do\":\"toggle\",\"target\":\"row\"}]}]}}]}}";

// verifier 判 medium fix 2 负例：list 的 item 模板里再嵌一层 list——外层重渲时 lv_obj_clean
// 整个行子树会把内层 list 的 DataConsumer/json_pool 变悬垂指针，且每次重渲无界增长；
// validate 应同步拒绝（"a list can't nest inside another list's item template"）。
constexpr const char* kCardListNested =
    "{\"data\":{\"outer\":[{\"inner\":[{\"t\":\"x\"}]}]},\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"list\",\"bind_data\":\"outer\",\"item\":{\"type\":\"list\",\"bind_data\":\"inner\","
    "\"item\":{\"type\":\"label\",\"text\":\"{item.t}\"}}}]}}";

// T7：choice 的 on_change 直接 report "你选了{label}"——{label} 只在**触发控件本身就是
// choice** 时有值（ChoiceLabel(target,...) 里 target=触发控件；隔壁按钮点它是取不到 choice
// 的 label 的，故本卡把 report 挂在 choice 自己身上，而非另一个确认按钮上）。
constexpr const char* kCardChoiceLabel =
    "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":16,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"口味\"},"
    "{\"type\":\"choice\",\"id\":\"flavor\",\"options\":[\"甜\",\"辣\",\"酸\"],\"value\":0,"
    "\"on_change\":[{\"do\":\"report\",\"text\":\"你选了{label}\"}]}]}}";

// T8：data.status + bind_data label（text 带 {value} 内联模板），固定 id 供 dataop set。
constexpr const char* kCardDataLabel =
    "{\"card\":\"datalabel\",\"display\":\"overlay\",\"data\":{\"status\":\"备餐中\"},"
    "\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"订单状态\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind_data\":\"status\",\"text\":\"状态：{value}\"}]}}";

void RenderBadCard(const char* spec, const char* tag) {
    cJSON* args = cJSON_Parse(spec);
    if (!args) {
        std::fprintf(stderr, "[sim] %s JSON parse failed\n", tag);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim][negative] %-10s -> %s (%s)\n", tag, res ? res : "(null)",
                 is_err ? "已拒绝 ✓" : "竟然通过了 ✗");
    free(res);
    cJSON_Delete(args);
}

// ============================================================================
// Phase3 演示卡：常驻小组件（display:'standby'）+ invoke 命令 + chart。
// ============================================================================

// 常驻卡：电量数值 + battery.level 历史折线 + 两个 invoke 按钮（safe/confirm 各一）。
// 演示 display:'standby' 的完整能力面——pin host 布局、chart 历史绑定、invoke 分发。
constexpr const char* kCardStandby =
    "{\"display\":\"standby\",\"root\":{\"type\":\"column\",\"gap\":10,\"children\":["
    "{\"type\":\"row\",\"children\":[{\"type\":\"icon\",\"icon\":\"battery\"},"
    "{\"type\":\"label\",\"role\":\"label\",\"text\":\"\xe7\x94\xb5\xe9\x87\x8f\"},"
    "{\"type\":\"spacer\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"bind\":\"battery.level\",\"fmt\":\"%d%%\"}]},"
    "{\"type\":\"chart\",\"bind_history\":\"battery.level\",\"points\":30},"
    "{\"type\":\"row\",\"gap\":10,\"children\":["
    "{\"type\":\"button\",\"text\":\"\xe9\x87\x8d\xe8\xbf\x9e\xe7\xbd\x91\xe7\xbb\x9c\","
    "\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"net.reconnect\"}]},"
    "{\"type\":\"button\",\"text\":\"\xe5\x88\x87\xe6\x8d\xa2\xe7\xbd\x91\xe7\xbb\x9c\","
    "\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"net.switch_type\"}]}"
    "]}]}}";

// 负例：chart 的 bind_history 指向一个未声明历史的路径（audio.volume 是可写 Int 但没
// keep_history）——Validate 应同步拒绝并列出可用历史路径（D16）。
constexpr const char* kCardChartBadHistory =
    "{\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"chart\",\"bind_history\":\"audio.volume\"}]}}";

// 负例：invoke 指向未注册命令（power.off 按编排者裁决压根不注册）——ValidateActions 应
// 同步拒绝并列出可用命令清单（D12）。
constexpr const char* kCardInvokeBad =
    "{\"root\":{\"type\":\"column\",\"children\":["
    "{\"type\":\"button\",\"text\":\"x\",\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"power.off\"}]}]}}";

// Commit 3 E1 grid 负例（均应被 Validate 同步拒绝，is_error=true）。
constexpr const char* kGridNoCols =
    "{\"root\":{\"type\":\"grid\",\"children\":[{\"type\":\"label\",\"text\":\"x\"}]}}";
constexpr const char* kGridEmptyCols =
    "{\"root\":{\"type\":\"grid\",\"cols\":[],\"children\":[{\"type\":\"label\",\"text\":\"x\"}]}}";
constexpr const char* kGridSevenCols =
    "{\"root\":{\"type\":\"grid\",\"cols\":[1,1,1,1,1,1,1],\"children\":[{\"type\":\"label\",\"text\":\"x\"}]}}";
constexpr const char* kGridBadElem =
    "{\"root\":{\"type\":\"grid\",\"cols\":[1,-1,1],\"children\":[{\"type\":\"label\",\"text\":\"x\"}]}}";
constexpr const char* kGridSpanOver =
    "{\"root\":{\"type\":\"grid\",\"cols\":[2,1],\"children\":["
    "{\"type\":\"label\",\"text\":\"x\",\"span\":3}]}}";
// 正例：column justify 未知值 "middle" → 不拒绝、回落默认、hints 含回落提示。
constexpr const char* kGridJustifyFallback =
    "{\"root\":{\"type\":\"column\",\"justify\":\"middle\",\"children\":["
    "{\"type\":\"label\",\"text\":\"x\"}]}}";

// 撑爆 64 节点预算：grid cols[1] + 70 个 label cell（1 grid + 70 = 71 > 64）→ 应被拒。
std::string BuildGridOverflow() {
    std::string s = "{\"root\":{\"type\":\"grid\",\"cols\":[1],\"children\":[";
    for (int i = 0; i < 70; i++) {
        if (i) s += ",";
        s += "{\"type\":\"label\",\"text\":\"n\"}";
    }
    s += "]}}";
    return s;
}

// 正例断言：期望 is_error=false 且 hints 含指定子串（回落提示）。
void RenderExpectOkWithHint(const char* spec, const char* tag, const char* want_hint) {
    cJSON* args = cJSON_Parse(spec);
    if (!args) {
        std::fprintf(stderr, "[sim] %s JSON parse failed\n", tag);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    const bool has_hint = res && std::strstr(res, want_hint) != nullptr;
    std::fprintf(stderr, "[sim][positive] %-14s -> %s (%s, hint %s)\n", tag, res ? res : "(null)",
                 is_err ? "竟被拒 ✗" : "通过 ✓", has_hint ? "含回落 ✓" : "缺回落 ✗");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（消息流「贴底跟随」验证）：往 chat feed 里追加一张普通卡片（非 overlay）。
// 每追加一张都会走 CardEndRow → ScrollFeedToBottom(false)——即模型侧输出的那条跟随分支，
// 于是不必真的调 LLM 就能验证「用户翻上去后新输出不抢视口」。
void RenderChatCard(int n) {
    char spec[512];
    std::snprintf(spec, sizeof(spec),
                  "{\"display\":\"chat\",\"root\":{\"type\":\"column\",\"gap\":6,\"children\":["
                  "{\"type\":\"label\",\"role\":\"title\",\"text\":\"消息 #%d\"},"
                  "{\"type\":\"label\",\"role\":\"caption\",\"text\":\"这是第 %d 条追加进消息流的卡片\"}]}}",
                  n, n);
    cJSON* args = cJSON_Parse(spec);
    if (!args) {
        std::fprintf(stderr, "[sim] chatcard %d JSON parse failed\n", n);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    free(res);
    cJSON_Delete(args);
}

void RenderFormCard() {
    cJSON* args = cJSON_Parse(kFormCard);
    if (!args) {
        std::fprintf(stderr, "[sim] formcard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] formcard render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（按钮 icon 支持验收）：三个纯图标播控钮 + icon+text 并存钮 +
// 两个负例（生造图标名 / 空白按钮）——正例看渲染，负例看 hints 是否当场纠正。
void RenderIconButtonCard() {
    static const char* kSpec =
        "{\"display\":\"overlay\",\"root\":{\"type\":\"column\",\"gap\":8,\"children\":["
        "{\"type\":\"label\",\"role\":\"title\",\"text\":\"图标按钮验证\"},"
        "{\"type\":\"row\",\"gap\":8,\"children\":["
        "{\"type\":\"button\",\"icon\":\"skip-back\",\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"media.prev\"}]},"
        "{\"type\":\"button\",\"icon\":\"play\",\"variant\":\"primary\",\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"media.toggle\"}]},"
        "{\"type\":\"button\",\"icon\":\"skip-forward\",\"on_click\":[{\"do\":\"invoke\",\"cmd\":\"media.next\"}]}]},"
        "{\"type\":\"row\",\"gap\":8,\"children\":["
        "{\"type\":\"button\",\"icon\":\"list-music\",\"text\":\"列表\",\"variant\":\"ghost\"},"
        "{\"type\":\"button\",\"icon\":\"totally-made-up\",\"text\":\"坏名\"},"
        "{\"type\":\"button\"}]}]}}";
    cJSON* args = cJSON_Parse(kSpec);
    if (!args) {
        std::fprintf(stderr, "[sim] iconcard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] iconcard render: %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// Phase3：渲染常驻小组件演示卡（display:'standby'）。
void RenderStandbyCard() {
    cJSON* args = cJSON_Parse(kCardStandby);
    if (!args) {
        std::fprintf(stderr, "[sim] standby JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] standby render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// Phase3 D8（verifier 修复后）：封套 >3072B 的 standby 现在在 worker 侧
// pi_card_tool_render 同步拒绝（入队前），不再入队、不再触碰既有 pin——构造一个超长
// label 文本把 spec 撑过 3KB，断言 is_error=true 且既有 pin（如果有）分毫未动。
void RenderStandbyOversized() {
    std::string huge(3200, 'x');
    std::string spec = "{\"display\":\"standby\",\"root\":{\"type\":\"column\",\"children\":["
                       "{\"type\":\"label\",\"text\":\"" + huge + "\"}]}}";
    cJSON* args = cJSON_Parse(spec.c_str());
    if (!args) {
        std::fprintf(stderr, "[sim] standbybig JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] standbybig render: %s (%s) — expect ERROR, synchronous, no NVS "
                        "write, no drain-side rollback\n",
                res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

void RenderDemoCard() {
    const char* idx_env = std::getenv("PI_SIM_CARD_IDX");
    int idx = idx_env ? std::atoi(idx_env) : 0;
    std::string tall;
    const char* spec;
    const int n_static = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    if (idx >= 0 && idx < n_static) {
        spec = kCards[idx];
    } else {
        tall = BuildTallCard();  // idx == n_static（kCards[] 实际条数，随数组增删自适应）→ 超高卡
        spec = tall.c_str();
    }
    cJSON* args = cJSON_Parse(spec);
    if (!args) {
        std::fprintf(stderr, "[sim] demo card %d JSON parse failed\n", idx);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] demo card %d render: %s (%s)\n", idx, res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（Phase2 T1/T4）：渲染 data 驱动的 kCardList（固定 card id "datacard"）。
void RenderDataCard() {
    cJSON* args = cJSON_Parse(kCardList);
    if (!args) {
        std::fprintf(stderr, "[sim] datacard JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] datacard render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（改造4 acceptance）：渲染 kCardListFast（固定 card id "datacard2"，行模板不含
// {i}/{n}，走行级 fast path）。
void RenderDataCardFast() {
    cJSON* args = cJSON_Parse(kCardListFast);
    if (!args) {
        std::fprintf(stderr, "[sim] datacard2 JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] datacard2 render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（改造4 acceptance）：listfastop <append|remove|replace|set> [index] — 走真实
// ui_update 的 data 通道，对准 "datacard2" 卡的 items 数组。append 尾插一条；remove/replace
// 需要 index（remove 缺省 0，replace 缺省 0）；set 整键换成全新数组（validate"退全量"用）。
void ExecListFastOp(const std::string& op, int index) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datacard2");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    if (op == "append") {
        cJSON* append = cJSON_AddObjectToObject(data, "append");
        cJSON_AddStringToObject(append, "key", "items");
        cJSON* item = cJSON_AddObjectToObject(append, "item");
        cJSON_AddStringToObject(item, "name", "新品");
        cJSON_AddStringToObject(item, "price", "1");
    } else if (op == "remove") {
        cJSON* remove = cJSON_AddObjectToObject(data, "remove");
        cJSON_AddStringToObject(remove, "key", "items");
        cJSON_AddNumberToObject(remove, "index", index);
    } else if (op == "replace") {
        cJSON* replace = cJSON_AddObjectToObject(data, "replace");
        cJSON_AddStringToObject(replace, "key", "items");
        cJSON_AddNumberToObject(replace, "index", index);
        cJSON* item = cJSON_AddObjectToObject(replace, "item");
        cJSON_AddStringToObject(item, "name", "换了");
        cJSON_AddStringToObject(item, "price", "99");
    } else if (op == "set") {
        cJSON* set = cJSON_AddObjectToObject(data, "set");
        cJSON* arr = cJSON_AddArrayToObject(set, "items");
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", "全新数组");
        cJSON_AddStringToObject(item, "price", "0");
        cJSON_AddItemToArray(arr, item);
    } else {
        std::fprintf(stderr, "[sim] listfastop 未知操作 '%s'（用 append|remove|replace|set）\n",
                     op.c_str());
        cJSON_Delete(args);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] listfastop %s idx=%d -> %s (%s)\n", op.c_str(), index,
                 res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（改造4 acceptance #6）：一次 ui_update 里同时塞 append+replace（同 key），验证
// "多 op 合并"——两条 op 都该在这一次调用里正确生效（append 尾插一条，replace 换掉指定行）。
void ExecListFastMultiAppendReplace() {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datacard2");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    cJSON* append = cJSON_AddObjectToObject(data, "append");
    cJSON_AddStringToObject(append, "key", "items");
    cJSON* aitem = cJSON_AddObjectToObject(append, "item");
    cJSON_AddStringToObject(aitem, "name", "多op追加");
    cJSON_AddStringToObject(aitem, "price", "2");
    cJSON* replace = cJSON_AddObjectToObject(data, "replace");
    cJSON_AddStringToObject(replace, "key", "items");
    cJSON_AddNumberToObject(replace, "index", 0);
    cJSON* ritem = cJSON_AddObjectToObject(replace, "item");
    cJSON_AddStringToObject(ritem, "name", "多op换首行");
    cJSON_AddStringToObject(ritem, "price", "3");
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] listfastmulti append+replace -> %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（改造4 acceptance #6）：一次 ui_update 里同时塞 append+set（同 key）——
// ApplyDataOps 按 set→append 的固定顺序落地两条操作（各自独立生效在 card->data 上：
// set 先把数组整个换成新内容，append 再往这个新数组末尾追一条，两条的数据效果都真实生效，
// 都能在最终数组里看到），但 RefreshDataConsumers 只因为出现了 SetWhole 就整个 key 走一次
// RefreshListFull，不会再额外为 append 单独跑一次 fast path——重建时直接读的是这一刻
// card->data 的最终值，天然已经含着 append 的效果，没有"跳过 append 的数据"这回事，只是
// "不会为它多做一次行级增量"。
void ExecListFastMultiAppendSet() {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datacard2");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    cJSON* append = cJSON_AddObjectToObject(data, "append");
    cJSON_AddStringToObject(append, "key", "items");
    cJSON* aitem = cJSON_AddObjectToObject(append, "item");
    cJSON_AddStringToObject(aitem, "name", "会被set吞掉");
    cJSON_AddStringToObject(aitem, "price", "4");
    cJSON* set = cJSON_AddObjectToObject(data, "set");
    cJSON* arr = cJSON_AddArrayToObject(set, "items");
    cJSON* sitem = cJSON_CreateObject();
    cJSON_AddStringToObject(sitem, "name", "set最终态");
    cJSON_AddStringToObject(sitem, "price", "5");
    cJSON_AddItemToArray(arr, sitem);
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] listfastmulti append+set -> %s (%s)\n", res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（Phase2 T8）：渲染 bind_data label 演示卡（固定 card id "datalabel"）。
void RenderDataLabelCard() {
    cJSON* args = cJSON_Parse(kCardDataLabel);
    if (!args) {
        std::fprintf(stderr, "[sim] datalabel JSON parse failed\n");
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] datalabel render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（Phase2 T4）：走真实 ui_update 的 data 通道——dataop <append|remove>，
// 对准 "datacard" 卡的 items 数组。append 加一条，remove 删第 0 条。
void ExecDataOp(const std::string& op) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datacard");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    if (op == "append") {
        cJSON* append = cJSON_AddObjectToObject(data, "append");
        cJSON_AddStringToObject(append, "key", "items");
        cJSON* item = cJSON_AddObjectToObject(append, "item");
        cJSON_AddStringToObject(item, "name", "新品");
        cJSON_AddStringToObject(item, "price", "1");
    } else if (op == "remove") {
        cJSON* remove = cJSON_AddObjectToObject(data, "remove");
        cJSON_AddStringToObject(remove, "key", "items");
        cJSON_AddNumberToObject(remove, "index", 0);
    } else {
        std::fprintf(stderr, "[sim] dataop 未知操作 '%s'（用 append|remove）\n", op.c_str());
        cJSON_Delete(args);
        return;
    }
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] dataop %s -> %s (%s)\n", op.c_str(), res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// Commit 3 G4：list 行模板 = grid（固定 id "gridlist"），验模板记账 + 重渲 GridDsc 无泄漏。
constexpr const char* kCardGridList =
    "{\"card\":\"gridlist\",\"display\":\"overlay\",\"data\":{\"rows\":["
    "{\"k\":\"甲\",\"v\":\"1\"},{\"k\":\"乙\",\"v\":\"2\"}]},"
    "\"root\":{\"type\":\"column\",\"gap\":12,\"children\":["
    "{\"type\":\"label\",\"role\":\"title\",\"text\":\"list-of-grid\"},"
    "{\"type\":\"list\",\"bind_data\":\"rows\",\"max\":8,\"item\":{\"type\":\"grid\",\"cols\":[1,1],"
    "\"children\":[{\"type\":\"label\",\"text\":\"{item.k}\"},"
    "{\"type\":\"label\",\"role\":\"value\",\"text\":\"{item.v}\"}]}}]}}";

// 渲染 gridlist 后跑 N 轮 {append,remove} ui_update（每轮触发 list 重渲 = 删旧 grid 行 +
// 建新 grid 行）。GridDsc 净值应保持有界（≈当前行数），绝不随轮次单调增长——[griddsc]
// 日志给出 alloc/free 净值证据。
void ExecG4Leak(int rounds) {
    cJSON* rc = cJSON_Parse(kCardGridList);
    bool is_err = false;
    char* r0 = pi_card_tool_render(rc, &is_err);
    std::fprintf(stderr, "[sim] g4leak render -> %s (%s)\n", r0 ? r0 : "(null)", is_err ? "ERR" : "ok");
    free(r0);
    cJSON_Delete(rc);
    for (int i = 0; i < rounds; i++) {
        for (const char* op : {"append", "remove"}) {
            cJSON* args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "card", "gridlist");
            cJSON* data = cJSON_AddObjectToObject(args, "data");
            if (std::strcmp(op, "append") == 0) {
                cJSON* ap = cJSON_AddObjectToObject(data, "append");
                cJSON_AddStringToObject(ap, "key", "rows");
                cJSON* item = cJSON_AddObjectToObject(ap, "item");
                cJSON_AddStringToObject(item, "k", "丙");
                cJSON_AddStringToObject(item, "v", "9");
            } else {
                cJSON* rm = cJSON_AddObjectToObject(data, "remove");
                cJSON_AddStringToObject(rm, "key", "rows");
                cJSON_AddNumberToObject(rm, "index", 0);
            }
            bool e = false;
            char* res = pi_card_tool_update(args, &e);
            free(res);
            cJSON_Delete(args);
        }
    }
    std::fprintf(stderr, "[sim] g4leak done: %d rounds of {append,remove}\n", rounds);
}

// TEMP SCAFFOLD（Phase2 T8）：走真实 ui_update 的 data.set，改 "datalabel" 卡的 status 值。
void ExecDataLabelSet(const std::string& status) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "card", "datalabel");
    cJSON* data = cJSON_AddObjectToObject(args, "data");
    cJSON* set = cJSON_AddObjectToObject(data, "set");
    cJSON_AddStringToObject(set, "status", status.c_str());
    bool is_err = false;
    char* res = pi_card_tool_update(args, &is_err);
    std::fprintf(stderr, "[sim] datalabelset '%s' -> %s (%s)\n", status.c_str(), res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

// TEMP SCAFFOLD（Phase2 T5）：preset <confirm|form|dashboard|menu> —— 走真实 ui_render 的
// preset+slots 展开路径。四种各给一份最小可行 slots；传参非法名走负向分支验证具名错误串。
void RenderPreset(const std::string& name) {
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "display", "overlay");
    cJSON_AddStringToObject(args, "preset", name.c_str());
    cJSON* slots = cJSON_AddObjectToObject(args, "slots");
    if (name == "confirm") {
        cJSON_AddStringToObject(slots, "title", "清空历史?");
        cJSON_AddStringToObject(slots, "body", "该操作不可撤销。");
        cJSON* confirm = cJSON_AddObjectToObject(slots, "confirm");
        cJSON_AddStringToObject(confirm, "text", "清空");
        cJSON_AddStringToObject(confirm, "report", "确认清空历史");
    } else if (name == "form") {
        cJSON_AddStringToObject(slots, "title", "下单");
        cJSON* fields = cJSON_AddArrayToObject(slots, "fields");
        cJSON* qty = cJSON_CreateObject();
        cJSON_AddStringToObject(qty, "type", "slider");
        cJSON_AddStringToObject(qty, "id", "qty");
        cJSON_AddStringToObject(qty, "label", "数量");
        cJSON_AddNumberToObject(qty, "min", 1);
        cJSON_AddNumberToObject(qty, "max", 10);
        cJSON_AddNumberToObject(qty, "value", 3);
        cJSON_AddItemToArray(fields, qty);
        cJSON* urgent = cJSON_CreateObject();
        cJSON_AddStringToObject(urgent, "type", "switch");
        cJSON_AddStringToObject(urgent, "id", "urgent");
        cJSON_AddStringToObject(urgent, "label", "加急");
        cJSON_AddItemToArray(fields, urgent);
    } else if (name == "dashboard") {
        cJSON_AddStringToObject(slots, "title", "设备状态");
        cJSON* metrics = cJSON_AddArrayToObject(slots, "metrics");
        cJSON* m1 = cJSON_CreateObject();
        cJSON_AddStringToObject(m1, "label", "音量");
        cJSON_AddStringToObject(m1, "bind", "audio.volume");
        cJSON_AddStringToObject(m1, "kind", "bar");
        cJSON_AddStringToObject(m1, "fmt", "%d%%");
        cJSON_AddStringToObject(m1, "icon", "volume");
        cJSON_AddItemToArray(metrics, m1);
        cJSON* m2 = cJSON_CreateObject();
        cJSON_AddStringToObject(m2, "label", "电量");
        cJSON_AddStringToObject(m2, "bind", "battery.level");
        cJSON_AddStringToObject(m2, "fmt", "%d%%");
        cJSON_AddItemToArray(metrics, m2);
    } else if (name == "menu") {
        cJSON_AddStringToObject(slots, "title", "选择操作");
        cJSON* items = cJSON_AddArrayToObject(slots, "items");
        cJSON* i1 = cJSON_CreateObject();
        cJSON_AddStringToObject(i1, "text", "新建对话");
        cJSON_AddItemToArray(items, i1);
        cJSON* i2 = cJSON_CreateObject();
        cJSON_AddStringToObject(i2, "text", "导出记录");
        cJSON_AddItemToArray(items, i2);
    }
    // 负向：未知 preset 名（如 "presetbad"）故意不填 slots 必需字段，走 ExpandPreset 的具名错误串。
    bool is_err = false;
    char* res = pi_card_tool_render(args, &is_err);
    std::fprintf(stderr, "[sim] preset %s -> %s (%s)\n", name.c_str(), res ? res : "(null)",
                 is_err ? "ERROR" : "ok");
    free(res);
    cJSON_Delete(args);
}

uint32_t TickCb() { return SDL_GetTicks(); }

// Runs inside SDL_PumpEvents — i.e. on the LVGL thread, under the LVGL lock
// (the SDL driver pumps from an lv_timer). No LVGL calls here; just record.
int EventWatch(void*, SDL_Event* ev) {
    switch (ev->type) {
        case SDL_QUIT:
            g_quit = true;
            break;
        case SDL_KEYDOWN:
            if (ev->key.keysym.sym == SDLK_F1) {
                g_pwr_key_held = true;  // PWR_KEY 按下（按住说话）
            } else if (ev->key.keysym.sym == SDLK_F12) {
                g_shot_pending = true;
            } else if (ev->key.keysym.sym == SDLK_F9) {
                g_demo_pending = true;  // pi_card 演示卡
            } else if (sim_asr_session_active()) {
                if (ev->key.keysym.sym == SDLK_BACKSPACE) sim_asr_backspace();
            }
            break;
        case SDL_KEYUP:
            if (ev->key.keysym.sym == SDLK_F1) g_pwr_key_held = false;  // 松开发送
            break;
        case SDL_TEXTINPUT:
            if (sim_asr_session_active()) sim_asr_type(ev->text.text);
            break;
        default:
            break;
    }
    return 0;
}

void Put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

// 32bpp top-down BMP; pixel data is LVGL XRGB8888 (B,G,R,X little-endian),
// which is exactly BMP's BGRX byte order.
bool WriteBmp32(const char* path, const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride) {
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const uint32_t row_bytes = w * 4;
    const uint32_t img_bytes = row_bytes * h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    Put32(hdr + 2, 54 + img_bytes);
    Put32(hdr + 10, 54);
    Put32(hdr + 14, 40);
    Put32(hdr + 18, w);
    Put32(hdr + 22, static_cast<uint32_t>(-static_cast<int32_t>(h)));
    hdr[26] = 1;
    hdr[28] = 32;
    Put32(hdr + 34, img_bytes);
    bool ok = std::fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    for (uint32_t y = 0; ok && y < h; y++) {
        ok = std::fwrite(data + static_cast<size_t>(y) * stride, 1, row_bytes, f) == row_bytes;
    }
    std::fclose(f);
    return ok;
}

const char* ShotPath() {
    const char* p = std::getenv("PI_SIM_SHOT");
    return (p != nullptr && p[0] != '\0') ? p : "pi_sim_shot.bmp";
}

void TakeScreenshot(const char* path) {
    lv_lock();
    lv_draw_buf_t* buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_XRGB8888);
    if (buf != nullptr) {
        if (WriteBmp32(path, buf->data, buf->header.w, buf->header.h, buf->header.stride)) {
            std::fprintf(stderr, "[sim] screenshot -> %s\n", path);
        } else {
            std::fprintf(stderr, "[sim] screenshot write failed: %s\n", path);
        }
        lv_draw_buf_destroy(buf);
    } else {
        std::fprintf(stderr, "[sim] lv_snapshot_take failed\n");
    }
    lv_unlock();
}

// ---- virtual touch: a second pointer indev, driven by PI_SIM_CMDFILE ----
// Equivalent of the GT911: lets tests press/hold/move/release at exact pixels.

struct VirtTouch {
    bool pressed = false;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t release_at = 0;  // 0 = hold until an explicit release
};
VirtTouch g_touch;
std::mutex g_touch_mu;

// Smooth fling: interpolate the virtual touch (x0,y0)->(x1,y1) over dur_ms on the
// sim's own clock, emitting an intermediate point every Pump iteration. Coarse
// cmdfile `move` steps (>=100ms apart) can't reproduce a continuous fast swipe, so
// they race screen gestures against short press-and-hold timers (hold-to-talk).
// This delivers real fling-like motion for deterministic gesture tests.
struct Swipe {
    bool active = false;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t start_ms = 0, dur_ms = 0;
};
Swipe g_swipe;

void VirtTouchRead(lv_indev_t*, lv_indev_data_t* data) {
    static bool last_reported = false;
    static uint32_t reads = 0, last_ms = 0;
    if (std::getenv("PI_SIM_TOUCH_DEBUG") != nullptr) {
        uint32_t now = SDL_GetTicks();
        if (++reads % 16 == 1) {
            std::fprintf(stderr, "[sim][vtouch] read#%u dt=%ums\n", reads, now - last_ms);
        }
        last_ms = now;
    }
    std::lock_guard<std::mutex> lk(g_touch_mu);
    if (g_touch.pressed && g_touch.release_at != 0 && SDL_GetTicks() >= g_touch.release_at) {
        g_touch.pressed = false;
    }
    data->point.x = g_touch.x;
    data->point.y = g_touch.y;
    data->state = g_touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (g_touch.pressed != last_reported) {
        last_reported = g_touch.pressed;
        std::fprintf(stderr, "[sim][vtouch] %s (%d,%d)\n", g_touch.pressed ? "press" : "release",
                     (int)g_touch.x, (int)g_touch.y);
    }
}

// Dump the current overlay card's root ("tree" in pi_card_host.cc) geometry —
// useful whenever an overlay-height bug is suspected (the scroll-shrink bug this
// was built for is fixed by pi_card::ReflowOverlay, but the numbers are handy for
// any future height/scroll regression). Tree walk mirrors BuildOverlay():
// screen_active's last child = scrim, scrim[0] = wrap, wrap[0] = tree.
void DumpCardGeom(const char* tag) {
    lv_lock();
    lv_obj_t* scr = lv_screen_active();
    uint32_t n = lv_obj_get_child_count(scr);
    lv_obj_t* scrim = (n > 0) ? lv_obj_get_child(scr, n - 1) : nullptr;
    lv_obj_t* wrap = (scrim && lv_obj_get_child_count(scrim) > 0) ? lv_obj_get_child(scrim, 0) : nullptr;
    lv_obj_t* tree = (wrap && lv_obj_get_child_count(wrap) > 0) ? lv_obj_get_child(wrap, 0) : nullptr;
    if (tree != nullptr) {
        std::fprintf(stderr,
                     "[sim][cardh] %-10s tree_h=%4d tree_w=%4d content_h=%4d scroll_y=%4d "
                     "scroll_top=%4d scroll_bottom=%4d wrap_h=%4d\n",
                     tag, (int)lv_obj_get_height(tree), (int)lv_obj_get_width(tree),
                     (int)lv_obj_get_content_height(tree), (int)lv_obj_get_scroll_y(tree),
                     (int)lv_obj_get_scroll_top(tree), (int)lv_obj_get_scroll_bottom(tree),
                     (int)lv_obj_get_height(wrap));
    } else {
        std::fprintf(stderr, "[sim][cardh] %-10s no overlay card found\n", tag);
    }
    lv_unlock();
}

// TEMP SCAFFOLD: 找到 chat 的消息流容器（pi_screen 的 s_feed 是 static，外部拿不到，故按
// 特征识别：全屏宽、竖向可滚的那个容器）。
lv_obj_t* FindFeed(lv_obj_t* parent) {
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* c = lv_obj_get_child(parent, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_dir(c) == LV_DIR_VER &&
            lv_obj_get_width(c) >= 700) {
            return c;
        }
        if (lv_obj_t* r = FindFeed(c)) return r;
    }
    return nullptr;
}

// TEMP SCAFFOLD: 打印消息流的滚动位置——验证「贴底跟随」时看 scroll_y 有没有被抢走。
void DumpFeedGeom(const char* tag) {
    lv_lock();
    lv_obj_t* feed = FindFeed(lv_screen_active());
    if (feed != nullptr) {
        std::fprintf(stderr, "[sim][feedh] %-12s scroll_y=%5d scroll_top=%5d scroll_bottom=%5d\n", tag,
                     (int)lv_obj_get_scroll_y(feed), (int)lv_obj_get_scroll_top(feed),
                     (int)lv_obj_get_scroll_bottom(feed));
    } else {
        std::fprintf(stderr, "[sim][feedh] %-12s feed 未找到\n", tag);
    }
    lv_unlock();
}

// TEMP SCAFFOLD: 按按钮上的文本找到该 button（递归找 label，再往上找最近的 button 祖先）。
// 祖先判定放宽成"任意 CLICKABLE 对象"而非严格 lv_button_class——sbar 的 mode_btn（ZEN/FLOW
// 切换）是 lv_obj_create + 手动挂 CLICKABLE，不是真正的 lv_button，原判定找不到它。
lv_obj_t* FindBtnByLabel(lv_obj_t* parent, const char* text) {
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* c = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(c, &lv_label_class)) {
            const char* t = lv_label_get_text(c);
            if (t != nullptr && std::strcmp(t, text) == 0) {
                for (lv_obj_t* p = lv_obj_get_parent(c); p != nullptr; p = lv_obj_get_parent(p)) {
                    if (lv_obj_check_type(p, &lv_button_class) || lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE))
                        return p;
                }
            }
        }
        if (lv_obj_t* r = FindBtnByLabel(c, text)) return r;
    }
    return nullptr;
}

// TEMP SCAFFOLD: 点一个按钮——查出它的屏幕坐标后走**真实触摸**（同 click 命令的通路），
// 而不是直接 lv_obj_send_event 伪造事件，这样 action 分发链路是真的被走了一遍。
void ClickBtnByLabel(const char* text) {
    lv_lock();
    lv_obj_t* btn = FindBtnByLabel(lv_screen_active(), text);
    int cx = -1, cy = -1;
    bool hidden = false;
    if (btn != nullptr) {
        lv_area_t a;
        lv_obj_get_coords(btn, &a);
        cx = (int)(a.x1 + a.x2) / 2;
        cy = (int)(a.y1 + a.y2) / 2;
        hidden = lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_unlock();
    if (cx < 0) {
        std::fprintf(stderr, "[sim][clickbtn] '%s' 未找到\n", text);
        return;
    }
    std::fprintf(stderr, "[sim][clickbtn] '%s' @(%d,%d)%s\n", text, cx, cy, hidden ? " [HIDDEN!]" : "");
    std::lock_guard<std::mutex> lk(g_touch_mu);
    g_touch.pressed = true;
    g_touch.x = cx;
    g_touch.y = cy;
    g_touch.release_at = SDL_GetTicks() + 250;
}

// TEMP SCAFFOLD: 打印 form 卡里各控件的可交互状态——验证「无 bind 只有 id」的纯本地表单
// 控件没被死控件兜底 DIM 掉（pi_card_render.cc 的 live 判据）。走 LVGL 树按类型找。
void DumpWidgetLive(lv_obj_t* parent, const char* tag) {
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* c = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(c, &lv_slider_class) || lv_obj_check_type(c, &lv_switch_class)) {
            std::fprintf(stderr, "[sim][live] %-8s %-7s clickable=%d opa=%d\n", tag,
                         lv_obj_check_type(c, &lv_slider_class) ? "slider" : "switch",
                         lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) ? 1 : 0,
                         (int)lv_obj_get_style_opa(c, LV_PART_MAIN));
        }
        DumpWidgetLive(c, tag);
    }
}

// One command per line: keydown | keyup | type <text> | backspace |
// click <x> <y> | press <x> <y> | move <x> <y> | release | shot <path> | quit
// (PWR_KEY is press-and-hold: keydown ... type ... keyup to record+send.)
void ExecCmd(const std::string& line) {
    std::fprintf(stderr, "[sim][cmd] %s\n", line.c_str());
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd == "keydown") {
        g_pwr_key_held = true;
    } else if (cmd == "keyup") {
        g_pwr_key_held = false;
    } else if (cmd == "backspace") {
        sim_asr_backspace();
    } else if (cmd == "type") {
        std::string rest;
        std::getline(ss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        sim_asr_type(rest.c_str());
    } else if (cmd == "click" || cmd == "press") {
        int x = 0, y = 0;
        ss >> x >> y;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.pressed = true;
        g_touch.x = x;
        g_touch.y = y;
        g_touch.release_at = (cmd == "click") ? SDL_GetTicks() + 250 : 0;
    } else if (cmd == "move") {
        int x = 0, y = 0;
        ss >> x >> y;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.x = x;
        g_touch.y = y;
    } else if (cmd == "release") {
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.pressed = false;
        g_touch.release_at = 0;
    } else if (cmd == "swipe") {  // swipe <x0> <y0> <x1> <y1> [dur_ms=180]
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0, ms = 180;
        ss >> x0 >> y0 >> x1 >> y1 >> ms;
        g_swipe.x0 = x0; g_swipe.y0 = y0; g_swipe.x1 = x1; g_swipe.y1 = y1;
        g_swipe.dur_ms = (ms > 0) ? (uint32_t)ms : 180;
        g_swipe.start_ms = SDL_GetTicks();
        g_swipe.active = true;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        g_touch.pressed = true;
        g_touch.x = x0;
        g_touch.y = y0;
        g_touch.release_at = 0;
    } else if (cmd == "mediadump") {  // Stage B: 打印 MediaController 快照，观察 invoke/set 联动
        auto& mc = media::MediaController::Instance();
        const char* st = "stopped";
        switch (mc.state()) {
            case media::MediaState::Loading: st = "loading"; break;
            case media::MediaState::Playing: st = "playing"; break;
            case media::MediaState::Paused: st = "paused"; break;
            case media::MediaState::Error: st = "error"; break;
            default: break;
        }
        std::fprintf(stderr, "[sim][mediadump] state=%s index=%d pos=%ds title=%s\n", st, mc.index(),
                     mc.position_s(), mc.current().title.c_str());
    } else if (cmd == "shot") {  // Stage B: shot <path> — 立即截图到指定路径
        std::string p;
        std::getline(ss, p);
        if (!p.empty() && p[0] == ' ') p.erase(0, 1);
        TakeScreenshot(p.empty() ? ShotPath() : p.c_str());
    } else if (cmd == "growcard") {  // TEMP SCAFFOLD: render the reflow re-entrancy test card
        RenderGrowCard();
    } else if (cmd == "showrows") {  // TEMP SCAFFOLD: showrows <n> — real ui_update, drives reflow
        int n = 0;
        ss >> n;
        ShowRows(n);
    } else if (cmd == "patchtest") {  // TEMP SCAFFOLD: real ui_update batch `patches` path
        ExecPatchTest();
    } else if (cmd == "numanimcard") {  // TEMP SCAFFOLD: render the num-anim acceptance card
        RenderNumAnimCard();
    } else if (cmd == "numset") {  // TEMP SCAFFOLD: numset <path> <value> — direct subject write
        std::string path;
        int v = 0;
        ss >> path >> v;
        ExecNumSet(path, v);
    } else if (cmd == "closecard") {  // TEMP SCAFFOLD: closecard <id> — real ui_close
        std::string id;
        ss >> id;
        ExecCloseCard(id);
    } else if (cmd == "previewfeed") {  // TEMP SCAFFOLD: previewfeed <file> — feed partial-JSON frames
        std::string path;
        std::getline(ss, path);
        if (!path.empty() && path[0] == ' ') path.erase(0, 1);
        ExecPreviewFeed(path);
    } else if (cmd == "previewdump") {  // TEMP SCAFFOLD (verify): recursively dump preview tree
        ExecPreviewDump();
    } else if (cmd == "previewend") {  // TEMP SCAFFOLD: simulate UI_TOOL_END without a card render
        ExecPreviewEnd();
    } else if (cmd == "bargein") {  // TEMP SCAFFOLD: simulate a barge-in/new-session gen bump
        ExecBargeIn();
    } else if (cmd == "rendercard") {  // TEMP SCAFFOLD: rendercard <file> — real ui_render from raw JSON file
        std::string path;
        std::getline(ss, path);
        if (!path.empty() && path[0] == ' ') path.erase(0, 1);
        ExecRenderJson(path);
    } else if (cmd == "strset") {  // TEMP SCAFFOLD: strset <path> <text> — direct String subject write
        std::string path, text;
        ss >> path;
        std::getline(ss, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        ExecStrSet(path, text);
    } else if (cmd == "chatcard") {  // TEMP SCAFFOLD: chatcard <n> — append card #n to the chat feed
        int n = 1;
        ss >> n;
        RenderChatCard(n);
    } else if (cmd == "feedh") {  // TEMP SCAFFOLD: feedh <tag> — dump the chat feed's scroll position
        std::string tag;
        ss >> tag;
        DumpFeedGeom(tag.empty() ? "-" : tag.c_str());
    } else if (cmd == "iconcard") {  // TEMP SCAFFOLD: 按钮 icon 支持 + 负例 hints 验收卡
        RenderIconButtonCard();
    } else if (cmd == "formcard") {  // TEMP SCAFFOLD: render the report-snapshot / toggle test card
        RenderFormCard();
    } else if (cmd == "badcards") {  // TEMP SCAFFOLD: validator negative cases (bad/missing target)
        RenderBadCard(kBadToggleCard, "坏 target");
        RenderBadCard(kNoTargetCard, "缺 target");
        std::string long_qr = BuildLongQrCard();
        RenderBadCard(long_qr.c_str(), "qrcode 超长");
        RenderBadCard(kCardChoiceBad, "choice 单项");
        RenderBadCard(kCardSliderStringBind, "slider绑string");
        // Phase2 T2/T3：list 节点预算超限 / 行内非法动作，均应在 worker 侧同步被拒。
        RenderBadCard(kCardListOverflow, "list超预算");
        RenderBadCard(kCardListBadAction, "list行内toggle");
        RenderBadCard(kCardListNested, "list嵌套list");  // verifier fix2 负例
        // Phase3：chart bind_history 非历史路径 / invoke 未注册命令，均应同步被拒（D12/D16）。
        RenderBadCard(kCardChartBadHistory, "chart绑非history路径");
        RenderBadCard(kCardInvokeBad, "invoke未注册cmd");
        // Commit 3 E1：grid 结构性负例（cols 缺失/空/7列/元素-1/span越界/撑爆64节点）全应拒；
        // column justify 未知值走回落正例（不拒 + hints 含回落提示）。
        RenderBadCard(kGridNoCols, "grid缺cols");
        RenderBadCard(kGridEmptyCols, "grid空cols");
        RenderBadCard(kGridSevenCols, "grid7列");
        RenderBadCard(kGridBadElem, "grid元素-1");
        RenderBadCard(kGridSpanOver, "grid span越界");
        RenderBadCard(BuildGridOverflow().c_str(), "grid撑爆64");
        RenderExpectOkWithHint(kGridJustifyFallback, "justify middle",
                               "is not a recognized value");
    } else if (cmd == "p1grid") {  // Commit 3 P1: standby grid pin persists → RehydratePin re-renders
        // 直写一张 standby grid 卡的 pin 封套到 NVS（模拟上次会话已持久化），再调
        // RehydratePin（模拟重启回灌）：grid 应 Validate 通过并重渲，不被 discard/erase。
        const char* env =
            "{\"v\":1,\"root\":{\"type\":\"grid\",\"cols\":[1,1],\"gap\":6,\"children\":["
            "{\"type\":\"label\",\"role\":\"section\",\"text\":\"CPU\"},"
            "{\"type\":\"label\",\"role\":\"value\",\"text\":\"42%\"},"
            "{\"type\":\"label\",\"role\":\"section\",\"text\":\"MEM\"},"
            "{\"type\":\"label\",\"role\":\"value\",\"text\":\"61%\"}]}}";
        Settings("ui", true).SetString("pin", env);
        std::fprintf(stderr, "[sim] p1grid: wrote standby-grid pin envelope to NVS\n");
        pi_card::RehydratePin();  // 读 NVS + Validate + OnRenderEvent（drain 下一拍渲染）
        Settings ui_ro("ui", false);
        const bool kept = !ui_ro.GetString("pin", "").empty();
        std::fprintf(stderr, "[sim] p1grid: after RehydratePin pin-in-NVS=%s (kept=Validate通过未被erase)\n",
                     kept ? "true ✓" : "false ✗");
    } else if (cmd == "g4leak") {  // Commit 3 G4: list-of-grid, N rounds ui_update, GridDsc leak check
        int n = 20;
        std::string rest;
        std::getline(ss, rest);
        if (!rest.empty()) n = std::atoi(rest.c_str());
        ExecG4Leak(n);
    } else if (cmd == "standby") {  // Phase3: render the standby pin-widget demo card
        RenderStandbyCard();
    } else if (cmd == "standbybig") {  // Phase3 D8: oversized standby envelope (>3072B) rejection
        RenderStandbyOversized();
    } else if (cmd == "unpincard") {  // Phase3: exercise pi_card::UnpinCard() directly (EraseKey+delete)
        pi_card::UnpinCard();
        std::fprintf(stderr, "[sim] unpincard: HasPin()=%s\n", pi_card::HasPin() ? "true" : "false");
    } else if (cmd == "clickbtn") {  // TEMP SCAFFOLD: clickbtn <text> — real touch on a button
        std::string rest;
        std::getline(ss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        ClickBtnByLabel(rest.c_str());
    } else if (cmd == "live") {  // TEMP SCAFFOLD: live <tag> — dump slider/switch interactivity
        std::string tag;
        ss >> tag;
        lv_lock();
        DumpWidgetLive(lv_screen_active(), tag.empty() ? "-" : tag.c_str());
        lv_unlock();
    } else if (cmd == "cardh") {  // cardh <tag> — dump the current overlay's height/scroll geometry
        std::string tag;
        ss >> tag;
        DumpCardGeom(tag.empty() ? "-" : tag.c_str());
    } else if (cmd == "hintcard") {  // TEMP SCAFFOLD（B 验收 §4 断言 7）：触发多条 Lint 规则
        RenderBadCard(kCardHints, "hints");
    } else if (cmd == "choicebind") {  // TEMP SCAFFOLD（B 验收断言 18 可写 bind 半支）
        RenderBadCard(kCardChoiceBind, "choicebind");
    } else if (cmd == "desc") {  // TEMP SCAFFOLD（B 验收 §4 断言 8）：打印动态 ui_render 描述
        std::fprintf(stderr, "[sim][desc] %s\n", pi_card_render_desc());
    } else if (cmd == "datacard") {  // Phase2 T1/T4: render kCardList (fixed card id "datacard")
        RenderDataCard();
    } else if (cmd == "dataop") {  // Phase2 T4: dataop <append|remove> — real ui_update data ops
        std::string op;
        ss >> op;
        ExecDataOp(op);
    } else if (cmd == "datacard2") {  // 改造4: render kCardListFast (fixed card id "datacard2")
        RenderDataCardFast();
    } else if (cmd == "listfastop") {  // 改造4: listfastop <append|remove|replace|set> [index]
        std::string op;
        int idx = 0;
        ss >> op >> idx;
        ExecListFastOp(op, idx);
    } else if (cmd == "listfastmulti_ar") {  // 改造4: 一次 ui_update 里 append+replace 合并
        ExecListFastMultiAppendReplace();
    } else if (cmd == "listfastmulti_as") {  // 改造4: 一次 ui_update 里 append+set 合并
        ExecListFastMultiAppendSet();
    } else if (cmd == "datalabel") {  // Phase2 T8: render kCardDataLabel (fixed card id "datalabel")
        RenderDataLabelCard();
    } else if (cmd == "datalabelset") {  // Phase2 T8: datalabelset <text> — real ui_update data.set
        std::string val;
        std::getline(ss, val);
        if (!val.empty() && val[0] == ' ') val.erase(0, 1);
        ExecDataLabelSet(val);
    } else if (cmd == "choicelabelcard") {  // Phase2 T7: choice + report "{label}" token
        RenderBadCard(kCardChoiceLabel, "choicelabel");
    } else if (cmd == "preset") {  // Phase2 T5: preset <confirm|form|dashboard|menu>
        std::string name;
        ss >> name;
        RenderPreset(name);
    } else if (cmd == "presetbad") {  // Phase2 T5 负向：form 缺 fields，应报具名错误串
        cJSON* args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "preset", "form");
        cJSON_AddObjectToObject(args, "slots");  // 空 slots：无 title/fields
        bool is_err = false;
        char* res = pi_card_tool_render(args, &is_err);
        std::fprintf(stderr, "[sim][negative] preset(form缺字段) -> %s (%s)\n", res ? res : "(null)",
                     is_err ? "已拒绝 ✓" : "竟然通过了 ✗");
        free(res);
        cJSON_Delete(args);
    } else if (cmd == "budget") {  // Phase2 T9: system prompt + ui_render desc 字节预算
        const char* sysp = pi_card_system_prompt();
        const char* desc = pi_card_render_desc();
        size_t sys_len = std::strlen(sysp);
        size_t desc_len = std::strlen(desc);
        std::fprintf(stderr, "[sim][budget] sys=%zu desc=%zu sum=%zu (limit 9216) %s\n", sys_len,
                     desc_len, sys_len + desc_len, (sys_len + desc_len <= 9216) ? "OK" : "OVER!");
    } else if (cmd == "sysprompt") {  // Phase2 T9: 打印完整 system prompt 供目视核对
        std::fprintf(stderr, "[sim][sysprompt] %s\n", pi_card_system_prompt());
    } else if (cmd == "stockcard") {  // 股票: stockcard <symbol> [name] — 注入 stock_chart chat 卡
        std::string sym, name;
        ss >> sym;
        std::getline(ss, name);
        if (!name.empty() && name[0] == ' ') name.erase(0, 1);
        cJSON* args = cJSON_CreateObject();
        cJSON* root = cJSON_AddObjectToObject(args, "root");
        cJSON_AddStringToObject(root, "type", "stock_chart");
        cJSON_AddStringToObject(root, "symbol", sym.c_str());
        if (!name.empty()) cJSON_AddStringToObject(root, "name", name.c_str());
        bool is_err = false;
        char* res = pi_card_tool_render(args, &is_err);
        std::fprintf(stderr, "[sim] stockcard render: %s (%s)\n", res ? res : "(null)", is_err ? "ERROR" : "ok");
        free(res);
        cJSON_Delete(args);
    } else if (cmd == "stockq") {  // 股票: stockq <查询词|symbol,...> — 直调 stock tool（阻塞 ≤6s/请求）
        std::string q;
        std::getline(ss, q);
        if (!q.empty() && q[0] == ' ') q.erase(0, 1);
        cJSON* args = cJSON_CreateObject();
        if (q.find(',') != std::string::npos || (q.size() > 2 && std::isdigit((unsigned char)q[2]))) {
            cJSON* arr = cJSON_AddArrayToObject(args, "symbols");
            std::stringstream qs(q);
            std::string sym;
            while (std::getline(qs, sym, ',')) {
                if (!sym.empty()) cJSON_AddItemToArray(arr, cJSON_CreateString(sym.c_str()));
            }
        } else {
            cJSON_AddStringToObject(args, "query", q.c_str());
        }
        bool is_err = false;
        char* res = pi_stock_tool_run(args, &is_err);
        std::fprintf(stderr, "[sim][stockq] %s -> %s\n", is_err ? "ERR" : "OK", res ? res : "(null)");
        free(res);
        cJSON_Delete(args);
    } else if (cmd == "shot") {
        std::string p;
        ss >> p;
        TakeScreenshot(p.empty() ? ShotPath() : p.c_str());
    } else if (cmd == "quit") {
        g_quit = true;
    }
}

void PollCmdFile(uint32_t now) {
    static const char* cmdfile = std::getenv("PI_SIM_CMDFILE");
    static uint32_t last_poll = 0;
    if (cmdfile == nullptr || cmdfile[0] == '\0' || now - last_poll < 100) return;
    last_poll = now;
    std::ifstream f(cmdfile);
    if (!f.good()) return;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    f.close();
    ::unlink(cmdfile);
    for (const auto& l : lines) ExecCmd(l);
}

uint32_t EnvMs(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr) ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0;
}

// Post-lv_timer_handler pump on the main thread (LVGL lock NOT held here).
void Pump() {
    static const char* demo_text = std::getenv("PI_SIM_AUTODEMO");
    static const uint32_t shot_ms = EnvMs("PI_SIM_SHOT_MS");
    static const uint32_t exit_ms = EnvMs("PI_SIM_EXIT_MS");
    static const uint32_t card_ms = EnvMs("PI_SIM_CARD_MS");  // 到点渲染 pi_card 演示卡
    static int demo_phase = 0;
    static bool shot_done = false;
    static bool card_done = false;
    const uint32_t now = SDL_GetTicks();

    if (card_ms > 0 && !card_done && now > card_ms) {
        card_done = true;
        RenderDemoCard();
    }

    // T1：到点做一次主题往返（深→浅→深）。几何走共享 lv_style_t（无色）故不受影响；
    // 往返后应与从未切换的深色截图逐像素一致——证 pi_theme::Set 只改色、不动几何。
    static const uint32_t theme_swap_ms = EnvMs("PI_SIM_THEME_SWAP_MS");
    static bool theme_swapped = false;
    if (theme_swap_ms > 0 && !theme_swapped && now > theme_swap_ms) {
        theme_swapped = true;
        lv_lock();
        pi_theme::Set(true);   // → 浅色
        pi_theme::Set(false);  // → 回深色（往返）
        lv_unlock();
    }

    // Stage A 媒体管线无头测试钩子：PI_SIM_MEDIA_FILE / PI_SIM_MEDIA_URL 启动即
    // StagePlaylist 单曲播放（WAV dump 由 PI_SIM_MEDIA_WAV 在 pump 内部激活）。
    static const char* media_file = std::getenv("PI_SIM_MEDIA_FILE");
    static const char* media_url = std::getenv("PI_SIM_MEDIA_URL");
    static bool media_started = false;
    if (!media_started && now > 800 && ((media_file && media_file[0]) || (media_url && media_url[0]))) {
        media_started = true;
        media::MediaItem item;
        if (media_url && media_url[0]) {
            item.title = "sim stream";
            item.path_or_url = media_url;
            item.is_stream = true;
        } else {
            item.title = "sim file";
            item.path_or_url = media_file;
            item.is_stream = false;
        }
        std::vector<media::MediaItem> pl{item};
        std::fprintf(stderr, "[sim] media start: %s (%s)\n", item.path_or_url.c_str(),
                     item.is_stream ? "stream" : "file");
        media::MediaController::Instance().StagePlaylist(std::move(pl), 0);
    }

    // Stage D 硬化验收钩子：PI_SIM_MEDIA_STOP_MS=<ms> 在该时刻调 Stop()（多半命中
    // reader 线程正阻塞在文件/网络 Read 里的中途），Stop() 内部已记录 teardown 耗时
    // （"Stop: teardown took Xms" 日志），本处只负责在正确时刻触发。
    static const uint32_t stop_ms = EnvMs("PI_SIM_MEDIA_STOP_MS");
    static bool stop_done = false;
    if (stop_ms > 0 && !stop_done && now > stop_ms) {
        stop_done = true;
        std::fprintf(stderr, "[sim] media stop triggered at wall=%ums\n", now);
        media::MediaController::Instance().Stop();
    }

    // Stage B media 工具无头直测：PI_SIM_MEDIA_TOOL = 一段 args JSON（如
    // {"mode":"search"}），启动后调 pi_media_tool_run 一次并打印返回 JSON。
    static const char* media_tool = std::getenv("PI_SIM_MEDIA_TOOL");
    static bool media_tool_done = false;
    if (!media_tool_done && media_tool && media_tool[0] && now > 900) {
        media_tool_done = true;
        cJSON* args = cJSON_Parse(media_tool);
        if (args) {
            bool is_err = false;
            char* res = pi_media_tool_run(args, &is_err);
            std::fprintf(stderr, "[sim] media tool (%s) -> %s\n", is_err ? "ERROR" : "ok",
                         res ? res : "(null)");
            free(res);
            cJSON_Delete(args);
        } else {
            std::fprintf(stderr, "[sim] media tool: bad args JSON\n");
        }
    }

    // Advance an in-flight smooth swipe (continuous motion, sim-clock paced).
    if (g_swipe.active) {
        uint32_t el = now - g_swipe.start_ms;
        std::lock_guard<std::mutex> lk(g_touch_mu);
        if (el >= g_swipe.dur_ms) {
            g_touch.x = g_swipe.x1;
            g_touch.y = g_swipe.y1;
            g_touch.pressed = false;  // fling ends in a release
            g_swipe.active = false;
        } else {
            float t = (float)el / (float)g_swipe.dur_ms;
            g_touch.x = g_swipe.x0 + (int32_t)((g_swipe.x1 - g_swipe.x0) * t);
            g_touch.y = g_swipe.y0 + (int32_t)((g_swipe.y1 - g_swipe.y0) * t);
            g_touch.pressed = true;
        }
    }

    PollCmdFile(now);

    // AUTODEMO: press-and-hold F1 (keydown), type while holding, release to send.
    if (demo_text != nullptr && demo_text[0] != '\0') {
        if (demo_phase == 0 && now > 1500) {
            g_pwr_key_held = true;  // 按住
            demo_phase = 1;
        } else if (demo_phase == 1 && now > 2600 && sim_asr_session_active()) {
            sim_asr_type(demo_text);
            demo_phase = 2;
        } else if (demo_phase == 2 && now > 3400) {
            g_pwr_key_held = false;  // 松开发送
            demo_phase = 3;
        }
    }

    // Mirror the physical PWR_KEY held state and run the edge state machines
    // (press / hold-to-talk / release) every iteration, under the LVGL lock.
    {
        lv_lock();
        IOExpander::getInstance().simSetPressed(IOExpander::Pin::PWR_KEY, g_pwr_key_held.load());
        IOExpander::getInstance().simPoll(now);
        lv_unlock();
    }
    if (g_demo_pending.exchange(false)) RenderDemoCard();  // 校验+入队；drain 下一拍渲染
    if (g_shot_pending.exchange(false)) TakeScreenshot(ShotPath());
    if (shot_ms > 0 && !shot_done && now > shot_ms) {
        shot_done = true;
        TakeScreenshot(ShotPath());
    }
    if (exit_ms > 0 && now > exit_ms) g_quit = true;
}

}  // namespace

int main() {
    signal(SIGCHLD, SIG_IGN); /* auto-reap the optional `say` children */

#ifdef __APPLE__
    /* Inherited background QoS (e.g. launched from a script/daemon) stretches a
     * 10ms SDL_Delay to ~95ms via timer coalescing — the whole UI drops to ~10fps
     * and sub-100ms touch gestures get lost. Pin the LVGL thread to interactive. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    std::fprintf(stderr,
                 "pi_sim — Metalio Claw pi_screen simulator (LVGL %d.%d SDL2)\n"
                 "  F1 按住   = PWR_KEY 按住说话(按住录音/松开发送; 轻点无反应; 生成中点按打断)\n"
                 "  打字      = 聆听时说话(退格删字)\n"
                 "  F9        = 渲染 pi_card 演示卡(overlay)\n"
                 "  F12       = 截图 BMP\n"
                 "  鼠标      = 触摸(状态栏下拉 = 快捷面板)\n",
                 LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);

    lv_init();
    lv_tick_set_cb(TickCb);

    lv_display_t* disp = lv_sdl_window_create(720, 720);
    lv_sdl_window_set_title(disp, "Metalio Claw — pi_screen sim");
    lv_sdl_mouse_create();
    SDL_AddEventWatch(EventWatch, nullptr);
    SDL_StartTextInput();

    lv_indev_t* vtouch = lv_indev_create();
    lv_indev_set_type(vtouch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(vtouch, VirtTouchRead);

    if (std::getenv("PI_SIM_TOUCH_DEBUG") != nullptr) {
        lv_timer_create(
            [](lv_timer_t*) {
                static uint32_t n = 0, last = 0;
                if (++n % 32 == 1) {
                    uint32_t now = SDL_GetTicks();
                    std::fprintf(stderr, "[sim][t33] fire#%u dt=%ums lv_tick=%u sdl=%u\n", n,
                                 now - last, lv_tick_get(), now);
                    last = now;
                }
            },
            33, nullptr);
    }

    // main/main.cc boot chain, hardware-free part, verbatim semantics.
    lv_lock();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* pi = PiScreen::Create();
    screen_attach_lifecycle(pi, [](screen_lifecycle_event_t e) { PiScreen::LifecycleCallback(e); });
    lv_screen_load(pi);
    if (old_scr != nullptr && old_scr != pi) lv_obj_delete(old_scr);
    lv_unlock();

    // PI_SIM_ADMIN=1：起 SD 音乐 Web 后台（POSIX socket 薄壳，http://127.0.0.1:8080），
    // 在 macOS 浏览器里真实走通整个前端；文件落到 pi_sim_sd/。
    if (std::getenv("PI_SIM_ADMIN") != nullptr) media_admin::httpd::Start();

    const bool loop_debug = std::getenv("PI_SIM_TOUCH_DEBUG") != nullptr;
    uint32_t stat_loops = 0, stat_handler = 0, stat_pump = 0, stat_last = SDL_GetTicks();
    while (!g_quit) {
        uint32_t t0 = SDL_GetTicks();
        uint32_t wait = lv_timer_handler();
        uint32_t t1 = SDL_GetTicks();
        Pump();
        uint32_t t2 = SDL_GetTicks();
        if (loop_debug) {
            stat_loops++;
            stat_handler += t1 - t0;
            stat_pump += t2 - t1;
            if (t2 - stat_last >= 2000) {
                std::fprintf(stderr, "[sim][loop] %.1f loops/s, handler avg %.1fms, pump avg %.1fms\n",
                             stat_loops * 1000.0 / (t2 - stat_last), (double)stat_handler / stat_loops,
                             (double)stat_pump / stat_loops);
                stat_loops = stat_handler = stat_pump = 0;
                stat_last = t2;
            }
        }
        if (wait == LV_NO_TIMER_READY || wait > 10) wait = 10;
        SDL_Delay(wait);
    }
    return 0;
}
