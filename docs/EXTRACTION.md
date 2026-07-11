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
  src/                         # 私有实现（对外不可见）
    board_pins.h               # 原 boards/metalio-claw-4/config.h
    hal.cc                     # Init 序列（原 METALIO_CLAW_4 构造函数）
    display/ (display.cc, touch_feed.{cc,h}, esp_lcd_nv3051f.{c,h}, esp_lcd_fl7707n.{c,h})
    backlight.cc               # 原 boards/common/backlight.cc（PwmBacklight 内化）
    net/ (network.cc, wifi_backend.cc, nt26_backend.{cc,h}, uart_eth_modem.{cc,h})
    bt/ (bt_module.cc, SimpleUart.hpp)
    audio/ (audio_codec.{cc,h}, bt_audio_codec.{cc,h})
    power/ (power.cc, bq27220_gauge.{cc,h}, wireless_charger.cc, i2c_device.{cc,h})
    sysmon.cc                  # 监控任务 + system_info 工具
    settings.cc
```

`main/` 终态：`main.cc`（新写）、`display/screen/pi_screen/*`、`display/screen/screen_util.{cc,h}`、`display/font/font_pi_*.c`、`CMakeLists.txt`、`idf_component.yml`（仅 UI 侧依赖）、`Kconfig.projbuild`（仅 pi 相关残留，语言/board/audio 选项删）。

## 2. 门面 API 草案（签名以最终头文件为准）

### hal.h
```cpp
namespace mhal {
// I2C → IOExpander(上电序列) → BQ27220 → 开机电量保护 → BT UART+默认模式
// → SD 挂载(失败不致命) → LCD(NV3051F/FL7707N) → GT911 → LVGL adapter 起显
// → 无线充监控任务 → 背光恢复。全部完成后即可加载 LVGL screen。
struct InitOptions {
    bool mount_sd_card   = true;
    bool bt_default_mode = true;   // 开机发 AT+RX=2 / AT+MODE=1
    bool battery_boot_guard = true;// 0% 且未充电 → PWR_KEY_PULSE 强制关机
};
esp_err_t Init(const InitOptions& opts = {});
}
```

### display.h
```cpp
namespace mhal::display {
lv_display_t* GetLvDisplay();          // Init() 后有效
bool Lock(int timeout_ms = -1);        // esp_lv_adapter_lock 包装
void Unlock();
int  Width();  int Height();           // 720/720
}
```

### backlight.h
```cpp
namespace mhal::backlight {
void    SetBrightness(uint8_t percent, bool persist = false); // NVS "display"/"brightness"
uint8_t GetBrightness();
void    Restore();                     // Init() 已自动调用
}
```

### network.h
```cpp
namespace mhal::network {
enum class Type { WiFi = 0, Cellular = 1 };          // NVS "network"/"type"
enum class Event { ModemDetecting, Connecting, Connected, Disconnected,
                   ErrorNoSim, ErrorRegDenied, ErrorTimeout, WifiNoCredentials };
void Start();                        // 阻塞式起网（在后台任务里调）
void StartAsync();                   // 内部 xTaskCreate 包装
Type GetType();
void SwitchType();                   // 持久化另一类型 + esp_restart()
bool IsConnected();
void OnEvent(std::function<void(Event)> cb);   // 替代原 Display::SetStatus 耦合
// Wi-Fi 配网：
void AddWifiCredential(const std::string& ssid, const std::string& password); // SsidManager
void StartConfigPortal();            // softAP 网页配网（阻塞，配置完成后设备重启）
}
```

### bluetooth.h
```cpp
namespace mhal::bt {
enum class Mode { None = 0, Rx = 1, Tx = 2, MusicRx = 3 };
enum class ConnState { Idle, Scanning, Connecting, Connected };
struct Device { std::string addr_hex; std::string name; };
struct Callbacks {
    std::function<void(Mode)>               on_mode_changed;
    std::function<void(ConnState)>          on_conn_state;
    std::function<void(const Device&)>      on_device_found;
    std::function<void(const std::string&)> on_status_text;  // 原 post_status 文案
};
void SetCallbacks(Callbacks cbs);     // UI 可选订阅；lib 不反向依赖 UI
void ApplyDefaultMode();              // AT+RX=2 → AT+MODE=1（Init 可自动做）
void SetMode(Mode m);
void StartScan();                     // AT+INQUIRING
void Connect(const std::string& addr_hex);   // AT+CONNECT=<12hex>
void EnterCallMode();  void EnterMusicMode();
void PowerCycle();                    // IOExpander BT_POWER 断电 300ms 重上电
Mode GetMode();  ConnState GetConnState();
}
```

### audio.h
```cpp
namespace mhal::audio {
AudioCodec* Codec();                  // BTAudioCodecDuplex 单例（I2S0 slave 16k）
void EnableInput(bool);  void EnableOutput(bool);
int  ReadPcm(int16_t* dst, int samples);        // mic → PCM（ASR 喂料入口）
int  WritePcm(const int16_t* src, int samples); // PCM → speaker
void SetVolume(int percent, bool persist = true); // NVS "audio"/"output_volume"
int  GetVolume();
}
```

### power.h
```cpp
namespace mhal::power {
bool GetBatteryLevel(int& level, bool& charging, bool& discharging); // BQ27220
bool GetVoltageMv(uint16_t& mv);
bool GetCurrentMa(int16_t& ma);
void ForcePowerOff();                 // PWR_KEY_PULSE ×10 脉冲
}
```

### sysmon.h
```cpp
namespace mhal::sysmon {
void Start(uint32_t period_ms = 1000); // 双核CPU占用/内存水位/电池 周期日志任务
}
```

### 按键
直接暴露 `IOExpander.hpp`（现有单例 API：`onClick/offClick/onLongPress/setLevel/readLevel`），
pi_screen 现有 `IOExpander::getInstance().onClick(PWR_KEY,…)` 用法不变。

## 3. 剥离映射表（旧 → 新 → 耦合点处理）

| 旧位置 | 新位置 | 耦合点处理 |
|---|---|---|
| boards/metalio-claw-4/metalio-claw-4.cc 构造序列 | src/hal.cc | 类解散为 Init()；DECLARE_BOARD/Board 单例删除 |
| 〃 InitializeLCD/Touch（NV3051F/FL7707N/GT911） | src/display/display.cc | camera 用的 panel_io 全局导出删除（camera 已亡） |
| 〃 监控任务(lambda "_task") | src/sysmon.cc | 无耦合，原样搬 |
| 〃 CheckBatteryLevelAtBoot/Wxcho 无线充 | src/power/ | 无耦合，原样搬 |
| 〃 InitializeBTAudio + bluetooth_screen 的 AT/状态机 | src/bt/bt_module.cc | UI 写屏（post_status/add_device_to_list/lv_async_call）翻转为 Callbacks；LOAD/UNLOAD 注册的 UART 回调改 Init 时常驻 |
| boards/metalio-claw-4/config.h | src/board_pins.h | 无耦合 |
| boards/metalio-claw-4/esp_lcd_{nv3051f,fl7707n}.{c,h} | src/display/ | 无耦合 |
| boards/common/wifi_board.cc StartNetwork/EnterWifiConfigMode | src/net/wifi_backend.cc | Display::ShowNotification→OnEvent；Application::Alert/SetDeviceState→删；Lang::*→硬编码中文日志；afsk 声波配网删（Kconfig 未开） |
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
| display/lv_adapter_display.cc 的 adapter 初始化 | src/display/display.cc | SetupUI/BootScreen 逻辑移出到 main.cc；resources mmap 两段删（pi 字体全编译进固件）；Display 类层次删除 |
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
`main/display/screen/pi_screen/pi_models_data.h`（gitignored 真钥）不动、不入库。

## 7. 里程碑提交

1. docs/EXTRACTION.md（本文档）。
2. components/metalio_hal 建立：硬件代码搬迁+耦合翻转，main 侧改用 lib（此时业务层可能仍在编译）。
3. 删除全部业务层与残余 screen，main.cc 重写，CMake/依赖收敛；build + size + grep 验证。
（2/3 若耦合导致无法各自独立编译通过，允许合并为一个 commit，以每 commit 可构建为先。）
