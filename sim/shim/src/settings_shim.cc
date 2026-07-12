// sim shim — Settings on a "ns.key=value" text file instead of NVS.
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

namespace {

std::mutex g_mu;
std::map<std::string, std::string> g_store;
bool g_loaded = false;

const char* StorePath() {
    const char* p = std::getenv("PI_SIM_SETTINGS");
    return (p != nullptr && p[0] != '\0') ? p : "pi_sim_settings.ini";
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(StorePath());
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        g_store[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void SaveLocked() {
    std::ofstream out(StorePath(), std::ios::trunc);
    for (const auto& kv : g_store) out << kv.first << '=' << kv.second << '\n';
}

}  // namespace

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {}

Settings::~Settings() = default;

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    std::lock_guard<std::mutex> lk(g_mu);
    LoadLocked();
    auto it = g_store.find(ns_ + "." + key);
    return it != g_store.end() ? it->second : default_value;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(g_mu);
    LoadLocked();
    g_store[ns_ + "." + key] = value;
    SaveLocked();
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    std::string s = GetString(key, "");
    if (s.empty()) return default_value;
    return static_cast<int32_t>(std::strtol(s.c_str(), nullptr, 10));
}

void Settings::SetInt(const std::string& key, int32_t value) {
    SetString(key, std::to_string(value));
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    std::string s = GetString(key, "");
    if (s.empty()) return default_value;
    return s == "1" || s == "true";
}

void Settings::SetBool(const std::string& key, bool value) { SetString(key, value ? "1" : "0"); }

void Settings::EraseKey(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_mu);
    LoadLocked();
    g_store.erase(ns_ + "." + key);
    SaveLocked();
}

void Settings::EraseAll() {
    std::lock_guard<std::mutex> lk(g_mu);
    LoadLocked();
    for (auto it = g_store.begin(); it != g_store.end();) {
        if (it->first.rfind(ns_ + ".", 0) == 0) {
            it = g_store.erase(it);
        } else {
            ++it;
        }
    }
    SaveLocked();
}
