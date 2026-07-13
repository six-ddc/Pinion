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
// ---------------------------------------------------------------------------

#include <functional>
#include <map>
#include <string>
#include <variant>

#include "lvgl.h"

namespace pi_card {

using HubValue = std::variant<int, bool, std::string>;

enum class HubType { Int, Bool, String };

class DataHub {
 public:
    static DataHub& Instance();

    // 注册 v1 内置路径（audio.volume / display.brightness / battery.* / net.*）。
    // 幂等：重复调用只在首次生效。LVGL 线程。
    void RegisterBuiltins();

    // 元数据查询（校验器用；任意线程安全，结构在 Register 后稳定）。
    bool Has(const std::string& path) const;
    bool TypeOf(const std::string& path, HubType& out) const;
    bool Writable(const std::string& path) const;  // 有 setter 即可双向 bind

    // 渲染器绑定时调用（LVGL 线程）。refcount 0→1 时读一次 getter 种子写 subject。
    // 找不到返回 nullptr。
    lv_subject_t* Acquire(const std::string& path);

    // 卡片销毁时调用（LVGL 线程，root 的 LV_EVENT_DELETE 里）。
    void Release(const std::string& path);

    // 用户交互回写硬件（LVGL 线程）。int/bool 可写路径有效；调用 setter 写硬件。
    // subject 由控件自身的双向 bind 负责同步，这里不再重复写 subject。
    void Write(const std::string& path, int value);

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
    };

    const Entry* Find(const std::string& path) const;
    Entry* Find(const std::string& path);
    void Seed(Entry* e);  // 读 getter 写 subject（LVGL 线程）

    std::map<std::string, Entry> entries_;
    bool inited_ = false;
};

}  // namespace pi_card
