#pragma once

// ---------------------------------------------------------------------------
// pi_card::CommandRegistry —— invoke 命令注册表（Phase3）
//
// LLM 下发的 {do:'invoke',cmd:'…'} 需要真正执行一个「数据路径做不到」的设备动作
// （重连网络/蓝牙、切换网络通道、新建会话……）。与 DataHub 同款线程契约：
//   * RegisterBuiltins/Register 只在启动期（LVGL 线程，Create 期）调用一次；
//     之后 entries_ 只读不改 —— Has/LevelOf/ListCommands 对 agent worker 线程安全。
//   * Invoke 在 LVGL 线程执行（drain tick 分发 action 时）：Safe 直接调 exec()；
//     Confirm 转给 pi_screen 通过 SetConfirmHook 注入的通用固件确认 sheet ——
//     取消=无副作用，用户点确认才真正 exec()，绝不能被 LLM 绕过。
//
// 命令三级：safe（直接执行）/ confirm（弹固件确认）/ forbidden（干脆不注册，
// 从任何清单/文案里消失 —— 如 power.off、factory.reset 一律不进这张表）。
// ---------------------------------------------------------------------------

#include <functional>
#include <string>
#include <vector>

namespace pi_card {

enum class CmdLevel { Safe, Confirm };

struct CmdMeta {
    std::string name;
    std::string desc;
    CmdLevel level;
};

// 固件确认弹窗回调：(title, body, confirm_label, on_confirm)。由 pi_screen 注入，
// 内部真正显示的是一张通用参数化确认 sheet（invoke-confirm 与 pin ✕ 手势共用）。
using ConfirmHook =
    std::function<void(const std::string&, const std::string&, const std::string&, std::function<void()>)>;

class CommandRegistry {
 public:
    static CommandRegistry& Instance();

    // 注册纯 mhal/Settings 命令（net.reconnect / bt.reconnect / net.switch_type）。
    // 幂等：重复调用只在首次生效。LVGL 线程，Create 期。
    void RegisterBuiltins();

    // 注册一条命令。confirm_title/body/label 仅 Confirm 级需要（Safe 级可留空）。
    void Register(const std::string& name, const std::string& desc, CmdLevel level,
                  std::function<void()> exec, const std::string& confirm_title = "",
                  const std::string& confirm_body = "", const std::string& confirm_label = "");

    // 元数据查询（worker 线程安全：结构在 Register 后不再变）。
    bool Has(const std::string& name) const;
    bool LevelOf(const std::string& name, CmdLevel& out) const;
    std::vector<CmdMeta> ListCommands() const;

    // 真正执行（LVGL 线程）：Safe 直接 exec()；Confirm 调 s_confirm_hook 弹固件确认。
    // 未注册 no-op（校验器已在 worker 线程挡过 unknown cmd）。
    void Invoke(const std::string& name);

    void SetConfirmHook(ConfirmHook hook);

    // 直接弹固件确认 sheet（不经过命令表）：pin 卡的屏上 ✕ 角标复用同一条通道
    // （invoke-confirm 与 unpin 手势共用一张 sheet，见 spec 决策摘要）。无 hook 时 no-op。
    void ShowConfirm(const std::string& title, const std::string& body, const std::string& label,
                     std::function<void()> on_confirm);

 private:
    CommandRegistry() = default;
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    struct Entry {
        std::string desc;
        CmdLevel level = CmdLevel::Safe;
        std::function<void()> exec;
        std::string confirm_title;
        std::string confirm_body;
        std::string confirm_label;
    };

    std::vector<std::pair<std::string, Entry>> entries_;  // 插入序，供 ListCommands 稳定输出
    ConfirmHook confirm_hook_;
    bool inited_ = false;

    const Entry* Find(const std::string& name) const;
};

// 遍历 ListCommands() 拼 "safe: … / confirm(prompts user): …"，DESC 与 system prompt
// 单一真相（类比 BuildPathsClause）。full 保留供未来 DESC/prompt 措辞分叉，当前两处一致。
std::string BuildCommandsClause(bool full);

}  // namespace pi_card
