#include "pi_card_cmd.h"

#include "esp_log.h"

#include "metalio_hal/bluetooth.h"
#include "metalio_hal/gps.h"
#include "metalio_hal/motor.h"
#include "metalio_hal/network.h"
#include "settings.h"

#define TAG "pi_card_cmd"

namespace pi_card {

CommandRegistry& CommandRegistry::Instance() {
    static CommandRegistry instance;
    return instance;
}

const CommandRegistry::Entry* CommandRegistry::Find(const std::string& name) const {
    for (const auto& [n, e] : entries_) {
        if (n == name) return &e;
    }
    return nullptr;
}

void CommandRegistry::Register(const std::string& name, const std::string& desc, CmdLevel level,
                               std::function<void()> exec, const std::string& confirm_title,
                               const std::string& confirm_body, const std::string& confirm_label) {
    if (Find(name) != nullptr) return;  // 幂等：已注册则忽略
    Entry e;
    e.desc = desc;
    e.level = level;
    e.exec = std::move(exec);
    e.confirm_title = confirm_title;
    e.confirm_body = confirm_body;
    e.confirm_label = confirm_label;
    entries_.emplace_back(name, std::move(e));
}

bool CommandRegistry::Has(const std::string& name) const { return Find(name) != nullptr; }

bool CommandRegistry::LevelOf(const std::string& name, CmdLevel& out) const {
    const Entry* e = Find(name);
    if (!e) return false;
    out = e->level;
    return true;
}

std::vector<CmdMeta> CommandRegistry::ListCommands() const {
    std::vector<CmdMeta> out;
    out.reserve(entries_.size());
    for (const auto& [name, e] : entries_) {
        out.push_back(CmdMeta{name, e.desc, e.level});
    }
    return out;
}

void CommandRegistry::Invoke(const std::string& name) {
    const Entry* e = Find(name);
    if (!e) {
        ESP_LOGW(TAG, "invoke: unknown cmd '%s' (worker 校验器本应挡住)", name.c_str());
        return;
    }
    if (e->level == CmdLevel::Safe) {
        if (e->exec) e->exec();
        return;
    }
    // Confirm 级：绝不直接执行——转给固件确认 sheet，取消=无副作用。
    if (!confirm_hook_) {
        ESP_LOGW(TAG, "invoke: confirm cmd '%s' but no confirm hook wired", name.c_str());
        return;
    }
    std::function<void()> exec = e->exec;  // 拷一份，闭包捕获，Entry 可能在确认期间不变但求稳
    confirm_hook_(e->confirm_title, e->confirm_body, e->confirm_label, exec);
}

void CommandRegistry::SetConfirmHook(ConfirmHook hook) { confirm_hook_ = std::move(hook); }

void CommandRegistry::ShowConfirm(const std::string& title, const std::string& body,
                                  const std::string& label, std::function<void()> on_confirm) {
    if (confirm_hook_) confirm_hook_(title, body, label, std::move(on_confirm));
}

// ---- 初始命令清单（编排者裁决：无 power.off） ----
void CommandRegistry::RegisterBuiltins() {
    if (inited_) return;
    inited_ = true;

    // net.reconnect：幂等重连，最坏 no-op（Safe）。
    Register("net.reconnect", "reconnect network", CmdLevel::Safe,
             []() { mhal::network::StartAsync(); });

    // device.vibrate：短促振动反馈，无副作用（Safe）。
    Register("device.vibrate", "buzz the vibration motor briefly", CmdLevel::Safe,
             []() { mhal::motor::Buzz(200); });

    // bt.reconnect：重连上次配对成功的音箱；无记录/失败均 no-op（Safe，可逆）。
    Register("bt.reconnect", "reconnect last-paired BT speaker", CmdLevel::Safe, []() {
        Settings bt("bt", false);
        std::string addr = bt.GetString("last_addr", "");
        if (!addr.empty()) mhal::bt::Connect(addr);
    });

    // net.switch_type：持久化+重启，破坏性——必须走固件确认（安全红线）。
    Register("net.switch_type", "switch WiFi/4G, reboots", CmdLevel::Confirm,
             []() { mhal::network::SwitchType(); }, "切换网络通道将重启设备", "切换后设备会重启，期间无法使用",
             "切换并重启");

    // gps.enable：启用 GPS（占用 UART0 + 给模块上电）。有真机风险（UART0 可能是控制台、
    // 模块是否贴料未定，见 gps.h），故走固件确认而非让 LLM 直接开。
    Register("gps.enable", "power on GPS module (uses UART0)", CmdLevel::Confirm,
             []() { mhal::gps::Enable(true); }, "启用 GPS 模块？", "将占用 UART0 并给模块上电；若该口用作日志会冲突",
             "启用 GPS");
    // gps.disable：停解析 + 断电，可逆（Safe）。
    Register("gps.disable", "power off GPS module", CmdLevel::Safe,
             []() { mhal::gps::Enable(false); });

    ESP_LOGI(TAG, "registered %d builtin commands", static_cast<int>(entries_.size()));
}

// full=true（DESC）：名+简述，供 LLM 挑选命令语义；full=false（system prompt）：只列名，
// 措辞已在 DEVICE COMMANDS 段落里讲过一遍，无需重复整段描述（省字节预算）。
std::string BuildCommandsClause(bool full) {
    CommandRegistry::Instance().RegisterBuiltins();
    std::string safe, confirm;
    for (const auto& m : CommandRegistry::Instance().ListCommands()) {
        std::string& bucket = (m.level == CmdLevel::Safe) ? safe : confirm;
        if (!bucket.empty()) bucket += ", ";
        bucket += full ? (m.name + "(" + m.desc + ")") : m.name;
    }
    return "safe: " + safe + "; confirm(prompts user): " + confirm;
}

}  // namespace pi_card
