/* sim shim — same public API as components/metalio_hal/include/settings.h,
 * backed by a plain "ns.key=value" text file (./pi_sim_settings.ini, override
 * with PI_SIM_SETTINGS) instead of NVS. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <cstdint>
#include <string>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    void SetInt(const std::string& key, int32_t value);
    bool GetBool(const std::string& key, bool default_value = false);
    void SetBool(const std::string& key, bool value);
    void EraseKey(const std::string& key);
    void EraseAll();

private:
    std::string ns_;
    bool read_write_ = false;
    bool dirty_ = false;
};

#endif
