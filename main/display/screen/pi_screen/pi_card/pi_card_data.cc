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

bool DataHub::Has(const std::string& path) const { return Find(path) != nullptr; }

bool DataHub::TypeOf(const std::string& path, HubType& out) const {
    const Entry* e = Find(path);
    if (!e) return false;
    out = e->type;
    return true;
}

bool DataHub::Writable(const std::string& path) const {
    const Entry* e = Find(path);
    return e && static_cast<bool>(e->setter);
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
        ESP_LOGW(TAG, "acquire unknown path: %s", path.c_str());
        return nullptr;
    }
    if (e->refcount++ == 0) Seed(e);  // 首个绑定：读一次硬件快照
    return &e->subject;
}

void DataHub::Release(const std::string& path) {
    Entry* e = Find(path);
    if (e && e->refcount > 0) e->refcount--;
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
                       std::function<void(const HubValue&)> setter, int lo, int hi) {
    auto [it, ok] = entries_.try_emplace(path);
    if (!ok) return;
    Entry& e = it->second;
    e.type = type;
    e.getter = std::move(getter);
    e.setter = std::move(setter);
    if (lo <= hi) {
        e.has_range = true;
        e.vmin = lo;
        e.vmax = hi;
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
    Register("audio.volume", HubType::Int,
             []() -> HubValue { return mhal::audio::GetVolume(); },
             [](const HubValue& v) { mhal::audio::SetVolume(std::get<int>(v), true); }, 0, 100);

    // ---- display.brightness（可读写；持久化）----
    // 量程 5–100：与 backlight::Restore 的下限、quick_panel BRT 滑条(min=5) 一致，
    // 绝不让屏幕被拉到全黑（0 只有开机 Restore 才会被抬回 5，运行时不可自救）。
    Register("display.brightness", HubType::Int,
             []() -> HubValue { return static_cast<int>(mhal::backlight::GetBrightness()); },
             [](const HubValue& v) {
                 mhal::backlight::SetBrightness(static_cast<uint8_t>(std::get<int>(v)), true);
             },
             5, 100);

    // ---- battery.level（只读；Acquire 时读一次，走阻塞 I2C）----
    Register("battery.level", HubType::Int,
             []() -> HubValue {
                 int level = 0;
                 bool chg = false, dis = false;
                 mhal::power::GetBatteryLevel(level, chg, dis);
                 return level;
             },
             nullptr, 0, 100);

    // ---- battery.charging（只读）----
    Register("battery.charging", HubType::Bool,
             []() -> HubValue {
                 int level = 0;
                 bool chg = false, dis = false;
                 mhal::power::GetBatteryLevel(level, chg, dis);
                 return chg;
             },
             nullptr);

    // ---- net.type（只读；wifi / 4g）----
    // 红线：网络类型切换在 pi_settings 里是「持久化 NVS + esp_restart」且带二次确认
    // 弹窗（「切换网络通道将重启设备」）。这里刻意只读——绝不能给它加 setter 把重启
    // 类操作变成 LLM 一句话的静默副作用。将来若要放开，必须复用 settings 的确认+重启
    // 路径，而非在 DataHub 里挂 setter。
    Register("net.type", HubType::String,
             []() -> HubValue {
                 bool wifi = mhal::network::GetType() == mhal::network::Type::WiFi;
                 return std::string(wifi ? "wifi" : "4g");
             },
             nullptr);

    // ---- net.rssi（只读；WiFi=dBm，4G=CSQ）----
    Register("net.rssi", HubType::Int,
             []() -> HubValue {
                 bool wifi = mhal::network::GetType() == mhal::network::Type::WiFi;
                 return wifi ? mhal::network::GetWifiRssi() : mhal::network::GetSignalStrength();
             },
             nullptr);

    // ---- net.ssid（只读；当前 WiFi 名，4G 时为空）----
    Register("net.ssid", HubType::String,
             []() -> HubValue { return mhal::network::GetWifiSsid(); }, nullptr);

    // ---- net.connected（只读；是否已联网）----
    Register("net.connected", HubType::Bool,
             []() -> HubValue { return mhal::network::IsConnected(); }, nullptr);

    // ---- ui.theme（可读写；0=深 1=浅；Set 内部立即全 UI 翻转 + NVS 持久化）----
    // 无重启、无破坏性，纯样式开关——可安全交给 LLM（"帮我切浅色"）。pi_theme 是 pi_card
    // 可直接依赖的同层模块，故这里直接内置，无需上层注册。
    Register("ui.theme", HubType::Bool,
             []() -> HubValue { return pi_theme::IsLight(); },
             [](const HubValue& v) { pi_theme::Set(std::get<bool>(v)); });

    ESP_LOGI(TAG, "registered %d builtin data paths", static_cast<int>(entries_.size()));
}

}  // namespace pi_card
