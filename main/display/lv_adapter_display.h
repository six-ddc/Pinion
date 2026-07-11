#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include "display.h"
#include "esp_lv_adapter.h"

class LVAdapterDisplay : public Display {
public:
    LVAdapterDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     const esp_lcd_touch_handle_t touch_handle, int width, int height);
    virtual ~LVAdapterDisplay();

    // No SetEmotion/SetChatMessage override: those routed to chat_screen /
    // digital_people_screen, which don't exist in this pi-only firmware.
    // Display's base-class no-op implementations cover any residual caller
    // (e.g. board/network code calling GetDisplay()->SetStatus(...)).
    virtual void SetStatus(const char* status) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    void SetupUI();
};
