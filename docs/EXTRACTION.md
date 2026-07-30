# metalio_hal 剥离设计 — pi 专属固件 + 可复用硬件库

目标终态：
1. 唯一 UI = `main/display/screen/pi_screen/`，开机硬件/网络初始化后直进 pi 屏（无 Boot/Home/其他 screen）。
2. 全部硬件能力剥离进本地组件 `components/metalio_hal/`，暴露干净 C++ 门面（namespace `mhal`），lib 内零 screen/业务引用。
3. xiaozhi 云端业务（Application/protocols/ota/mcp/AudioService 会话层）删除，git 历史保底。

基线说明：任务书基线为 e1556dc，但开工时仓库已稳定在 f5bc34d（wp2-v2：已删 23 个
home-menu UI app、CMakeLists 相应裁剪、SetupUI 直跳 pi）。f5bc34d 的删除集是本方案
终态的真子集，故直接在其上继续，不回退。

---

## 1. 组件布局

```
components/metalio_hal/
  CMakeLists.txt
  idf_component.yml            # lib 自带 managed 依赖（见 §4）
  include/                     # 公共门面（对外 API，全部可被 main 引用）
    metalio_hal/
      hal.h                    # mhal::Init() 一站式硬件初始化
      display.h                # 面板+触摸+LVGL adapter 起显；Lock/Unlock
      backlight.h              # 亮度设置/读取/恢复（NVS 持久化）
      network.h                # Wi-Fi + 4G 双网启动/切换/事件
      bluetooth.h              # BT 音频模组控制（AT 指令、模式、扫描、连接）
      audio.h                  # mic/speaker PCM 读写、音量（未来 ASR 入口）
      power.h                  # 电池电量/电压/电流/充电态、强制关机
      sysmon.h                 # CPU/内存/电池周期监控日志
    IOExpander.hpp             # TCA9555 门面（pi_screen 直接用，路径保持不变）
    settings.h                 # NVS 通用包装（pi_screen 直接用，路径保持不变）
    audio_codec.h              # AudioCodec 基类（mhal::audio::Codec() 返回类型）
  src/                         # 私有实现（对外不可见）
    config.h                   # 原 boards/metalio-claw-4/config.h（引脚表，保留原名）
    hal.cc                     # Init 序列（原 METALIO_CLAW_4 构造函数）+ I2C 总线
    hal_internal.h             # lib 内部跨模块接口
    display/ (display_hal.cc, touch_feed.{cc,h}, esp_lcd_nv3051f.{c,h}, esp_lcd_fl7707n.{c,h})
    backlight.{cc,h} + backlight_facade.cc   # PwmBacklight 内化
    net/ (network.cc[=旧 dual_network_board+wifi_board], nt26_modem.{cc,h}, uart_eth_modem.{cc,h})
    bt/ (bt_module.cc, SimpleUart.hpp)
    audio/ (audio_codec.cc, bt_audio_codec.{cc,h}, audio_facade.cc)
    power/ (power.cc[含 Wxcho 无线充+开机电量保护], bq27220_gauge.{cc,h}, i2c_device.{cc,h})
    sysmon.cc                  # 监控任务（原板级匿名 lambda）
    settings.cc
    SdCardManager.hpp
```

`main/` 终态：`main.cc`（新写）、`display/screen/pi_screen/*`、`display/screen/screen_util.{cc,h}`、`display/font/font_pi_*.c`、`CMakeLists.txt`、`idf_component.yml`（仅 UI 侧依赖）、`Kconfig.projbuild`（仅 pi 相关残留，语言/board/audio 选项删）。

## 2. 门面 API（as-built，与头文件一致；每能力附调用示例）

### 一站式初始化 + 调用示例（main.cc 即最小示例）
```cpp
#include "metalio_hal/hal.h"
mhal::Init();                       // 全部硬件就绪，LVGL 已在跑
// 可选项：mhal::Init({.mount_sd_card=false, .bt_default_mode=false,
//                     .battery_boot_guard=false, .restore_backlight=false});
```

