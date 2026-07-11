# volc_speech — 火山引擎流式语音（ASR/TTS）设备端直连组件

设备直连火山引擎（豆包）开放平台，无中转服务器：

- **ASR**：麦克风 PCM 流式上传 → 流式转写文本（SAUC bigmodel，
  `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`）
- **TTS**：文本流式输入 → 音频流式下发经扬声器播放（双向流式，
  `wss://openspeech.bytedance.com/api/v3/tts/bidirection`）

协议实现字节级对齐已在生产验证过的 TS 参考实现
（`ai-chat-esp32/service/src/asr.ts`、`tts.ts`、`volcengine/protocols.ts`）。

## 密钥配置（必做，否则编译失败）

```sh
cp components/volc_speech/include/volc_keys.h.example \
   components/volc_speech/include/volc_keys.h
# 编辑填入火山控制台的 App ID / Access Token
```

`volc_keys.h` 已按文件名全局 gitignore，**绝不入库、绝不打印**。
需开通的产品：流式语音识别大模型（resource `volc.seedasr.sauc.duration`）、
双向流式语音合成（resource `seed-tts-2.0`，音色 `zh_female_xiaohe_uranus_bigtts`；
三者以宏硬编码在 `src/volc_asr.cc` / `src/volc_tts.cc` 顶部，换产品改宏即可）。

## API

### ASR（include/volc_asr.h）— 一次识别 = 一条 WSS 连接

```c
volc_asr_callbacks_t cbs = {
    .on_delta = [](const char* text, void* ctx){ /* 中间结果（全量文本） */ },
    .on_final = [](const char* text, void* ctx){ /* 最终结果，恰好一次 */ },
    .on_error = [](int code, const char* msg, void* ctx){ /* 火山错误码或负 esp_err */ },
    .ctx = ...,
};
volc_asr_start(&cbs);                 // 建连+握手，阻塞数百 ms

// 喂料：用 mhal::audio_pipeline 采集（推荐，带预热/VAD/固定帧回调）
mhal::audio_pipeline::CaptureConfig cap_cfg;      // 默认 20ms 帧 + 能量 VAD
mhal::audio_pipeline::CaptureCallbacks cap_cbs;
cap_cbs.on_frame = [](const int16_t* pcm, size_t n) { volc_asr_feed(pcm, n); };
cap_cbs.on_vad   = [](bool speaking) { /* 静音挂起可触发自动收音 */ };
mhal::audio_pipeline::StartCapture(cap_cfg, cap_cbs);
// ... 录音期间 ASR on_delta 持续回调 ...
mhal::audio_pipeline::StopCapture();
volc_asr_stop(10000);                 // 发末段(负序号)，最多等 10s final；返回后会话已释放
// 用户取消：volc_asr_abort();  查询：volc_asr_is_active();
```

采用**调用方推流（push）**而非组件自拉 mic：录音生命周期（按住说话/VAD）
由上层控制，且 mic 通路未来还有回声消除/唤醒词等消费者，组件不独占。
喂料端与 `mhal::audio_pipeline::StartCapture` 的 `on_frame` 直接对接
（见《音频管线》一节）；不经管线直接 `mhal::audio::ReadPcm` 推 feed 也可以。

### TTS（include/volc_tts.h）— 会话式，连接跨会话复用

```c
volc_tts_callbacks_t cbs = {
    .on_audio_start = ...,   // 第一帧音频（UI 切"说话中"）
    .on_finished    = ...,   // 播完（含播放队列排空）
    .on_error       = ...,
};
volc_tts_speak_begin(&cbs);           // 建/复用连接 + StartSession
volc_tts_feed_text("你好，");          // LLM text_delta 逐段追加
volc_tts_feed_text("我是小派。");
volc_tts_speak_end();                 // FinishSession；音频继续到达并播放
// 阻塞等播完（可选）：volc_tts_wait_done(30000);
// 打断（barge-in）：volc_tts_stop();  // 丢缓冲 + CancelSession，连接保留
```

音频路径走播放管线：WS 任务 → `mhal::audio_pipeline::FeedPlayback`
（64KB PSRAM 抖动队列 ≈2s，满则阻塞 WS 任务形成 TCP 背压）→ 播放任务
写 I2S。100ms 低水位预启动防欠载；打断延迟 ≤128ms（单次写块上限）；
播完由 `OnPlaybackDrained` 排空回调驱动 `on_finished`。

## 音频管线（metalio_hal/audio_pipeline.h）

采集/播放基建从旧 xiaozhi 固件 AudioService 剥离（去 protocol/
Application/wake_word/opus 耦合），落位 `mhal::audio_pipeline`：
硬件邻接、协议无关，未来唤醒词/本地音效等消费者都在这一层。

- **采集**：`StartCapture(cfg, cbs)` 独立任务（优先级 8）拉裸 codec 流，
  120ms 预热丢弃后按固定帧（10–200ms 可配）回调；内置能量迟滞 VAD
  （enter/exit 阈值 + hangover，可关）。`StopCapture()` 关 mic 通路。
- **播放**：`EnsurePlayback()` 幂等起播放任务；`FeedPlayback` 流式喂入
  （背压点）；`FlushPlayback()` barge-in 清空即静音；`OnPlaybackDrained`
  一次性排空回调；队列空闲 15s 自动关扬声器通路。播放中音量控制直接用
  `mhal::audio::SetVolume`（codec 级实时生效）。
