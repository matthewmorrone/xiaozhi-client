#include "wifi_board.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "display/lvgl_display/lvgl_font.h"
#include "esp_lcd_sh8601.h"
#include "application.h"

LV_FONT_DECLARE(icons_bs_ms_36);
LV_FONT_DECLARE(clock_mono_30);

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "settings.h"

#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <cstring>

#define TAG "WaveshareEsp32c6TouchAMOLED2inch06"
#define DEBUG_RULER 1

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);
        WriteReg(0x93, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x03);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x0A); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const sh8601_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x51, (uint8_t []){0x00}, 1, 10},
    {0x2A, (uint8_t []){0x00,0x16,0x01,0xAF}, 4, 0},
    {0x2B, (uint8_t []){0x00,0x00,0x01,0xF5}, 4, 0},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};

class CustomLcdDisplay : public SpiLcdDisplay {
private:
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* battery_widget_ = nullptr;
    lv_obj_t* battery_body_ = nullptr;
    lv_obj_t* battery_bar_ = nullptr;
    lv_obj_t* battery_nub_ = nullptr;
    DeviceState last_state_ = kDeviceStateUnknown;

    static bool LooksLikeClock(const char* s) {
        return s != nullptr && std::strlen(s) == 5 && s[2] == ':';
    }

    static lv_color_t BatteryColor(int pct, bool charging) {
        if (charging)   return lv_color_hex(0x4263EB); // blue
        if (pct > 50)   return lv_color_hex(0x37B24D); // green
        if (pct > 20)   return lv_color_hex(0xFAB005); // yellow
        return lv_color_hex(0xE03131);                 // red
    }

    static lv_color_t ColorForState(DeviceState s) {
        switch (s) {
            case kDeviceStateStarting:        return lv_color_hex(0x1F3D7A);
            case kDeviceStateWifiConfiguring: return lv_color_hex(0x4263EB);
            case kDeviceStateAudioTesting:    return lv_color_hex(0x15AABF);
            case kDeviceStateActivating:      return lv_color_hex(0xFD7E14);
            case kDeviceStateConnecting:      return lv_color_hex(0xFAB005);
            case kDeviceStateIdle:            return lv_color_hex(0x3F3F3F);
            case kDeviceStateListening:       return lv_color_hex(0xE03131);
            case kDeviceStateSpeaking:        return lv_color_hex(0x37B24D);
            case kDeviceStateUpgrading:       return lv_color_hex(0x9333EA);
            case kDeviceStateUnknown:
            default:                          return lv_color_hex(0x000000);
        }
    }

public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Swap the small-icon font on every registered theme to a Bootstrap +
        // Material Outlined hybrid (batteries from Bootstrap, wifi/mute from
        // Material). Codepoints in the new font are remapped to the same FA
        // codepoints the existing display code already writes via lv_label_set_text.
        auto bs_ms_font = std::make_shared<LvglBuiltInFont>(&icons_bs_ms_36);
        auto& tm = LvglThemeManager::GetInstance();
        for (const char* name : {"light", "dark"}) {
            if (auto* t = tm.GetTheme(name)) {
                static_cast<LvglTheme*>(t)->set_icon_font(bs_ms_font);
            }
        }
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const lv_color_t white = lv_color_hex(0xFFFFFF);