### display.h — 屏幕操作
```cpp
lv_display_t* GetLvDisplay();  bool Lock(int timeout_ms=-1);  void Unlock();
int Width();  int Height();
// 示例：加载首屏
if (mhal::display::Lock()) { lv_screen_load(MyScreen()); mhal::display::Unlock(); }
```

### backlight.h — 亮度
```cpp
void SetBrightness(uint8_t percent, bool persist=false);
uint8_t GetBrightness();  void Restore();
// 示例：调到 40% 并记住
mhal::backlight::SetBrightness(40, /*persist=*/true);
```

### network.h — Wi-Fi/4G 双网
```cpp
enum class Type { WiFi=0, Cellular=1 };            // NVS "network"/"type"
enum class Event { WifiScanning, WifiConnecting, WifiConnected,
    WifiNoCredentials, WifiConnectFailed, WifiConfigPortal,
    ModemDetecting, CellularConnecting, CellularConnected,
    CellularDisconnected, CellularErrorNoSim, CellularErrorRegDenied,
    CellularErrorInitFailed, CellularErrorTimeout };
void OnEvent(std::function<void(Event, const std::string&)> cb);
bool Start();  void StartAsync();
Type GetType();  void SwitchType();               // 持久化另一类型并重启
bool IsConnected();
std::string GetWifiSsid();                         // WiFi 已连接时的 SSID；非 WiFi/未连接 ""
int GetWifiRssi();                                 // WiFi RSSI（dBm 负值）；非 WiFi/未连接 0
std::string GetIpAddress();                        // 当前 IP（WiFi 经 esp-wifi-connect 缓存，
                                                   //  4G 读 iot_eth netif）；未连接/未拿到 IP ""
void AddWifiCredential(const std::string& ssid, const std::string& pass);
void StartConfigPortal();                          // softAP 网页配网，阻塞
esp_err_t SendAtCommand(const std::string& cmd, std::string& resp,
                        uint32_t timeout_ms=5000, bool bypass_init_check=false);
int GetSignalStrength();                           // 4G CSQ；未就绪 -1
std::string GetRegistrationStateJson();            // AT+CEREG 状态
// 示例：订阅事件 + 后台起网
mhal::network::OnEvent([](auto e, const std::string& d){
    if (e == mhal::network::Event::WifiConnected) ESP_LOGI("app", "wifi up: %s", d.c_str());
});
mhal::network::StartAsync();
```

### bluetooth.h — BT 音频模组
```cpp
enum class Mode { None=0, Rx=1, Tx=2, MusicRx=3 };
enum class ConnState { Idle, Scanning, Connecting, Connected };
void SetCallbacks(Callbacks cbs);   // on_mode_changed/on_conn_state/on_device_found/on_status_text
void ApplyDefaultMode();  void SetMode(Mode);
void StartScan();  void Connect(const std::string& addr_hex);
void EnterCallMode();  void EnterMusicMode();  void PowerCycle();
Mode GetMode();  ConnState GetConnState();
// 示例：扫描并连第一个发现的设备
mhal::bt::SetCallbacks({.on_device_found = [](const mhal::bt::Device& d){
    mhal::bt::Connect(d.addr_hex);
}});
mhal::bt::SetMode(mhal::bt::Mode::Tx);   // 回包确认后
mhal::bt::StartScan();
```

### audio.h — mic/speaker PCM（未来 ASR 入口；惰性初始化）
```cpp
AudioCodec* Codec();                       // 完整类见公共头 audio_codec.h
void EnableInput(bool);  void EnableOutput(bool);
int ReadPcm(int16_t* dst, int samples);    // 16kHz 16-bit mono
int WritePcm(const int16_t* src, int samples);
void SetVolume(int percent, bool persist=true);  int GetVolume();
// 示例：抓 20ms mic 帧喂 ASR
mhal::audio::EnableInput(true);
int16_t frame[320];
mhal::audio::ReadPcm(frame, 320);
```

