# Metalio Claw6 — pi Agent 终端固件

ESP32-P4 掌上设备（720×720 MIPI-DSI 触摸屏）的单一用途固件：开机直接进入
[pi-c](../../six-ddc/pi-c) Agent 对话界面（`main/display/screen/pi_screen/`），
通过 DeepSeek API 进行流式对话。没有菜单、没有其他 App —— UI 只有这一个屏。

本仓库源自 xiaozhi-esp32 的 fork（Claw4/Claw5），Claw6 重构删除了全部 xiaozhi
业务层，硬件能力收敛为可复用组件 **`components/metalio_hal/`**。重构的权威记录
（as-built API、旧→新映射、能力验收矩阵、资产清理清单）见 **`docs/EXTRACTION.md`**。

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
- **兄弟仓库布局**：pi-c 是 path 依赖（`main/idf_component.yml`），两仓必须并排：
  `Code/esp32/MetalioClaw6` ↔ `Code/six-ddc/pi-c`。
- 设备会枚举出**四个**串口，只对 *USB JTAG/serial debug unit* 烧录 P4 固件；其余三个
  （CH340K = 蓝牙音频芯片，log/at = NT26 4G 模组）属于独立子系统，不要烧。
- `sdkconfig` 不入库；`sdkconfig.defaults` 携带全部承重配置（PSRAM/DSI/压缩字体/
  ESP-Hosted 引脚等），细节与禁忌见 `CLAUDE.md`。

## models.json 配置（含密钥，不入库）

真实 API 的模型目录来自 `main/display/screen/pi_screen/pi_models_data.h` ——
该文件被 gitignore（内含 DeepSeek API key），构建前必须在本地生成：

1. 准备好 `six-ddc/pi-c/models.json`（pi-c 仓库同样对其 gitignore）；
2. 把它的内容原样写成 C 字符串字面量，放进 `pi_models_data.h`，宏名为
   `PI_MODELS_JSON_TEXT`（格式参考 `pi_agent_task.c` 文件头注释——无需任何
   构建系统 embed 步骤）。

**绝不要把该文件或任何 key 提交进 git。** 每次提交前用 `git status` 复核。

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
