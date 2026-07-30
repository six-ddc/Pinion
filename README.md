# Pinion — pi Agent 终端固件

ESP32-P4 掌上设备（720×720 MIPI-DSI 触摸屏）的单一用途固件：开机直接进入
[pi-c](components/pi_c_prebuilt/) Agent 对话界面（`main/display/screen/pi_screen/`），
通过 DeepSeek API 进行流式对话。没有菜单、没有其他 App —— UI 只有这一个屏。

除了对话，模型还能**自己画界面**：`ui_render` 工具让 LLM 吐一份声明式 UI 描述，
设备把它求解成布局、渲染为原生 LVGL 网格卡片，并可绑定实时数据（行情、媒体、
传感器）——设计说明见 **`docs/AI_TO_UI.md`**。

本仓库源自 xiaozhi-esp32 的 fork，xiaozhi 业务层已整体删除，硬件能力收敛为可复用
组件 **`components/metalio_hal/`**。该重构的权威记录（as-built API、旧→新映射、
能力验收矩阵、资产清理清单）见 **`docs/EXTRACTION.md`**。

## 硬件

| 部件 | 说明 |
|---|---|
| 主控 | ESP32-P4（双核 RISC-V 480 MHz，32 MB Flash，32 MB PSRAM） |
| 屏幕 | 3.95″ 720×720 MIPI-DSI（NV3051F / FL7707N 面板，运行时探测），GT911 触摸 |
| Wi-Fi | ESP32-C5 协处理器，ESP-Hosted SDIO |
| 4G | NT26 模组（UART eth-modem），网络类型存 NVS `"network"/"type"`（0=WiFi，1=4G，默认 4G） |
| 音频 | 蓝牙音频编解码芯片（UART AT 控制）+ I2S 16 kHz 全双工 codec |
| 电源 | BQ27220 电量计、无线充电、TCA9555 IO 扩展（电源键/电源轨） |

## 构建与烧录

```bash
source ./.idf-env.sh                  # 激活项目本地 ESP-IDF v5.5.4（见下）
idf.py build                          # sdkconfig 已预调好；绝不要运行 idf.py set-target
idf.py -p /dev/ttyACM0 flash monitor  # P4 口 = "USB JTAG/serial debug unit"；Ctrl+] 退出 monitor
```

- **项目本地 ESP-IDF**：`.esp-idf/` 与 `.idf-env.sh` 均不入库。fresh clone 后：
  1. `git clone --branch v5.5.4 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git .esp-idf`
     （必须 v5.5.4：managed 依赖 uart-uhci 需要 `idf >=5.5.2` 的 UHCI driver）；
  2. `cd .esp-idf && ./install.sh esp32p4`；
  3. 参照 `CLAUDE.md` 的 "Build / flash" 一节写 `.idf-env.sh`（导出 `IDF_PATH`、
     `IDF_TOOLS_PATH`、`IDF_PYTHON_ENV_PATH` 后 source `export.sh`）。
- **pi-c 依赖**：以**预编译静态库**随仓交付（`components/pi_c_prebuilt/`，私有上游、
  不 vendor 源码）。它是 `components/` 下的普通本地组件，无需在 `main/idf_component.yml`
  声明，也**不需要**把 pi-c 源码仓 clone 到旁边。上游更新后用
  `components/pi_c_prebuilt/pack_pi_c.sh` 重新打包。
- 设备会枚举出**四个**串口，只对 *USB JTAG/serial debug unit* 烧录 P4 固件；其余三个
  （CH340K = 蓝牙音频芯片，log/at = NT26 4G 模组）属于独立子系统，不要烧。
- `sdkconfig` 不入库；`sdkconfig.defaults` 携带全部承重配置（PSRAM/DSI/压缩字体/
  ESP-Hosted 引脚等），细节与禁忌见 `CLAUDE.md`。

## 密钥配置（必做，两个文件都不入库）

克隆后有两个含密钥的头文件必须在本地生成，否则**编译失败**。两者都已按文件名
gitignore，**绝不入库、绝不打印进日志**；每次提交前用 `git status` 复核。

### 1. 模型目录 — `main/display/screen/pi_screen/pi_models_data.h`

一份 pi-c 格式的 models.json，minify 后转义成 C 字符串字面量（宏名
`PI_MODELS_JSON_TEXT`），内含 DeepSeek API key。从模板复制后把 `apiKey` 换成自己的：

```sh
cp main/display/screen/pi_screen/pi_models_data.h.example \
   main/display/screen/pi_screen/pi_models_data.h
```

模板里有展开可读的 JSON 结构、各字段说明（哪些是承重的、缺了会怎么崩），以及换
provider / 重新转义用的一行命令。

### 2. 语音密钥 — `components/volc_speech/include/volc_keys.h`

火山引擎语音（ASR + TTS）的 App ID / Access Token，从模板复制后填入：

```sh
cp components/volc_speech/include/volc_keys.h.example \
   components/volc_speech/include/volc_keys.h
```

需在火山控制台开通：流式语音识别大模型（resource `volc.seedasr.sauc.duration`）
与双向流式语音合成（resource `seed-tts-2.0`）。资源名与音色以宏硬编码在
`src/volc_asr.cc` / `src/volc_tts.cc` 顶部，换产品改宏即可。详见
**`docs/VOLC_SPEECH.md`**。

## 代码结构

- `main/` — 仅 UI：`main.cc`（启动链：NVS → `mhal::Init()` → 加载 pi_screen →
  `mhal::network::StartAsync()` → `mhal::sysmon::Start()`）、
  `display/screen/pi_screen/`（对话 UI + agent 任务）、`display/screen/screen_util.*`、
  3 个压缩 pi 字体。
- `components/metalio_hal/` — 硬件库，公共门面头在 `include/metalio_hal/`：
  `hal.h`（一站式初始化）、`display.h`、`backlight.h`、`network.h`（Wi-Fi/4G 双网）、
  `bluetooth.h`、`audio.h`、`power.h`、`sysmon.h`，另有直通头 `IOExpander.hpp`、
  `settings.h`、`audio_codec.h`。**每个 API 的签名与调用示例见 `docs/EXTRACTION.md` §2。**
  lib 不引用任何 UI/业务代码，向上仅通过注册回调通知。

## 许可

MIT，见 [LICENSE](LICENSE)。上游致谢：[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)。