### power.h — 电池/电源
```cpp
bool GetBatteryLevel(int& level, bool& charging, bool& discharging);
bool GetVoltageMv(uint16_t& mv);  bool GetCurrentMa(int16_t& ma);
void ForcePowerOff();                      // PWR_KEY_PULSE ×10，不返回
// 示例
int lv; bool chg, dis;
if (mhal::power::GetBatteryLevel(lv, chg, dis)) ESP_LOGI("app", "bat %d%%", lv);
```

### sysmon.h — 系统监控
```cpp
void Start(uint32_t period_ms = 1000);     // 幂等
// 示例
mhal::sysmon::Start();
```

### storage.h — SD 卡状态（只读快照；挂载动作在 mhal::Init 内完成，失败不致命）
```cpp
bool IsSdMounted();            // Init 时挂载成功即 true（常驻挂载，无热插拔监测）
const char* GetMountPoint();   // VFS 挂载点，恒定 "/sdcard"（与挂载成败无关）
// 示例：依赖 SD 的功能按需降级
if (mhal::storage::IsSdMounted())
    ESP_LOGI("app", "SD ready at %s", mhal::storage::GetMountPoint());
```

### IOExpander.hpp — 按键/电源轨（原 API 不变）
```cpp
// 示例：pi_screen 的电源键注册（现有代码零改动）
IOExpander::getInstance().onClick(IOExpander::Pin::PWR_KEY, [](){ /* ... */ });
IOExpander::getInstance().onLongPress(IOExpander::Pin::PWR_KEY, 1500, [](){ /* 关机 */ });
IOExpander::getInstance().offLongPress(IOExpander::Pin::PWR_KEY);  // 撤销该 pin 全部长按回调
                                                                   // （offClick 的对称清理，screen 卸载时用）
IOExpander::getInstance().setLevel(IOExpander::Pin::BT_POWER, true);
```

### settings.h — NVS 包装（原 API 不变）
```cpp
Settings s("pi_screen", /*read_write=*/true);
s.SetInt("zen", 1);  int zen = s.GetInt("zen", 0);
```

## 3. 剥离映射表（旧 → 新 → 耦合点处理）

