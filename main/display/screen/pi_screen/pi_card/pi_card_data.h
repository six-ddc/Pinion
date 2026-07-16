#pragma once

// ---------------------------------------------------------------------------
// pi_card::DataHub —— 声明式 UI 卡片的数据注册中心（Claw6 精简版）
//
// 用点路径（"audio.volume" / "battery.level" / "net.rssi" …）注册一批设备侧
// 数据源，每个路径背后是一个常驻的 lv_subject_t。卡片 schema 里 widget 声明
// `"bind":"audio.volume"` 时，渲染器用 LVGL 内建 observer 把控件挂到对应
// subject 上，实现「外部改值 → 屏上回显」「同卡多控件共享一值」。
//
// Claw4 → Claw6 的关键简化（不再依赖 Application::Schedule / 后台采样任务）：
//   * 只做「Acquire 时 seed 一次 + 控件交互回写硬件」。没有周期轮询：电量/
//     RSSI 这类值在卡片渲染时读一次快照即可（真机上 sysmon 已在 1Hz 采电量，
//     此处再起采样会与其无锁滤波竞争——见 docs 调研；live 刷新留待后续）。
//   * 硬件回写不走 setter-observer：LVGL 里「程序化改 subject」不会触发控件的
//     VALUE_CHANGED，只有用户交互才触发；渲染器据此把回写挂在控件事件上，
//     天然无回环，无需 publishing 标志。DataHub 只暴露 Write() 供其调用。
//
// 线程契约：
//   * Register/RegisterBuiltins 在 Create()（LVGL 线程）一次性调用；之后
//     entries_ 结构只读不改 → subject 指针永生。
//   * Acquire/Release/Write 由渲染器与控件事件在 LVGL 线程调用（持显示锁的
//     drain tick 或事件回调）。
//   * Has/TypeOf/Writable 是纯元数据查询（只读稳定的 map），校验器在 agent
//     worker 线程调用它们是安全的（结构在 Register 后不再变）。
//   * ReadForWorker 由 ui_render 工具在 agent worker 线程调，直接跑 getter 读快照。
//     依据：**没有任何 getter 碰 lv_***——LVGL 线程对这些 mhal 状态根本不是一个串行化
//     域（显示锁没保护过它们的内部状态），所以「从 LVGL 线程调」没买到安全性，「改到
//     worker 调」也没弄丢安全性。battery.*/net.rssi/net.ssid 背后曾经是既有竞争
//     （无锁滤波器 / 未连接时 ESP_ERROR_CHECK 直接 abort / 无锁 std::string 拷贝），
//     现已在 mhal 后端根治：电量走加锁采样 + 原子快照，rssi 改直调
//     esp_wifi_sta_get_ap_info 做 err-check，ssid 改读 mutex 保护的门面缓存副本——
//     getter 均已非阻塞、线程安全，故标 Safe。
//     * 活性刷新：DataHub 持有一个进程级 1Hz lv_timer（LVGL 线程），对 refcount>0
//       且只读(setter==null)的路径重跑 getter → Seed(subject)，绑定控件经既有
//       observer 自动动画，无需渲染器/工具层介入。
// ---------------------------------------------------------------------------

#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "lvgl.h"

namespace pi_card {

using HubValue = std::variant<int, bool, std::string>;

enum class HubType { Int, Bool, String };

// 该路径的 getter 能否在 agent worker 线程直接调（ui_render 同步读快照用）。Register 强制
// 显式表态、不给默认值：新增路径的人必须当场想清楚这件事，而不是默认蒙混过关。
enum class WorkerRead { Unsafe, Safe };

class DataHub {
 public:
    static DataHub& Instance();

    // 注册 v1 内置路径（audio.volume / display.brightness / battery.* / net.*）。
    // 幂等：重复调用只在首次生效。LVGL 线程。
    void RegisterBuiltins();

    // 运行时注册一个自定义数据路径。UI 侧路径（主题/TTS/息屏等）由 pi_screen 在
    // Create() 时注册——DataHub 不能反向依赖 pi_screen，故 getter/setter 由上层传入。
    // 幂等：路径已存在则忽略。LVGL 线程，须在 agent 首次跑工具（校验器查 bind 路径）
    // 之前完成。lo>hi 表示无量程；setter 为空则只读。keep_history=true（Phase3）：该路径
    // 每 1Hz tick 总是记入环形历史（不依赖是否有活跃绑定），供 chart 控件取用。
    void Register(const std::string& path, HubType type, std::function<HubValue()> getter,
                  std::function<void(const HubValue&)> setter, WorkerRead worker_read, int lo = 0,
                  int hi = -1, bool keep_history = false);

    // 元数据查询（校验器用；任意线程安全，结构在 Register 后稳定）。
    bool Has(const std::string& path) const;
    bool TypeOf(const std::string& path, HubType& out) const;
    bool Writable(const std::string& path) const;  // 有 setter 即可双向 bind

    // 当前所有可写路径（有 setter）拼成 "a|b|c"。set 校验失败时回给 LLM 的报错提示用
    // 它动态列全——含 pi_screen 运行时注入的 speech.tts / display.sleep_s，绝不写死过时。
    std::string WritablePathsJoined() const;

