# pi_sim — pi_screen 桌面模拟器（macOS / SDL2）

把固件的 pi Agent 四状态 UI **零改动**编译到 macOS 上跑：LVGL 渲染进 720×720 的
SDL2 窗口，agent 走 pi-c 自带的 POSIX port（libcurl）**直连真 DeepSeek API**，
硬件层换成 `sim/shim/` 里的桌面实现。

## 构建 / 运行

```bash
brew install sdl2                       # 一次性
cmake -S sim -B sim/build
cmake --build sim/build -j
./sim/build/pi_sim                      # 从任意目录运行均可
```

前置条件（均为仓库现状，缺了 CMake 会直接报错）：

- `managed_components/lvgl__lvgl` 存在（跑过一次 `idf.py build` 即有）——模拟器
  直接用这份源码，保证与设备**严格同版**（9.3.0）；
- `main/display/screen/pi_screen/pi_models_data.h` 存在（gitignored 密钥文件）；
- pi-c 仓库在兄弟目录 `../../six-ddc/pi-c`（与固件同一路径约定）。

## 交互映射

| 设备 | 模拟器 |
|---|---|
| PWR_KEY 单击 | **F1**（待机→聆听 / 聆听→取消 / chat→聆听或 STOP；息屏时仅唤醒） |
| PWR_KEY 长按 1.2s | **F2**（快捷面板呼出/收起） |
| 状态栏下拉（呼出快捷面板） | 鼠标按住顶部状态栏向下拖 >60px |
| Chat 右滑回待机（新对话确认 sheet）/ 设置页右滑返回 | 鼠标按住向右拖 >80px |
| 设置栈（六页 Hub） | 快捷面板「设置」钮进入；主题切换在面板「主题」钮或 设置›显示 |
| 对着麦克风说话 | 聆听时**直接在窗口里打字**（支持中文输入法、退格） |
| VAD 静音自动收音 | 停止打字 1 秒自动发送；**回车**立即发送 |
| 触摸 | 鼠标（按住说话、STOP、ZEN/FLOW、TTS 开关、滚动都可用） |
| TTS 播报 | 控制台打印 `[sim][TTS] …`；`PI_SIM_SAY=1` 时经 macOS `say` 朗读 |
| NVS（zen_mode/tts_on/ui.theme/ui.sleep_s/bt.last_* 等） | `pi_sim_settings.ini`（`PI_SIM_SETTINGS` 可改路径） |
| SD 卡（mhal::storage） | storage shim：运行目录下 `./pi_sim_sd/` **目录存在 = 有卡**（`PI_SIM_SD` 可改路径）。当前无消费者，仅保留通用只读 HAL 桩 |
| — | **F12** 截图（BMP，`PI_SIM_SHOT` 可改路径） |

## 脚本驱动测试（PI_SIM_CMDFILE）

设 `PI_SIM_CMDFILE=/tmp/pi_cmd` 后，模拟器每 100ms 轮询该文件，读完即删，
每行一条命令——配合虚拟触摸 indev（等价 GT911 的第二个 pointer）可以脚本化
驱动任意交互并截图核对：

```
key                 # PWR_KEY 单击
longkey             # PWR_KEY 长按 1.2s（快捷面板）
type <文本>         # 聆听时"说话"
enter / backspace   # 立即收音 / 删一个码点
click <x> <y>       # 点按（按下 250ms 后自动松开）
press <x> <y>       # 按下并保持（PTT）
move <x> <y>        # 按住时移动（手势，如上滑取消）
release             # 松开
shot <路径.bmp>     # 截图
quit                # 退出
```

写入要原子：`printf 'click 360 400\n' > f.tmp && mv f.tmp /tmp/pi_cmd`。

注意：从被 macOS 降为 background QoS 的环境（脚本/守护进程）启动时，
`SDL_Delay(10)` 会被 timer coalescing 拉长到 ~95ms，全 UI 降到 ~10fps——
这是**启动环境**的节流，终端前台运行不受影响；click 的 250ms 保持窗口
已按此校准。

## 无人值守自检 / 截图

```bash
SDL_VIDEODRIVER=dummy \
PI_SIM_AUTODEMO="你好，先一句话介绍你自己，然后用工具算 37*89" \
PI_SIM_SHOT=/tmp/pi_sim_shot.bmp PI_SIM_SHOT_MS=15000 PI_SIM_EXIT_MS=16000 \
./sim/build/pi_sim
```

自动按 F1 → "说"出 `PI_SIM_AUTODEMO` 文本 → 真 API 流式回复 → 15s 截图 →
16s 退出。截图走 LVGL 软件渲染（`lv_snapshot`），dummy 驱动下也是真像素。

## 结构

```
sim/
├── CMakeLists.txt     # LVGL(managed_components) + pi-c(host) + UI 源码 + shim
├── lv_conf.h          # host 配置；镜像 sdkconfig 的 FONT_COMPRESSED / TXT_LARGE
├── main.cc            # SDL 窗口 + main/main.cc 无硬件部分的启动链 + 事件注入
└── shim/
    ├── include/       # freertos/* esp_log esp_err settings IOExpander
    │                  # metalio_hal/{audio_pipeline,audio,backlight,power}
    │                  # pi_esp32 sim_hooks
    └── src/           # pthread 版 FreeRTOS、文件版 Settings、F1/F2=PWR_KEY、
                       # 打字=ASR、TTS 打印/say、mhal 音量/亮度/电量桩、
                       # pi_esp32_*→pi_posix_*
```

原则：`main/display/**` 与 `pi_agent_task.c` 保持字节级不动；所有平台差异
都收敛在 shim。`pi_esp32_{alloc,sys,transport}()` 三个符号在 host 上由
`shim/src/esp_shim.c` 转发到 `pi_alloc_default / pi_posix_sys /
pi_posix_curl_transport`，因此 agent 行为（models.json 目录、DeepSeek compat、
流式事件桥）与设备完全一致。

已知差异：S1 的 ASR/VAD 是"打字模拟"（`volc_asr` 桩），不采集真实音频；
TTS 不产生音频（可选 `say`）。其余（四状态流转、流式渲染、工具卡片、
错误横幅/重试、ZEN、上下文表、token 用量、快捷面板、设置栈六页、双主题、
息屏状态机）全部走真实代码路径（网络/蓝牙/电量为 shim 假数据）。