| 旧位置 | 新位置 | 耦合点处理 |
|---|---|---|
| boards/metalio-claw-4/metalio-claw-4.cc 构造序列 | src/hal.cc | 类解散为 Init()；DECLARE_BOARD/Board 单例删除 |
| 〃 InitializeLCD/Touch（NV3051F/FL7707N/GT911） | src/display/display_hal.cc | camera 用的 panel_io 全局导出删除（camera 已亡） |
| 〃 监控任务(lambda "_task") | src/sysmon.cc | 无耦合，原样搬 |
| 〃 CheckBatteryLevelAtBoot/Wxcho 无线充 | src/power/ | 无耦合，原样搬 |
| 〃 InitializeBTAudio + bluetooth_screen 的 AT/状态机 | src/bt/bt_module.cc | UI 写屏（post_status/add_device_to_list/lv_async_call）翻转为 Callbacks；LOAD/UNLOAD 注册的 UART 回调改 Init 时常驻 |
| boards/metalio-claw-4/config.h | src/config.h（保留原名） | 无耦合 |
| boards/metalio-claw-4/esp_lcd_{nv3051f,fl7707n}.{c,h} | src/display/ | 无耦合 |
| boards/common/wifi_board.cc StartNetwork/EnterWifiConfigMode | src/net/network.cc（Wi-Fi 分支） | Display::ShowNotification→OnEvent；Application::Alert/SetDeviceState→删；Lang::*→硬编码中文日志；afsk 声波配网删（Kconfig 未开） |
| boards/common/{dual_network_board,nt26_board,uart_eth_modem}.cc | src/net/ | Display::SetStatus→OnEvent；Application::Reboot→esp_restart；Application::Schedule→一次性 task（沿 2f05882 方案）；GetBoardJson/GetDeviceStatusJson/GetNetworkStateIcon 删（唯一调用方 ota/mcp/状态栏均亡） |
| boards/common/ml307_board.* | 删除 | 从未实例化（dual_network_board.cc:59 注释），78/esp-ml307 依赖一并删 |
| boards/common/board.{h,cc}（Board 基类） | 解散删除 | GetUuid/GetSystemInfoJson 调用方全亡；http/websocket/mqtt/udp/NetworkInterface 抽象无人用（pi 走 esp_http_client） |
| boards/common/backlight.{h,cc} | src/backlight.cc + 门面 | 无耦合（NVS 键不变） |
| boards/common/{IOExpander,SimpleUart}.hpp | include/IOExpander.hpp、src/bt/SimpleUart.hpp | 无耦合 |
| boards/common/bq27220_gauge.*、i2c_device.* | src/power/ | 无耦合 |
| boards/common/SdCardManager.hpp | src/（Init 内挂载） | 无耦合 |
| audio/audio_codec.{h,cc}、boards/common/bt_audio_codec.* | src/audio/ | audio_codec.cc 的 `#include "board.h"` 删（无实际使用）；其余 codec（es83xx/box/no/dummy）+AudioService+processors+wake_words 删 |
| main/settings.{cc,h} | lib include/settings.h + src/settings.cc | pi_screen include 路径不变 |
| main/system_info.* | 并入 src/sysmon.cc 私有 | GetUserAgent（dynamic_cast DualNetworkBoard，仅 protocols 用）删 |
| display/lv_adapter_display.cc 的 adapter 初始化 | src/display/display_hal.cc | SetupUI/BootScreen 逻辑移出到 main.cc；resources mmap 两段删（pi 字体全编译进固件）；Display 类层次删除 |
| display/touch_feed.* | src/display/ | 无耦合 |

## 4. 删除清单（业务层，git 历史保底）

`main/`：application.{cc,h}、audio/（整目录）、protocols/、ota.{cc,h}、mcp_server.{cc,h}、
assets.{cc,h}+assets/(locales/lang 生成链)、device_state_event.*、device_state.h、
api_endpoints.h、stock/、ebook/、led/、mmap_generate_resources.h、xingzhi-assets/、
display/{display,lcd_display,oled_display,emote_display,theme_manager,lv_adapter_display}.*、
display/lvgl_display/、display/font/font_puhui_{40_4,number_50_4}.c、
screen/{home,boot,ota,bluetooth}_screen/、boards/（整目录，已搬空）、
boards/common 未用死码：sy6970、axp2101、adc_battery_monitor、afsk_demod、esp32_camera、
camera.h、gps_service、knob、button、Weather.hpp、gzip_util、lamp_controller、
press_to_talk_mcp_tool、system_reset、power_save_timer、sleep_timer（后两者本板从未实例化
且深耦合 Application/AudioService）。
`wakeword/`、`scripts/`（gen_lang/build_default_assets 等生成链）、顶层 CMakeLists 的
wakeword override 块。screen_util.cc 的 `screen_register_pwr_key_toggle_chat`
（Application::ToggleChatState，pi 不用）删。

managed 依赖删除（main/idf_component.yml）：esp-sr、esp-opus-encoder、esp_codec_dev、
adc_mic、adc_battery_estimation、esp-ml307、esp_video、esp_new_jpeg、sscma_client、
otto-emoji-gif(+component)、image_player、esp_lv_eaf_player、esp_mmap_assets、
esp_image_effects、led_strip、button、knob、servo_dog_ctrl、sh1106、cpp_bus_driver、
esp32_p4_function_ev_board、全部非本板 LCD/touch 驱动（sh8601/ili9341/gc9a01/st77916/
axs15231b/st7701/st7796/spd2010/nv3023/jd9365/st7703/ili9881c/ek79007/ft5x06/gt1151/
cst9217/cst816s/st7123）、tca9554（16bit 版留）。