        // Round AMOLED panel: corners are physically masked, so inset top_bar_
        // (network / battery / mute icons) to keep them inside the visible area.
        lv_obj_set_style_pad_left(top_bar_, LV_HOR_RES * 0.2, 0);
        lv_obj_set_style_pad_right(top_bar_, LV_HOR_RES * 0.2, 0);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);

        // Status text (state names, notifications) goes to the bottom.
        lv_obj_align(status_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.1, 0);

        // Chat / boot-info bar moves to the vertical middle of the screen so
        // the welcome message and assistant replies sit in the visible center
        // of the round AMOLED rather than at the bottom.
        if (bottom_bar_ != nullptr) {
            lv_obj_align(bottom_bar_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
        }

        // Hide the centered emoji — state is now communicated by the screen
        // background color.
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }

        // Force foreground elements to white so they read on every state's
        // background color (all states use medium-to-dark hues).
        if (network_label_ != nullptr)      lv_obj_set_style_text_color(network_label_, white, 0);
        if (mute_label_ != nullptr)         lv_obj_set_style_text_color(mute_label_, white, 0);
        if (status_label_ != nullptr)       lv_obj_set_style_text_color(status_label_, white, 0);
        if (notification_label_ != nullptr) lv_obj_set_style_text_color(notification_label_, white, 0);
        if (chat_message_label_ != nullptr) lv_obj_set_style_text_color(chat_message_label_, white, 0);

        // Clock inside top_bar_ between network_label_ (left) and the right-
        // icons cluster (right). Inserted at index 1 so flex centers it.
        clock_label_ = lv_label_create(lv_screen_active());
        lv_label_set_text(clock_label_, "--:--");
        lv_obj_set_style_text_font(clock_label_, &clock_mono_30, 0);
        lv_obj_set_style_text_color(clock_label_, white, 0);
        lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(clock_label_, LV_ALIGN_TOP_MID, 0, lvgl_theme->spacing(2));

        // Custom battery widget: outline body + proportional fill bar + nub.
        // Replaces the FA battery glyph (which is hidden); UpdateStatusBar
        // override drives the fill width and charging animation.
        if (battery_label_ != nullptr) {
            lv_obj_t* right_icons = lv_obj_get_parent(battery_label_);
            lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
            if (right_icons != nullptr) {
                const int kBodyW = 40;
                const int kBodyH = 18;
                const int kNubW  = 3;
                const int kNubH  = 8;

                battery_widget_ = lv_obj_create(right_icons);
                lv_obj_set_size(battery_widget_, kBodyW + kNubW, kBodyH);
                lv_obj_set_style_bg_opa(battery_widget_, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(battery_widget_, 0, 0);
                lv_obj_set_style_pad_all(battery_widget_, 0, 0);
                lv_obj_clear_flag(battery_widget_, LV_OBJ_FLAG_SCROLLABLE);

                battery_body_ = lv_obj_create(battery_widget_);
                lv_obj_set_size(battery_body_, kBodyW, kBodyH);
                lv_obj_align(battery_body_, LV_ALIGN_LEFT_MID, 0, 0);
                lv_obj_set_style_radius(battery_body_, 4, 0);
                lv_obj_set_style_border_width(battery_body_, 2, 0);
                lv_obj_set_style_border_color(battery_body_, white, 0);
                lv_obj_set_style_bg_opa(battery_body_, LV_OPA_TRANSP, 0);
                lv_obj_set_style_pad_all(battery_body_, 2, 0);
                lv_obj_clear_flag(battery_body_, LV_OBJ_FLAG_SCROLLABLE);

                battery_bar_ = lv_obj_create(battery_body_);
                lv_obj_set_size(battery_bar_, 0, LV_PCT(100));
                lv_obj_align(battery_bar_, LV_ALIGN_LEFT_MID, 0, 0);
                lv_obj_set_style_radius(battery_bar_, 2, 0);
                lv_obj_set_style_border_width(battery_bar_, 0, 0);
                lv_obj_set_style_bg_color(battery_bar_, white, 0);
                lv_obj_set_style_pad_all(battery_bar_, 0, 0);
                lv_obj_clear_flag(battery_bar_, LV_OBJ_FLAG_SCROLLABLE);

                battery_nub_ = lv_obj_create(battery_widget_);
                lv_obj_set_size(battery_nub_, kNubW, kNubH);
                lv_obj_align(battery_nub_, LV_ALIGN_RIGHT_MID, 0, 0);
                lv_obj_set_style_radius(battery_nub_, 1, 0);
                lv_obj_set_style_border_width(battery_nub_, 0, 0);
                lv_obj_set_style_bg_color(battery_nub_, white, 0);
                lv_obj_set_style_pad_all(battery_nub_, 0, 0);
                lv_obj_clear_flag(battery_nub_, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_move_to_index(battery_widget_, 0);
            }
        }

        // Apply the initial background color before app_main schedules anything.
        ApplyStateColor(Application::GetInstance().GetDeviceState());

#ifdef DEBUG_RULER
        DrawDebugRuler();
#endif

        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    void ApplyStateColor(DeviceState s) {
        if (last_state_ == s) return;
        last_state_ = s;
        DisplayLockGuard lock(this);
        lv_color_t bg = ColorForState(s);
        lv_obj_t* screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, bg, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        if (container_ != nullptr) {
            lv_obj_set_style_bg_color(container_, bg, 0);
            lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
        }
    }

    // Debug overlay: draws horizontal/vertical center crosshairs + four edge
    // ticks + 50-px gridlines so we can confirm UI alignment on the round
    // panel. Toggle with DEBUG_RULER below.
    void DrawDebugRuler() {
        lv_obj_t* screen = lv_screen_active();
        const lv_color_t guide = lv_color_hex(0xFFFFFF);

        auto thin_rect = [&](int x, int y, int w, int h) {
            lv_obj_t* r = lv_obj_create(screen);
            lv_obj_set_size(r, w, h);
            lv_obj_set_pos(r, x, y);
            lv_obj_set_style_bg_color(r, guide, 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_50, 0);
            lv_obj_set_style_border_width(r, 0, 0);
            lv_obj_set_style_pad_all(r, 0, 0);
            lv_obj_set_style_radius(r, 0, 0);
            lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
        };

        // Crosshair through screen center, with edge ticks placed symmetrically.
        const int cx = LV_HOR_RES / 2;
        const int cy = LV_VER_RES / 2;
        thin_rect(0, cy, LV_HOR_RES, 1);
        thin_rect(cx, 0, 1, LV_VER_RES);
        for (int dx = 50; dx <= cx; dx += 50) {
            thin_rect(cx - dx, 0, 1, 8);
            thin_rect(cx + dx, 0, 1, 8);
            thin_rect(cx - dx, LV_VER_RES - 8, 1, 8);
            thin_rect(cx + dx, LV_VER_RES - 8, 1, 8);
        }
        for (int dy = 50; dy <= cy; dy += 50) {
            thin_rect(0, cy - dy, 8, 1);
            thin_rect(0, cy + dy, 8, 1);
            thin_rect(LV_HOR_RES - 8, cy - dy, 8, 1);
            thin_rect(LV_HOR_RES - 8, cy + dy, 8, 1);
        }
    }

    virtual void UpdateStatusBar(bool update_all) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);

        // 1. Background color follows app state.
        ApplyStateColor(Application::GetInstance().GetDeviceState());

        DisplayLockGuard lock(this);

        // Force-white the bottom status text every tick. Some code paths
        // (theme refresh, message recreate) reset text color to the theme's
        // default, which is black on light theme.
        const lv_color_t white = lv_color_hex(0xFFFFFF);
        if (status_label_ != nullptr)       lv_obj_set_style_text_color(status_label_, white, 0);
        if (notification_label_ != nullptr) lv_obj_set_style_text_color(notification_label_, white, 0);
        if (chat_message_label_ != nullptr) lv_obj_set_style_text_color(chat_message_label_, white, 0);

        // 2. Clock — write directly each tick rather than waiting for the
        // parent's idle-only update. Shows '--:--' until SNTP syncs.
        if (clock_label_ != nullptr) {
            time_t now = time(NULL);
            struct tm* tm = localtime(&now);
            if (tm->tm_year >= 2025 - 1900) {
                char buf[8];
                strftime(buf, sizeof(buf), "%H:%M", tm);
                lv_label_set_text(clock_label_, buf);
            } else {
                lv_label_set_text(clock_label_, "--:--");
            }
        }

        // 3. Battery widget: fill bar width = level%; color tells the story.
        // Charging always shows a full bar in blue; otherwise level dictates
        // both width and color (green/yellow/red).
        if (battery_bar_ != nullptr) {
            int level;
            bool charging, discharging;
            if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
                int pct = level < 0 ? 0 : (level > 100 ? 100 : level);
                lv_obj_set_style_bg_color(battery_bar_, BatteryColor(pct, charging), 0);
                lv_obj_set_width(battery_bar_, LV_PCT(pct));
            }
        }
    }

    // The Assets module refreshes the theme after boot; the parent SetTheme
    // resets container_ bg, top_bar_ bg_opa, and several text colors to theme
    // values. Re-apply our overrides so the state-color UI sticks.
    virtual void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);
        DisplayLockGuard lock(this);
        const lv_color_t white = lv_color_hex(0xFFFFFF);

        if (top_bar_ != nullptr)    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        if (status_bar_ != nullptr) lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        if (bottom_bar_ != nullptr) lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);

        if (network_label_ != nullptr)      lv_obj_set_style_text_color(network_label_, white, 0);
        if (status_label_ != nullptr)       lv_obj_set_style_text_color(status_label_, white, 0);
        if (mute_label_ != nullptr)         lv_obj_set_style_text_color(mute_label_, white, 0);
        if (notification_label_ != nullptr) lv_obj_set_style_text_color(notification_label_, white, 0);
        if (chat_message_label_ != nullptr) lv_obj_set_style_text_color(chat_message_label_, white, 0);
        if (clock_label_ != nullptr)        lv_obj_set_style_text_color(clock_label_, white, 0);

        // Force the state color back on the screen / container.
        last_state_ = kDeviceStateUnknown;
        ApplyStateColor(Application::GetInstance().GetDeviceState());
    }

    // Drop HH:MM writes — the dedicated clock_label_ already shows the time,
    // so the parent's idle-tick SetStatus(time_str) would just be a duplicate.
    // Also: state transitions arrive here via Application::HandleStateChangedEvent,
    // which calls SetStatus() but NOT UpdateStatusBar(). So this is the right
    // hook to refresh the screen background color promptly even for short-lived
    // states like Connecting (~hundreds of ms).
    virtual void SetStatus(const char* status) override {
        if (LooksLikeClock(status)) return;
        ApplyStateColor(Application::GetInstance().GetDeviceState());
        SpiLcdDisplay::SetStatus(status);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32c6TouchAMOLED2inch06 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        // -1, -1 = no auto-sleep, no auto-shutdown. Keeps USB CDC enumerated and
        // the device responsive across long idle periods.
        power_save_timer_ = new PowerSaveTimer(-1, -1, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->OnShutdownRequest([this](){ 
            pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // Long-press: pull a fresh xiaozhi.bin from the dev OTA URL and reboot.
        // URL is persisted in NVS (Settings("ota").url) and falls back to a
        // hardcoded LAN default. To change it without reflashing, write a new
        // value via the self.firmware.set_ota_url MCP tool.
        boot_button_.OnLongPress([this]() {
            Settings settings("ota", false);
            std::string url = settings.GetString("url", "http://192.168.0.62:8123/xiaozhi.bin");
            if (url.empty()) {
                ESP_LOGW(TAG, "OTA long-press: no URL configured");
                return;
            }
            ESP_LOGI(TAG, "OTA long-press: fetching %s", url.c_str());
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                app.UpgradeFirmware(url);
            });
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeSH8601Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
            EXAMPLE_PIN_NUM_LCD_CS,
            nullptr,
            nullptr);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const sh8601_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(sh8601_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x16, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
        mcp_server.AddTool("self.firmware.set_ota_url",
            "Set the URL the device pulls firmware from on long-press of the boot button. "
            "Persisted in NVS so it survives reboots.",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                std::string url = properties["url"].value<std::string>();
                Settings settings("ota", true);
                settings.SetString("url", url);
                return true;
            });
        mcp_server.AddTool("self.firmware.get_ota_url",
            "Return the URL currently configured for boot-button OTA.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                Settings settings("ota", false);
                return settings.GetString("url", "");
            });
    }

public:
    WaveshareEsp32c6TouchAMOLED2inch06() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeAxp2101();
        InitializeSpi();
        InitializeSH8601Display();
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32c6TouchAMOLED2inch06);