    // agent worker 线程读当前值快照（ui_render 把它塞进返回值 —— 模型没有读取工具，渲染一张
    // 卡就是它读设备的唯一途径）。仅 WorkerRead::Safe 的路径返回 true；Unsafe 一律 false，
    // 宁可 state 里少一项，也不把既有竞争扩散到新调用点。
    bool ReadForWorker(const std::string& path, HubValue& out) const;

    // 有效值域（Int 路径）。返回 true 时 [lo,hi] 为该路径的合法量程；渲染器据此
    // 收口绑定滑条/条的量程，DataHub::Write 据此钳制写入——保证「不管 LLM 或用户
    // 拖到什么值，落到硬件的永远在量程内」（如亮度下限 5%，音量 0–100）。
    bool RangeOf(const std::string& path, int& lo, int& hi) const;

    // 渲染器绑定时调用（LVGL 线程）。refcount 0→1 时读一次 getter 种子写 subject。
    // 找不到返回 nullptr。
    lv_subject_t* Acquire(const std::string& path);

    // 卡片销毁时调用（LVGL 线程，root 的 LV_EVENT_DELETE 里）。
    void Release(const std::string& path);

    // 用户交互回写硬件（LVGL 线程）。int/bool 可写路径有效；调用 setter 写硬件。
    // subject 由控件自身的双向 bind 负责同步，这里不再重复写 subject。
    void Write(const std::string& path, int value);

    struct PathMeta {
        std::string path;
        HubType type;
        bool writable;      // 有 setter
        bool worker_safe;
        bool has_range;
        int vmin, vmax;
        bool has_history;   // Phase3：keep_history 声明（chart bind_history 的可选路径清单用）
    };
    // 快照式列出全部注册路径的元数据。线程契约同 Has/TypeOf：所有 Register 完成后
    // （即 PiScreen::Create 返回后）任意线程安全——只读 Register 期定型的不可变字段，
    // 绝不读 subject/refcount（那是 LVGL 线程可变态）。
    std::vector<PathMeta> ListPaths() const;

    // 活性刷新：LVGL 线程 1Hz 重读只读(setter==null)且 refcount>0 路径的 getter 写回
    // subject，绑定控件经 observer 自动更新。StartLiveRefresh 幂等建进程级 lv_timer。
    void StartLiveRefresh();

    // ---- Phase3：chart 历史缓冲（worker 安全元数据 + LVGL 线程读写）----
    // 该路径是否声明了 keep_history（worker 线程安全：Register 后只读）。chart 校验用。
    bool HasHistory(const std::string& path) const;
    // 种子快照：把当前环形缓冲（旧→新顺序）拷进 out。LVGL 线程（chart 渲染时取用）。
    void HistorySnapshot(const std::string& path, std::vector<int>& out) const;
    // 订阅该路径此后每个新样本；返回 handle（RemoveHistorySink 用）。找不到路径返回 -1。
    // LVGL 线程独占（sinks_ 只在 LVGL 线程读写，无需锁）。
    int AddHistorySink(const std::string& path, std::function<void(int)> cb);
    void RemoveHistorySink(int handle);

 private:
    DataHub() = default;
    DataHub(const DataHub&) = delete;
    DataHub& operator=(const DataHub&) = delete;

    struct Entry {
        HubType type = HubType::Int;
        std::function<HubValue()> getter;             // Acquire 首次读种子
        std::function<void(const HubValue&)> setter;  // 空 = 只读
        lv_subject_t subject{};
        char str_buf[64] = {0};
        char str_prev[64] = {0};
        int refcount = 0;
        bool worker_safe = false;  // getter 可在 agent worker 线程直接调（见 WorkerRead）
        bool has_range = false;    // Int 路径是否声明了有效量程
        int vmin = 0;            // has_range 时的下限（含）
        int vmax = 0;            // has_range 时的上限（含）
        bool keep_history = false;      // Phase3：1Hz tick 是否总是记入 hist
        std::vector<int16_t> hist;      // 环形历史（下标 0 最旧），上限 kHistMax
    };

    const Entry* Find(const std::string& path) const;
    Entry* Find(const std::string& path);
    void Seed(Entry* e);  // 读 getter 写 subject（LVGL 线程）
    void PublishLive();   // 1Hz 定时器回调：只读且有绑定的路径重跑 getter → Seed

    static constexpr size_t kHistMax = 120;

    std::map<std::string, Entry> entries_;
    bool inited_ = false;
    int active_live_count_ = 0;  // 当前 refcount>0 && !setter 的路径数，==0 时 PublishLive 早退
    int history_count_ = 0;      // 当前 keep_history 路径数，两者都为 0 时 PublishLive 早退

    // ---- Phase3：chart 的历史订阅（LVGL 线程独占，无需锁）----
    struct HistSink {
        std::string path;
        std::function<void(int)> cb;
    };
    std::map<int, HistSink> sinks_;
    int next_sink_id_ = 1;
};

}  // namespace pi_card