lib idf_component.yml 依赖：esp_hosted、esp_wifi_remote、78/esp-wifi-connect、
78/uart-uhci、iot_eth、esp_lcd_touch_gt911、esp_io_expander_tca95xx_16bit、
esp_lcd_panel_io_additions（防断，见风险）、esp_lvgl_adapter、esp_lvgl_port、lvgl。
main idf_component.yml 依赖：pi-c（path）、lvgl、esp_lvgl_adapter、78/xiaozhi-fonts
（pi_screen 用 font_puhui_20_4/30_4）、metalio_hal（本地组件自动发现）。

## 5. 新 main.cc 启动链

```
app_main:
  esp_event_loop_create_default + nvs_flash_init(坏页擦除重试)
  mhal::Init()                       // §2 hal.h 全序列，返回后 LVGL 已在跑
  mhal::display::Lock()
    pi = PiScreen::Create()
    screen_attach_lifecycle(pi, PiScreen::LifecycleCallback)  // LOAD 触发 pi_agent_task_start + PWR_KEY 注册
    lv_screen_load(pi)
  mhal::display::Unlock()
  mhal::network::StartAsync()        // 起网不阻塞首帧（原逻辑阻塞可达 60s）
  mhal::sysmon::Start()
```

pi_screen.cc 适配性修改（仅此三处，UI 逻辑不动）：
- 删 `#include "home_screen/home_screen.h"` 与 `screen_attach_swipe_back(...)`（单 App 无返回目标）；
- `IOExpander.hpp`/`settings.h` include 由 lib 公共头继续满足，代码零改动。

## 6. 承重项（不动）

sdkconfig 全部保留：FLASHSIZE 32MB、PARTITION_TABLE_CUSTOM(partitions/v1/32m.csv)、
PSRAM 全套、MIPI-DSI、LV_COLOR_DEPTH_24、CONFIG_LV_USE_FONT_COMPRESSED=y（pi 压缩字体，
关必崩）、ESP-Hosted C5 SDIO 整块、CONFIG_PI_FEATURE_{PARTIAL_JSON,MODELS_JSON,COMPAT}=y
（pi-c 组件 Kconfig）。分区表原样（resources 分区闲置无害）。managed_components 由
CM 依 yml 重解析。绝不 idf.py set-target。
密钥不进固件：大模型 / 语音密钥都存 NVS `"cfg"`，经设备 Web 后台配置（`components/device_config`）。

## 7. 能力对照表（验收凭证：任何一项不允许在终态固件里消失）