- **AFE（esp-sr）不纳入的理由**：其 vadnet/nsnet 模型链与 esp-sr 组件在
  metalio_hal 提取期已整体裁掉（重加约 1MB+ flash、数 MB PSRAM 与模型
  分区烧写链）；本板 mono mic 无回采参考通道，AEC 根本不可用、NS 近场
  收益有限；火山 bigmodel 云端识别本身抗噪。管线处理器挂点与旧
  AfeAudioProcessor 同形（Feed→帧回调），后续要上 AFE 时替换任务内处理
  段即可，外部 API 不变。
- **Opus 不剥离**：火山 ASR 上行是 gzip PCM、TTS 下行是裸 PCM，全链路
  无 Opus 消费者（那是 xiaozhi 自有服务端协议的要求）。

### 自测（include/volc_speech_selftest.h）

```c
volc_speech_selftest();   // 录3秒 → ASR → 把识别文本喂 TTS → 播放
```

阻塞式（数十秒），需网络已连通；符号经 CMake `-u volc_speech_selftest`
强制保留在固件里，接线阶段任意处调用即可触发（勿在 LVGL/UI 任务里跑）。
监控日志 tag：`volc_asr` / `volc_tts` / `volc_selftest`。

## 协议要点（v3 二进制帧）

```
帧 = header(4B) | [seq int32 BE] | [error uint32 BE] | [event int32 BE]
     | [sessionId u32len+bytes] | payload u32len+bytes
header[0]=version<<4|headerSize(4B单位)  header[1]=msgType<<4|flags
header[2]=serialization<<4|compression   header[3]=0
```

- **ASR**：full client request（JSON gzip，seq=1 正序号）→ audio-only
  （PCM gzip，200ms/段，正序号递增；末段 flags=0b0011 且 seq 取负，残余
  不足 200ms 也随末段发出，可为空段）。响应 flags bit1=final；错误帧
  msgType=0xF 带 uint32 错误码。文本取 `result.text`（服务端全量下发）。
- **TTS**：全部客户端帧为 full client request + WithEvent(0b0100)，JSON 不
  压缩，event/sessionId 编入帧头后部；连接级事件（StartConnection=1）无
  sessionId。时序：StartConnection→ConnectionStarted(50)→StartSession(100)
  →SessionStarted(150)→TaskRequest(200)×N→FinishSession(102)→
  [AudioOnlyServer(0xB) PCM 流]→SessionFinished(152)。打断发
  CancelSession(101)，等 SessionCanceled(151)。
- 鉴权：HTTP 升级头 `X-Api-App-Key` / `X-Api-Access-Key` /
  `X-Api-Resource-Id` + `X-Api-Request-Id`（ASR）/ `X-Api-Connect-Id`（TTS），
  TLS 走 esp-tls 证书包（sdkconfig `MBEDTLS_CERTIFICATE_BUNDLE` 承重）。

## 采样率链路

板载 codec（`mhal::audio`，BTAudioCodecDuplex I2S0 slave）固定
**16kHz/16bit/mono** 双向，全链路统一 16k、**零重采样点**：

```
mic 16k ─→ audio_pipeline 采集帧 16k ─→ ASR 上行（请求声明 rate:16000）
TTS 下行（audio_params.sample_rate:16000）─→ 播放队列 16k ─→ speaker 16k
```

参考服务端实现用 24kHz 是它面向的老固件如此；火山 SAUC 与 seed-tts 均
原生支持 16k。若未来换 24k 硬件或产品强制 24k，重采样点应放在管线边界
（采集帧回调后 / FeedPlayback 前），旧固件的 OpusResampler 可从
MetalioClaw5 按需取回；AFE（若将来纳入）同样工作在 16k，槽位兼容。

## 资源占用

- 新增 managed 依赖：`espressif/esp_websocket_client` ^1.2、`espressif/zlib`
  ^1.3（zlib 本已被 esp_lvgl_adapter 间接拉入）。固件增量 ≈ 85KB
  （0x620030 → 0x634d20），app 分区仍余 48%。
- 运行期：每条 WS 连接 1 个任务（6KB 栈）+ 4KB 收发缓冲；管线播放任务
  （4KB 栈，常驻）+ 64KB PSRAM 抖动队列；采集任务（4KB 栈，仅录音期间）；
  gzip 单流 ≈32KB（段级一次性）。
- 回调上下文：ASR/TTS 事件回调运行在 WS 客户端任务，采集 `on_frame`/
  `on_vad` 在采集任务，`on_finished` 在播放任务——一律禁止阻塞/耗时操作
  （UI 更新请转投 LVGL 线程）。

## 已知边界

- ASR 连接中断即报错结束（`disable_auto_reconnect`），由上层决定是否重
  录——识别会话状态在服务端，断线续传无意义。TTS 连接断开后下次
  `speak_begin` 自动重建。
- 同一时刻各自最多一个会话；TTS 上一会话未播完时 `speak_begin` 返回
  `ESP_ERR_INVALID_STATE`（先 `volc_tts_stop()`）。
- 真机联测（实际录音/放音）待主会话安排烧录后验证。
