#include "pi_card_data.h"

#include <cstring>

#include "esp_log.h"

#include "metalio_hal/audio.h"
#include "metalio_hal/backlight.h"
#include "metalio_hal/network.h"
#include "metalio_hal/power.h"

#define TAG "pi_card_data"

namespace pi_card {

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

void DataHub::Seed(Entry* e) {
    if (!e->getter) return;
    HubValue v = e->getter();
    switch (e->type) {
        case HubType::Int:
            lv_subject_set_int(&e->subject, std::get<int>(v));
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
            e->setter(HubValue(value));
            break;
    }
}

void DataHub::RegisterBuiltins() {
    if (inited_) return;
    inited_ = true;

    auto make = [this](const std::string& path, HubType type, std::function<HubValue()> getter,
                       std::function<void(const HubValue&)> setter) {
        auto [it, ok] = entries_.try_emplace(path);
        if (!ok) return;
        Entry& e = it->second;
        e.type = type;
        e.getter = std::move(getter);
        e.setter = std::move(setter);
        if (type == HubType::String) {
            lv_subject_init_string(&e.subject, e.str_buf, e.str_prev, sizeof(e.str_buf), "");
        } else {
            lv_subject_init_int(&e.subject, 0);
        }
    };

    // ---- audio.volume（可读写；纯内存缓存读，SetVolume 内部恒持久化 NVS）----
    make("audio.volume", HubType::Int,
         []() -> HubValue { return mhal::audio::GetVolume(); },
         [](const HubValue& v) { mhal::audio::SetVolume(std::get<int>(v), true); });

    // ---- display.brightness（可读写；持久化）----
    make("display.brightness", HubType::Int,
         []() -> HubValue { return static_cast<int>(mhal::backlight::GetBrightness()); },
         [](const HubValue& v) {
             mhal::backlight::SetBrightness(static_cast<uint8_t>(std::get<int>(v)), true);
         });

    // ---- battery.level（只读；Acquire 时读一次，走阻塞 I2C）----
    make("battery.level", HubType::Int,
         []() -> HubValue {
             int level = 0;
             bool chg = false, dis = false;
             mhal::power::GetBatteryLevel(level, chg, dis);
             return level;
         },
         nullptr);

    // ---- battery.charging（只读）----
    make("battery.charging", HubType::Bool,
         []() -> HubValue {
             int level = 0;
             bool chg = false, dis = false;
             mhal::power::GetBatteryLevel(level, chg, dis);
             return chg;
         },
         nullptr);

    // ---- net.type（只读；wifi / 4g）----
    make("net.type", HubType::String,
         []() -> HubValue {
             return std::string(mhal::network::GetType() == mhal::network::Type::WiFi ? "wifi"
                                                                                      : "4g");
         },
         nullptr);

    // ---- net.rssi（只读；WiFi=dBm，4G=CSQ）----
    make("net.rssi", HubType::Int,
         []() -> HubValue {
             return mhal::network::GetType() == mhal::network::Type::WiFi
                        ? mhal::network::GetWifiRssi()
                        : mhal::network::GetSignalStrength();
         },
         nullptr);

    ESP_LOGI(TAG, "registered %d builtin data paths", static_cast<int>(entries_.size()));
}

}  // namespace pi_card
