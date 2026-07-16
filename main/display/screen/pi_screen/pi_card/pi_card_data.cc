#include "pi_card_data.h"

#include <cstring>

#include "esp_log.h"

#include "metalio_hal/audio.h"
#include "metalio_hal/backlight.h"
#include "metalio_hal/network.h"
#include "metalio_hal/power.h"

#include "pi_theme.h"  // ui.theme 路径直接接 pi_theme（同 UI 层，无循环依赖）

#define TAG "pi_card_data"

namespace pi_card {

namespace {
// 把 v 钳入声明的有效量程（无量程则原样返回）。集中在此，Seed/Write 共用。
int ClampRange(int v, bool has_range, int lo, int hi) {
    if (!has_range) return v;
    return v < lo ? lo : (v > hi ? hi : v);
}
}  // namespace

DataHub& DataHub::Instance() {
    static DataHub instance;
    return instance;
}

const DataHub::Entry* DataHub::Find(const std::string& path) const {
    auto it = entries_.find(path);
    return it == entries_.end() ? nullptr : &it->second;
}

DataHub::Entry* DataHub::Find(const std::string& path) {
    auto it = entries_.find(path);
    return it == entries_.end() ? nullptr : &it->second;
}

// 动态 provider 匹配：纯函数链（不碰 dyn_entries_），worker 线程安全。providers_ 在
// Create() 期注册后只读，与 entries_ 同一线程契约。
const DataHub::DynProvider* DataHub::MatchProvider(const std::string& path, size_t* out_idx) const {
    for (size_t i = 0; i < providers_.size(); i++) {
        if (providers_[i].match && providers_[i].match(path)) {
            if (out_idx) *out_idx = i;
            return &providers_[i];
        }
    }
    return nullptr;
}

bool DataHub::Has(const std::string& path) const {
    return Find(path) != nullptr || MatchProvider(path, nullptr) != nullptr;
}

bool DataHub::TypeOf(const std::string& path, HubType& out) const {
    const Entry* e = Find(path);
    if (e) {
        out = e->type;
        return true;
    }
    if (MatchProvider(path, nullptr)) {
        out = HubType::String;  // 动态路径恒 String（推送侧已格式化好展示文本）
        return true;
    }
    return false;
}

bool DataHub::Writable(const std::string& path) const {
    const Entry* e = Find(path);
    return e && static_cast<bool>(e->setter);
}

bool DataHub::ReadForWorker(const std::string& path, HubValue& out) const {
    const Entry* e = Find(path);
    if (!e || !e->worker_safe || !e->getter) return false;
    out = e->getter();
    return true;
}

std::string DataHub::WritablePathsJoined() const {
    std::string out;
    for (const auto& [path, e] : entries_) {
        if (!e.setter) continue;
        if (!out.empty()) out += '|';
        out += path;
    }
    return out;
}

bool DataHub::RangeOf(const std::string& path, int& lo, int& hi) const {
    const Entry* e = Find(path);
    if (!e || !e->has_range) return false;
    lo = e->vmin;
    hi = e->vmax;
    return true;
}

void DataHub::Seed(Entry* e) {
    if (!e->getter) return;
    HubValue v = e->getter();
    switch (e->type) {
        case HubType::Int:
            // 硬件读回来的种子也钳一遍——防越界快照污染绑定控件的初值。
            lv_subject_set_int(&e->subject,
                               ClampRange(std::get<int>(v), e->has_range, e->vmin, e->vmax));
            break;
        case HubType::Bool:
            lv_subject_set_int(&e->subject, std::get<bool>(v) ? 1 : 0);
            break;
        case HubType::String:
            lv_subject_copy_string(&e->subject, std::get<std::string>(v).c_str());
            break;
    }
}

lv_subject_t* DataHub::Acquire(const std::string& path) {
    Entry* e = Find(path);
    if (!e) {
        size_t pidx = 0;
        const DynProvider* p = MatchProvider(path, &pidx);
        if (!p) {
            ESP_LOGW(TAG, "acquire unknown path: %s", path.c_str());
            return nullptr;
        }
        auto it = dyn_entries_.find(path);
        if (it == dyn_entries_.end()) {
            if (dyn_entries_.size() >= kDynMax) {
                ESP_LOGW(TAG, "dyn path cap (%d) reached, reject %s", (int)kDynMax, path.c_str());
                return nullptr;  // 渲染器按 unknown path 处理：该控件不绑定，卡片其余照常
            }
            it = dyn_entries_.try_emplace(path).first;
            DynEntry& de = it->second;
            de.provider_idx = pidx;
            lv_subject_init_string(&de.subject, de.str_buf, de.str_prev, sizeof(de.str_buf), "--");
        }
        DynEntry& de = it->second;
        if (de.refcount++ == 0 && p->on_first_acquire) p->on_first_acquire(path);
        return &de.subject;
    }
    if (e->refcount++ == 0) {
        Seed(e);  // 首个绑定：读一次硬件快照
        if (!e->setter) active_live_count_++;
    }
    return &e->subject;
}

void DataHub::Release(const std::string& path) {
    Entry* e = Find(path);
    if (e) {
        if (e->refcount > 0 && --e->refcount == 0 && !e->setter) active_live_count_--;
        return;
    }
    auto it = dyn_entries_.find(path);
    if (it != dyn_entries_.end() && it->second.refcount > 0) {
        if (--it->second.refcount == 0) {
            // entry（含 subject）留着不销毁——见头文件 DynProvider 注释的删除顺序约束；
            // 只通知 provider 停掉背后的数据订阅。
            const DynProvider& p = providers_[it->second.provider_idx];
            if (p.on_last_release) p.on_last_release(path);
        }
    }
}

void DataHub::RegisterDynProvider(const DynProvider& p) {
    if (!p.prefix || !p.match) return;
    for (const auto& q : providers_) {
        if (std::strcmp(q.prefix, p.prefix) == 0) return;  // 幂等
    }
    providers_.push_back(p);
}

const char* DataHub::HintFor(const std::string& path) const {
    for (const auto& p : providers_) {
        if (path.rfind(p.prefix, 0) == 0 && !p.match(path)) return p.hint;
    }
    return nullptr;
}

void DataHub::Push(const std::string& path, const char* value) {
    auto it = dyn_entries_.find(path);
    if (it == dyn_entries_.end()) return;
    lv_subject_copy_string(&it->second.subject, value ? value : "--");
}

void DataHub::Write(const std::string& path, int value) {
    Entry* e = Find(path);
    if (!e || !e->setter) return;
    switch (e->type) {
        case HubType::Bool:
            e->setter(HubValue(value != 0));
            break;
        default:
            // 收口有效量程后再落硬件：set 动作的显式越界值、滑条越界拖拽都在此兜住，
            // 保证「设成 -5 反成最亮」「音量 999 灌进编解码器」这类越界一律不发生。
            e->setter(HubValue(ClampRange(value, e->has_range, e->vmin, e->vmax)));
            break;
    }
}

// lo>hi 表示无量程（如 net.rssi）。Int 可写路径务必给量程，让写入与绑定控件都被收口到
// 硬件真实区间。幂等：路径已存在则忽略。
void DataHub::Register(const std::string& path, HubType type, std::function<HubValue()> getter,
                       std::function<void(const HubValue&)> setter, WorkerRead worker_read, int lo,
                       int hi, bool keep_history) {
    auto [it, ok] = entries_.try_emplace(path);
    if (!ok) return;
    Entry& e = it->second;
    e.type = type;
    e.getter = std::move(getter);
    e.setter = std::move(setter);
    e.worker_safe = worker_read == WorkerRead::Safe;
    if (lo <= hi) {
        e.has_range = true;
        e.vmin = lo;
        e.vmax = hi;
    }
    e.keep_history = keep_history;
    if (keep_history) {
        e.hist.reserve(kHistMax);
        history_count_++;
    }
    if (type == HubType::String) {
        lv_subject_init_string(&e.subject, e.str_buf, e.str_prev, sizeof(e.str_buf), "");
    } else {
        lv_subject_init_int(&e.subject, 0);
    }
}

void DataHub::RegisterBuiltins() {
    if (inited_) return;
    inited_ = true;

    // ---- audio.volume（可读写；纯内存缓存读，SetVolume 内部恒持久化 NVS）----
    // 量程 0–100：SetOutputVolume 本身不钳，越界值会直灌编解码器，故在此收口。
    // worker 可读：GetVolume 是 audio_codec 的纯 int 成员读，无内部状态。
    Register("audio.volume", HubType::Int,
             []() -> HubValue { return mhal::audio::GetVolume(); },
             [](const HubValue& v) { mhal::audio::SetVolume(std::get<int>(v), true); },
             WorkerRead::Safe, 0, 100);

    // ---- display.brightness（可读写；持久化）----
    // 量程 5–100：与 backlight::Restore 的下限、quick_panel BRT 滑条(min=5) 一致，
    // 绝不让屏幕被拉到全黑（0 只有开机 Restore 才会被抬回 5，运行时不可自救）。
    Register("display.brightness", HubType::Int,
             []() -> HubValue { return static_cast<int>(mhal::backlight::GetBrightness()); },
             [](const HubValue& v) {
                 mhal::backlight::SetBrightness(static_cast<uint8_t>(std::get<int>(v)), true);
             },
             WorkerRead::Safe, 5, 100);

    // ---- battery.level（只读；读 sysmon 1Hz 发布的原子快照，非阻塞、无 I2C、
    // 与 charging 共享同一次采样）----
    // worker 可读：GetBatterySnapshot 只做一次 atomic load + 解包，无 I2C、无滤波器改写。
    // keep_history=true（Phase3）：待机常驻卡的 chart 要显示"过去"，只在绑定时记会让
    // 新建的图表没有历史——故 1Hz tick 总是记，不依赖是否有活跃绑定。
    Register("battery.level", HubType::Int,
             []() -> HubValue {
                 int level = 0;
                 bool chg = false, dis = false;
                 mhal::power::GetBatterySnapshot(level, chg, dis);
                 return level;
             },
             nullptr, WorkerRead::Safe, 0, 100, /*keep_history=*/true);

    // ---- battery.charging（只读；同 battery.level 读原子快照）----
    // worker 可读：同 battery.level。
    Register("battery.charging", HubType::Bool,
             []() -> HubValue {
                 int level = 0;
                 bool chg = false, dis = false;
                 mhal::power::GetBatterySnapshot(level, chg, dis);
                 return chg;
             },
             nullptr, WorkerRead::Safe);

    // ---- net.type（只读；wifi / 4g）----
    // 红线：网络类型切换在 pi_settings 里是「持久化 NVS + esp_restart」且带二次确认
    // 弹窗（「切换网络通道将重启设备」）。这里刻意只读——绝不能给它加 setter 把重启
    // 类操作变成 LLM 一句话的静默副作用。将来若要放开，必须复用 settings 的确认+重启
    // 路径，而非在 DataHub 里挂 setter。
    // worker 可读：CachedType() 读函数内 static，Create 期就已定型。
    Register("net.type", HubType::String,
             []() -> HubValue {
                 bool wifi = mhal::network::GetType() == mhal::network::Type::WiFi;
                 return std::string(wifi ? "wifi" : "4g");
             },
             nullptr, WorkerRead::Safe);

    // ---- net.rssi（只读；WiFi=dBm，4G=CSQ）----
    // worker 可读：WiFi 分支 GetWifiRssi 已改直调 esp_wifi_sta_get_ap_info 并检查返回码，
    // 无 ESP_ERROR_CHECK abort、无 TOCTOU；4G 分支 GetSignalStrength 读原子 cached_csq_。
    Register("net.rssi", HubType::Int,
             []() -> HubValue {
                 bool wifi = mhal::network::GetType() == mhal::network::Type::WiFi;
                 return wifi ? mhal::network::GetWifiRssi() : mhal::network::GetSignalStrength();
             },
             nullptr, WorkerRead::Safe, 0, -1, /*keep_history=*/true);

    // ---- net.ssid（只读；当前 WiFi 名，4G 时为空）----
    // worker 可读：GetWifiSsid 返回 mutex 保护的门面缓存副本。
    Register("net.ssid", HubType::String,
             []() -> HubValue { return mhal::network::GetWifiSsid(); }, nullptr,
             WorkerRead::Safe);

    // ---- net.connected（只读；是否已联网）----
    // worker 可读：WiFi 走 xEventGroupGetBits（FreeRTOS 线程安全），4G 走 std::atomic<bool>。
    Register("net.connected", HubType::Bool,
             []() -> HubValue { return mhal::network::IsConnected(); }, nullptr, WorkerRead::Safe);

    // ---- ui.theme（可读写；0=深 1=浅；Set 内部立即全 UI 翻转 + NVS 持久化）----
    // 无重启、无破坏性，纯样式开关——可安全交给 LLM（"帮我切浅色"）。pi_theme 是 pi_card
    // 可直接依赖的同层模块，故这里直接内置，无需上层注册。
    // worker 可读：IsLight() 只读一个全局 bool（NVS 只在 pi_theme::Init 读一次）。碰 LVGL 的是
    // Set()（lv_obj_report_style_change），与 getter 无关——而 setter 只会从 LVGL 线程被调。
    Register("ui.theme", HubType::Bool,
             []() -> HubValue { return pi_theme::IsLight(); },
             [](const HubValue& v) { pi_theme::Set(std::get<bool>(v)); }, WorkerRead::Safe);

    ESP_LOGI(TAG, "registered %d builtin data paths", static_cast<int>(entries_.size()));
}

std::vector<DataHub::PathMeta> DataHub::ListPaths() const {
    std::vector<PathMeta> out;
    out.reserve(entries_.size());
    for (const auto& [path, e] : entries_) {
        PathMeta m;
        m.path = path;
        m.type = e.type;
        m.writable = static_cast<bool>(e.setter);
        m.worker_safe = e.worker_safe;
        m.has_range = e.has_range;
        m.vmin = e.vmin;
        m.vmax = e.vmax;
        m.has_history = e.keep_history;
        out.push_back(std::move(m));
    }
    return out;
}

void DataHub::PublishLive() {
    if (active_live_count_ == 0 && history_count_ == 0) return;  // 无活跃只读绑定亦无历史路径，早退
    for (auto& [path, e] : entries_) {
        if (e.refcount > 0 && !e.setter && e.getter) {
            Seed(&e);
        }
        if (e.keep_history && e.getter) {
            HubValue v = e.getter();
            int iv = 0;
            if (const auto* i = std::get_if<int>(&v)) {
                iv = *i;
            } else if (const auto* b = std::get_if<bool>(&v)) {
                iv = *b ? 1 : 0;
            } else {
                continue;  // String 路径不支持历史（chart 只画数值）
            }
            iv = ClampRange(iv, e.has_range, e.vmin, e.vmax);
            if (e.hist.size() >= kHistMax) e.hist.erase(e.hist.begin());
            e.hist.push_back(static_cast<int16_t>(iv));
            for (const auto& [id, sink] : sinks_) {
                if (sink.path == path && sink.cb) sink.cb(iv);
            }
        }
    }
}

void DataHub::StartLiveRefresh() {
    static lv_timer_t* timer = nullptr;
    if (timer != nullptr) return;  // 幂等：进程级只建一次
    timer = lv_timer_create(
        [](lv_timer_t*) { DataHub::Instance().PublishLive(); }, 1000, nullptr);
}

bool DataHub::HasHistory(const std::string& path) const {
    const Entry* e = Find(path);
    return e && e->keep_history;
}

void DataHub::HistorySnapshot(const std::string& path, std::vector<int>& out) const {
    out.clear();
    const Entry* e = Find(path);
    if (!e) return;
    out.reserve(e->hist.size());
    for (int16_t v : e->hist) out.push_back(v);
}

int DataHub::AddHistorySink(const std::string& path, std::function<void(int)> cb) {
    if (!Find(path)) return -1;
    int id = next_sink_id_++;
    sinks_[id] = HistSink{path, std::move(cb)};
    return id;
}

void DataHub::RemoveHistorySink(int handle) { sinks_.erase(handle); }

}  // namespace pi_card