| # | 能力 | 旧实现位置 | 新 lib API（metalio_hal） | 状态 |
|---|---|---|---|---|
| 1 | 屏幕亮度 | boards/common/backlight.{h,cc}（PwmBacklight GPIO52 LEDC，NVS "display"/"brightness"）+ metalio-claw-4.cc:676 GetBacklight/RestoreBrightness | `mhal::backlight::SetBrightness/GetBrightness/Restore`（NVS 键不变，Init 自动 Restore） | 迁移 |
| 2 | 屏幕操作（面板/触摸/LVGL） | metalio-claw-4.cc InitializeLCD(NV3051F/FL7707N DSI)+InitializeTouch(GT911)+lv_adapter_display.cc adapter 初始化 | `mhal::Init()` 内起显；`mhal::display::GetLvDisplay/Lock/Unlock/Width/Height` | 迁移 |
| 3 | Wi-Fi（ESP-Hosted C5 SDIO 全栈） | boards/common/wifi_board.cc（WifiStation/SsidManager/WifiConfigurationAp） | `mhal::network::Start/IsConnected/OnEvent/AddWifiCredential/StartConfigPortal`（Type::WiFi 分支） | 迁移 |
| 4 | 4G（NT26/DualNetworkBoard） | boards/common/{dual_network_board,nt26_board,uart_eth_modem}.cc（UART1 2Mbps + iot_eth） | `mhal::network::Start`（Type::Cellular 分支）`/GetType/SwitchType/SendAtCommand/GetSignalStrength/GetRegistrationState` | 迁移 |
| 5 | 蓝牙（BT 音频模组控制） | bluetooth_screen.cc AT 状态机 + metalio-claw-4.cc InitializeBTAudio(UART2 115200) | `mhal::bt::ApplyDefaultMode/SetMode/StartScan/Connect/EnterCallMode/EnterMusicMode/PowerCycle/SetCallbacks/GetMode/GetConnState` | 迁移（UI 删，硬件逻辑全保留） |
| 6 | 音频 mic/speaker 编解码 | audio/audio_codec.{h,cc} + boards/common/bt_audio_codec.{h,cc}（I2S0 slave 16k 全双工）+ metalio-claw-4.cc:667 GetAudioCodec | `mhal::audio::Codec/ReadPcm/WritePcm/EnableInput/EnableOutput/SetVolume/GetVolume`（AudioCodec 类原样保留，ASR 可直用） | 迁移（仅删 AudioService 会话层/唤醒词） |
| 7 | 电池/电源 | boards/common/bq27220_gauge.* + metalio-claw-4.cc CheckBatteryLevelAtBoot/Wxcho 无线充电流配置/GetBatteryLevel | `mhal::power::GetBatteryLevel/GetVoltageMv/GetCurrentMa/ForcePowerOff`；开机 0% 保护与无线充监控任务在 Init 内 | 迁移 |
| 8 | IOExpander 按键/电源控制 | boards/common/IOExpander.hpp（TCA9555：PWR_KEY 单击/长按、BT_POWER、RST_4G、PA、SD、CAM_PWDN、PWR_KEY_PULSE） | `IOExpander.hpp` 整体成为 lib 公共头（API 不变，pi_screen 现有用法零改动）；上电序列在 `mhal::Init()` | 迁移 |
| 9 | 系统监控 | metalio-claw-4.cc 匿名监控任务（双核 CPU 占用/内存水位/电池，1s 周期） | `mhal::sysmon::Start(period_ms)` | 迁移 |
| 10 | SD 卡（顺带） | boards/common/SdCardManager.hpp + metalio-claw-4.cc InitializeSdCard | lib 内保留 SdCardManager（Init 内挂载，失败不致命） | 迁移 |

## 8. 资产清理（用户追加需求）

判定标准 = 终态代码可达性；逐项：

| 资产 | 用途（旧） | 终态可达性 | 处置 |
|---|---|---|---|
| main/xingzhi-assets/ → resources.bin（4M resources 分区） | 已删 screen 的表情/图标/EAF 动画（"A:" 盘符 lv_image） | pi 零引用（字体全为编译进固件的 C 符号）；mmap 挂载点随 lv_adapter_display 删除 | 删目录 + 删 `spiffs_create_partition_assets` 块 + 删 esp_mmap_assets 依赖；分区闲置不烧 |
| main/assets/locales/*.ogg（EMBED_FILES） | xiaozhi 语音提示音 | Application/AudioService 删除后零引用 | 删 assets/ 目录 + EMBED_FILES + gen_lang 生成链 + Kconfig LANGUAGE_* |
| wakeword/srmodels.bin + esp-sr model | 唤醒词模型（model 分区） | 唤醒词子系统删除后零引用 | 删 wakeword/ + 顶层 CMake override 块 + esp-sr 依赖；model 分区闲置不烧 |
| display/font/font_puhui_{40_4,number_50_4}.c | 已删 screen 专用大字体 | 零引用 | 删 |
| 78/xiaozhi-fonts 的 font_puhui_20_4/30_4 | pi_screen 中文正文 | **pi_screen 直接引用** | 保留依赖 |
| display/font/font_pi_*.c（3 个压缩字体） | pi 屏专用 | pi 引用 | 保留（LV_USE_FONT_COMPRESSED=y 承重） |
| scripts/（gen_lang.py、build_default_assets.py 等） | 资产/语言生成链 | 生成链删除后零引用 | 删（无引用的辅助脚本一并删） |

分区表 partitions/v1/32m.csv 本身不动：resources(4M)/model(960K) 分区闲置，
flash_args 不再包含其烧写项（对应 CMake 声明删除后自动消失），风险最低且可回退。

## 9. 待终审清单（本轮"有把握才删"原则下保留的模糊边界，供终审 agent 逐项裁决）

| 项 | 现状 | 倾向 |
|---|---|---|
| `.github/`（build.yml/release.yml + issue 模板） | build.yml/release.yml 引用已删除的 `scripts/release.py` 与 board 矩阵，推到 GitHub 会跑失败的 CI | 删或重写为单一 `idf.py build` 工作流 |
| `README.md` / `README_zn.md` | xiaozhi 上游 README，内容与本仓库已严重不符 | 重写为 Claw6 简介（CLAUDE.md 已重写可作素材） |
| `main/display/screen/pi_screen/pi_mock_paced.c` + `pi_scr_mock.h` | mock 走带（`PI_AGENT_TASK_USE_MOCK 0` 编译期关闭），仍参与编译 | pi_screen 属验收成品，倾向保留（调试价值）；删则同步改 pi_agent_task.c/CMakeLists |
| SD 卡能力（SdCardManager + Init 挂载） | 用户能力清单未点名（"顺带"精神保留），无 UI 消费者 | 保留（硬件在、代码 header-only、成本≈0） |
| `78/xiaozhi-fonts` 整包依赖 | 只用 font_puhui_20_4/30_4 两个字体（未选中的不编译） | 保留（改自带字体源文件收益小、风险高） |
| managed_components 传递依赖（button/knob/freetype/esp_lv_decoder/esp_lv_fs/esp_mmap_assets/esp_new_jpeg/libpng/zlib） | esp_lvgl_adapter 自身 manifest 拉入，不可在本仓库删 | 保留（除非 fork adapter，不值） |
| sdkconfig 孤儿项（esp-sr/opus/LANGUAGE/BOARD_TYPE 等已删组件的 CONFIG_*） | 本地 sdkconfig 未跟踪，重新生成时自动消失 | 不动 |
| Wi-Fi 配网 portal（StartConfigPortal/force_ap）与 AddWifiCredential | 已迁入 lib 并编译通过，但从未真机验证（原 call site 在旧固件里就是注释掉的） | WP4 真机验证时裁决 |
| `partitions/v1/32m.csv` 中闲置分区（resources 4M / model 960K / ota_1 12M 双槽） | 不再烧写但仍占表；改单槽表可省约 17M 空间 | 留给真机验证后裁决（分区表变更需整机重烧，风险另计） |
| `.clang-format` / `LICENSE` / `docs/` | 正常仓库件 | 保留 |

已在本轮"有把握"追加删除（终审无需再看）：`sd_images/`（已删 digital_people 屏的 SD 卡表情
资产源 + 转换工具，5MB）、`partitions/` 除 v1/32m.csv 外全部变体（sdkconfig 只引用 32m.csv）；
项目 `CLAUDE.md` 已重写为 Claw6 现状（旧版描述 Application/Board/McpServer/release.py 均已失实）。

## 10. 里程碑提交

1. docs/EXTRACTION.md（本文档）。
2. components/metalio_hal 建立：硬件代码搬迁+耦合翻转，main 侧改用 lib（此时业务层可能仍在编译）。
3. 删除全部业务层与残余 screen + 资产清理，main.cc 重写，CMake/依赖收敛；build + size + grep 验证。
（2/3 若耦合导致无法各自独立编译通过，允许合并为一个 commit，以每 commit 可构建为先。）
4. sdkconfig.defaults 承重集 + 文档 as-built 同步。
5. 终态清扫：sd_images/分区表变体删除、CLAUDE.md 重写、待终审清单（本节）。
