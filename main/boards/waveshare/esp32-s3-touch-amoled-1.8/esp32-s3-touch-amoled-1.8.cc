#include "wifi_board.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "display/lvgl_display/lvgl_font.h"
#include "esp_lcd_sh8601.h"
#include "wifi_manager.h"
#include <functional>

LV_FONT_DECLARE(icons_bs_ms_36);
LV_FONT_DECLARE(clock_mono_30);

#include "codecs/es8311_audio_codec.h"
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
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <cstring>

#define TAG "WaveshareEsp32s3TouchAMOLED1inch8"
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

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);
    
        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA

        // Enable PWRON-key IRQs (short + long). The bottom power button on
        // this board is wired through the PMIC, not a GPIO — we read it
        // by polling INTSTS2.
        // INTEN2 (0x41) bit 2 = PKEY_LONG, bit 3 = PKEY_SHORT.
        WriteReg(0x41, 0x0C);
        // Clear any stale events.
        WriteReg(0x49, 0xFF);
    }

    // Poll PWRON-key event. Returns 1 = short press, 2 = long press, 0 = none.
    int PollPowerKey() {
        uint8_t status = ReadReg(0x49);
        int event = 0;
        if (status & 0x08)      event = 1;  // PKEY_SHORT
        else if (status & 0x04) event = 2;  // PKEY_LONG
        // W1C: writing back the bits that were set clears them.
        if (status & 0x0C) WriteReg(0x49, status & 0x0C);
        return event;
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const sh8601_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10}
};

class CustomLcdDisplay : public SpiLcdDisplay {
private:
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* battery_widget_ = nullptr;
    lv_obj_t* battery_body_ = nullptr;
    lv_obj_t* battery_bar_ = nullptr;
    lv_obj_t* battery_nub_ = nullptr;
    DeviceState last_state_ = kDeviceStateUnknown;
    bool awaiting_response_ = false;
    int64_t awaiting_started_us_ = 0;
    static constexpr int64_t kAwaitingTimeoutUs = 10LL * 1000 * 1000;  // 10 s

    // Set by the cancel gestures (slide-off, abort) so the next Listening→Idle
    // transition is treated as a discard rather than "awaiting response."
    static bool cancel_pending_;

    // Settings menu (opened by a single click of the boot button).
    lv_obj_t* settings_root_ = nullptr;
    bool ui_show_top_ = true;     // wifi / clock / battery at top
    bool ui_show_bottom_ = true;  // centered chat / boot info text

    static bool LooksLikeClock(const char* s) {
        return s != nullptr && std::strlen(s) == 5 && s[2] == ':';
    }

    static lv_color_t BatteryColor(int pct, bool charging) {
        if (charging)       return lv_color_hex(0x4263EB); // blue
        else if (pct > 50)  return lv_color_hex(0x37B24D); // green
        else if (pct > 20)  return lv_color_hex(0xFAB005); // yellow
        else                return lv_color_hex(0xE03131); // red
    }

    static lv_color_t ColorForState(DeviceState s) {
        switch (s) {
            case kDeviceStateStarting:        return lv_color_hex(0x3F3F3F); // 0x1F3D7A
            case kDeviceStateWifiConfiguring: return lv_color_hex(0x3F3F3F); // 0x4263EB
            case kDeviceStateAudioTesting:    return lv_color_hex(0x3F3F3F); // 0x15AABF
            case kDeviceStateActivating:      return lv_color_hex(0x3F3F3F); // 0xFD7E14
            case kDeviceStateConnecting:
            case kDeviceStateListening:       return lv_color_hex(0xE03131);
            case kDeviceStateIdle:            return lv_color_hex(0x3F3F3F);
            case kDeviceStateSpeaking:        return lv_color_hex(0x37B24D);
            case kDeviceStateUpgrading:       return lv_color_hex(0x9333EA);
            case kDeviceStateUnknown:
            default:                          return lv_color_hex(0x000000);
        }
    }

    // Push-to-talk: hold to listen, release to send. Short tap during
    // Speaking interrupts the assistant. Sliding finger off the screen while
    // holding cancels the recording without sending it to the server.
    static void OnScreenLongPressed(lv_event_t*) {
        // Only Idle → start listening. Skip if Wi-Fi is down — otherwise
        // OpenAudioChannel would block on TCP timeout for ~30 s while the
        // user thinks they're recording. Better to refuse fast.
        auto& wifi = WifiManager::GetInstance();
        if (!wifi.IsConnected() || wifi.IsConfigMode()) return;
        auto& app = Application::GetInstance();
        app.Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.ToggleChatState();
            }
        });
    }

    static void OnScreenReleased(lv_event_t*) {
        auto& app = Application::GetInstance();
        app.Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateListening) {
                app.StopListening();
            }
        });
    }

    // PRESS_LOST fires when the finger slides off while still pressed —
    // cancel the in-progress utterance without committing it.
    static void OnScreenPressLost(lv_event_t*) {
        auto& app = Application::GetInstance();
        app.Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateListening) {
                cancel_pending_ = true;  // skip the awaiting-response green window
                app.AbortSpeaking(kAbortReasonNone);
                app.SetDeviceState(kDeviceStateIdle);
            }
        });
    }

    // Touch-down during Speaking interrupts the assistant immediately.
    static void OnScreenPressed(lv_event_t*) {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            app.Schedule([]() {
                Application::GetInstance().AbortSpeaking(kAbortReasonNone);
            });
        }
    }

    static void AddLongPressHandler(lv_obj_t* obj) {
        if (obj == nullptr) return;
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(obj, OnScreenPressed,     LV_EVENT_PRESSED,      nullptr);
        lv_obj_add_event_cb(obj, OnScreenLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
        lv_obj_add_event_cb(obj, OnScreenReleased,    LV_EVENT_RELEASED,     nullptr);
        lv_obj_add_event_cb(obj, OnScreenPressLost,   LV_EVENT_PRESS_LOST,   nullptr);
    }

public:
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
        auto bs_ms_font = std::make_shared<LvglBuiltInFont>(&icons_bs_ms_36);
        auto& tm = LvglThemeManager::GetInstance();
        for (const char* name : {"light", "dark"}) {
            if (auto* t = tm.GetTheme(name)) {
                static_cast<LvglTheme*>(t)->set_icon_font(bs_ms_font);
            }
        }
    }

    virtual void SetupUI() override {
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const lv_color_t white = lv_color_hex(0xFFFFFF);

        lv_obj_set_style_pad_left(top_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_set_style_pad_right(top_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);

        lv_obj_align(status_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.1, 0);

        if (bottom_bar_ != nullptr) {
            lv_obj_align(bottom_bar_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
        }

        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }

        if (network_label_ != nullptr)      lv_obj_set_style_text_color(network_label_, white, 0);
        if (mute_label_ != nullptr)         lv_obj_set_style_text_color(mute_label_, white, 0);
        if (status_label_ != nullptr)       lv_obj_set_style_text_color(status_label_, white, 0);
        if (notification_label_ != nullptr) lv_obj_set_style_text_color(notification_label_, white, 0);
        if (chat_message_label_ != nullptr) lv_obj_set_style_text_color(chat_message_label_, white, 0);

        // Clock is anchored directly to the screen at top-center so it stays
        // dead-centered regardless of icon widths (top_bar_'s flex SPACE_BETWEEN
        // would otherwise position it midway between neighbors, not on axis).
        clock_label_ = lv_label_create(lv_screen_active());
        lv_label_set_text(clock_label_, "--:--");
        lv_obj_set_style_text_font(clock_label_, &clock_mono_30, 0);
        lv_obj_set_style_text_color(clock_label_, white, 0);
        lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(clock_label_, LV_ALIGN_TOP_MID, 0, lvgl_theme->spacing(2));

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

        // Press-and-hold anywhere on screen toggles chat (replaces the boot
        // button click as the primary "start/stop listening" gesture).
        AddLongPressHandler(container_);
        AddLongPressHandler(top_bar_);
        AddLongPressHandler(status_bar_);
        AddLongPressHandler(bottom_bar_);

        ApplyStateColor(Application::GetInstance().GetDeviceState());

        // Apply persisted UI settings (volume, brightness, rotation, hide-bars).
        LoadAndApplyPersistedSettings();

#ifdef DEBUG_RULER
        DrawDebugRuler();
#endif
    }

    void ApplyStateColor(DeviceState s) {
        // Track Listening → Idle transition as "awaiting response" so the brief
        // gap while the server runs STT/LLM/TTS doesn't show standby grey.
        // Cancel gestures (slide-off, abort) set cancel_pending_ so the
        // transition reads as a discard and skips the green window.
        if (last_state_ == kDeviceStateListening && s == kDeviceStateIdle) {
            if (cancel_pending_) {
                cancel_pending_ = false;
                awaiting_response_ = false;
            } else {
                awaiting_response_ = true;
                awaiting_started_us_ = esp_timer_get_time();
            }
        } else if (s == kDeviceStateSpeaking || s == kDeviceStateListening) {
            awaiting_response_ = false;
            cancel_pending_ = false;
        }
        if (last_state_ == s) return;
        last_state_ = s;
        DisplayLockGuard lock(this);
        lv_color_t bg = ColorForState(s);
        if (s == kDeviceStateIdle && awaiting_response_) {
            // Use the Speaking green so the Listening → awaiting → Speaking
            // run reads as a continuous color rather than a flash.
            bg = ColorForState(kDeviceStateSpeaking);
        }
        lv_obj_t* screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, bg, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        if (container_ != nullptr) {
            lv_obj_set_style_bg_color(container_, bg, 0);
            lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
        }
    }

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
        const int cx = LV_HOR_RES / 2;
        const int cy = LV_VER_RES / 2;
        thin_rect(0, cy, LV_HOR_RES, 1);
        thin_rect(cx, 0, 1, LV_VER_RES);
        // Edge ticks placed symmetrically around the centerlines.
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
        ApplyStateColor(Application::GetInstance().GetDeviceState());

        DisplayLockGuard lock(this);
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
        if (battery_bar_ != nullptr) {
            int level;
            bool charging, discharging;
            if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
                int pct = level < 0 ? 0 : (level > 100 ? 100 : level);
                lv_obj_set_style_bg_color(battery_bar_, BatteryColor(pct, charging), 0);
                lv_obj_set_width(battery_bar_, LV_PCT(pct));
            }
        }
        // Drop the "thinking" indigo back to standby grey if no Speaking state
        // arrives within the timeout (server unreachable, error, etc).
        if (awaiting_response_ && last_state_ == kDeviceStateIdle &&
            esp_timer_get_time() - awaiting_started_us_ > kAwaitingTimeoutUs) {
            awaiting_response_ = false;
            last_state_ = kDeviceStateUnknown;  // force ApplyStateColor to repaint
            ApplyStateColor(kDeviceStateIdle);
        }
    }

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
        last_state_ = kDeviceStateUnknown;
        ApplyStateColor(Application::GetInstance().GetDeviceState());
    }

    virtual void SetStatus(const char* status) override {
        if (LooksLikeClock(status)) return;
        ApplyStateColor(Application::GetInstance().GetDeviceState());
        SpiLcdDisplay::SetStatus(status);
    }

    // ---- Persisted UI settings ----
    void LoadAndApplyPersistedSettings() {
        Settings s("ui", false);
        ui_show_top_ = s.GetBool("show_top", true);
        ui_show_bottom_ = s.GetBool("show_bottom", true);
        ApplyTopVisible(ui_show_top_);
        ApplyBottomVisible(ui_show_bottom_);

        int volume = s.GetInt("volume", -1);
        if (volume >= 0) {
            auto* codec = Board::GetInstance().GetAudioCodec();
            if (codec) codec->SetOutputVolume(volume);
        }
        int brightness = s.GetInt("brightness", -1);
        if (brightness >= 5 && brightness <= 100) {
            auto* bl = Board::GetInstance().GetBacklight();
            if (bl) bl->SetBrightness((uint8_t)brightness, true);
        }
        // Rotation is applied by the board's ApplyRotation() once panel and
        // touch are both initialized — see WaveshareEsp32s3TouchAMOLED1inch8
        // constructor.
    }

    // Top row = wifi / clock / battery (top_bar_ + the standalone clock_label_).
    void ApplyTopVisible(bool show) {
        auto setv = [show](lv_obj_t* o) {
            if (o == nullptr) return;
            if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        };
        setv(top_bar_);
        setv(clock_label_);
    }

    // The "bottom text" the user sees is status_label_ (state names like
    // "Listening...") in status_bar_, which is at LV_ALIGN_BOTTOM_MID. The
    // centered chat_message_label_ in bottom_bar_ is unaffected.
    void ApplyBottomVisible(bool show) {
        if (status_bar_ == nullptr) return;
        if (show) lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }

    // ---- Settings menu ----
    bool IsSettingsOpen() const { return settings_root_ != nullptr; }

    void SetLvRotation(int rot) {
        DisplayLockGuard lock(this);
        lv_display_set_rotation(display_, (lv_display_rotation_t)(rot & 0x3));
    }

    void ToggleSettings() {
        if (IsSettingsOpen()) HideSettings();
        else                  ShowSettings();
    }

    void HideSettings() {
        DisplayLockGuard lock(this);
        if (settings_root_ != nullptr) {
            lv_obj_del(settings_root_);
            settings_root_ = nullptr;
        }
    }

    void ShowSettings() {
        DisplayLockGuard lock(this);
        if (settings_root_ != nullptr) return;
        BuildSettingsMenu();
    }

private:
    static void SettingsCloseCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (self) self->HideSettings();
    }

    static void SettingsBottomToggleCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (!self) return;
        bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
        self->ui_show_bottom_ = on;
        Settings("ui", true).SetBool("show_bottom", on);
        self->ApplyBottomVisible(on);
    }

    static void SettingsTopToggleCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (!self) return;
        bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
        self->ui_show_top_ = on;
        Settings("ui", true).SetBool("show_top", on);
        self->ApplyTopVisible(on);
    }

    static void SettingsVolumeCb(lv_event_t* e) {
        lv_obj_t* slider = lv_event_get_target_obj(e);
        int v = lv_slider_get_value(slider);
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec) codec->SetOutputVolume(v);
        Settings("ui", true).SetInt("volume", v);
        // Reactive icon: muted glyph at 0, speaker glyph otherwise.
        lv_obj_t* row = lv_obj_get_parent(slider);
        if (row && lv_obj_get_child_count(row) >= 2) {
            lv_obj_t* icon = lv_obj_get_child(row, 0);
            lv_label_set_text(icon, v == 0 ? kIconVolOff() : kIconVolUp());
        }
    }

    static void SettingsBrightnessCb(lv_event_t* e) {
        int v = lv_slider_get_value(lv_event_get_target_obj(e));
        auto* bl = Board::GetInstance().GetBacklight();
        if (bl) bl->SetBrightness((uint8_t)v, true);
        Settings("ui", true).SetInt("brightness", v);
    }

    static void SettingsWifiToggleCb(lv_event_t* e) {
        lv_obj_t* sw = lv_event_get_target_obj(e);
        bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
        auto& wifi = WifiManager::GetInstance();
        if (on) wifi.StartStation();
        else    wifi.StopStation();
        Settings("ui", true).SetBool("wifi_on", on);
        // Reactive icon: wifi when on, wifi_off when off.
        lv_obj_t* row = lv_obj_get_parent(sw);
        if (row && lv_obj_get_child_count(row) >= 2) {
            lv_obj_t* icon = lv_obj_get_child(row, 0);
            lv_label_set_text(icon, on ? kIconWifiOn() : kIconWifiOff());
        }
    }

    // Hotspot (away) toggle: swap the OTA URL the device pings on boot
    // between two pre-configured endpoints. The OTA response carries the
    // websocket URL the device then connects to, so this single switch
    // reroutes everything downstream.
    //   home  → LAN-IP   (e.g. http://192.168.1.50:8003/xiaozhi/ota/)
    //   away  → Tailscale (e.g. http://100.x.y.z:8003/xiaozhi/ota/)
    // Both URLs are stored in the "ui" NVS namespace under keys
    // ota_home / ota_away. Populate them once via the MCP tools
    // self.firmware.set_ota_home and self.firmware.set_ota_away.
    // Toggle takes effect on next reboot — we trigger Application::Reboot()
    // so the change is immediate and unambiguous.
    static void SettingsHotspotToggleCb(lv_event_t* e) {
        lv_obj_t* sw = lv_event_get_target_obj(e);
        bool away = lv_obj_has_state(sw, LV_STATE_CHECKED);
        Settings ui("ui", false);
        // If the user hasn't ALSO configured the home URL, falling back from
        // away → home would brick the device. Require both before allowing
        // the switch to do anything.
        std::string home_url = ui.GetString("ota_home", "");
        std::string away_url = ui.GetString("ota_away", "");
        std::string target = away ? away_url : home_url;
        if (target.empty() || home_url.empty() || away_url.empty()) {
            ESP_LOGW("Settings",
                     "Hotspot toggle ignored: ota_home=%s ota_away=%s",
                     home_url.empty() ? "(empty)" : "set",
                     away_url.empty() ? "(empty)" : "set");
            // Bounce the switch back to its prior state.
            if (away) lv_obj_clear_state(sw, LV_STATE_CHECKED);
            else      lv_obj_add_state(sw, LV_STATE_CHECKED);
            // Surface the failure so the user knows why nothing happened.
            Application::GetInstance().Alert(
                "Setup",
                "Set OTA URLs first via MCP:\n"
                "self.firmware.set_ota_home\n"
                "self.firmware.set_ota_away",
                "warning");
            return;
        }
        Settings("wifi", true).SetString("ota_url", target);
        Settings("ui", true).SetBool("hotspot_away", away);
        ESP_LOGI("Settings", "OTA URL → %s mode: %s",
                 away ? "away" : "home", target.c_str());
        // Reboot in 500 ms so the user sees the switch animation finish.
        esp_timer_handle_t t = nullptr;
        const esp_timer_create_args_t args = {
            .callback = [](void*) { Application::GetInstance().Reboot(); },
            .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
            .name = "hotspot_reboot", .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &t) == ESP_OK) {
            esp_timer_start_once(t, 500 * 1000);
        }
    }

    // Cycle 0° → 90° → 180° → 270° → 0°. Touch driver mirror/swap flags are
    // updated alongside via the board's ApplyRotation, so taps land where
    // the user pressed.
    static void SettingsRotationCb(lv_event_t* e);

    // Empty row shell with horizontal flex layout: caller adds children (icon
    // on the left, control on the right).
    lv_obj_t* MakeRowShell(lv_obj_t* parent) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        return row;
    }

    // 3 horizontal bars stacked vertically — top, middle, bottom. The "filled"
    // index gets a solid white fill, the others a thin white outline. Visual
    // language: "this row controls the [top/middle/bottom] bar of the UI."
    lv_obj_t* MakeStackIcon(lv_obj_t* parent, int filled_bar) {
        lv_obj_t* box = lv_obj_create(parent);
        const int W = 22, H = 28, BH = 5, BW = 22, GAP = 3;
        lv_obj_set_size(box, W, H);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        const int total = 3 * BH + 2 * GAP;
        const int top_y = (H - total) / 2;
        const lv_color_t white = lv_color_hex(0xFFFFFF);
        for (int i = 0; i < 3; ++i) {
            lv_obj_t* bar = lv_obj_create(box);
            lv_obj_set_size(bar, BW, BH);
            lv_obj_set_pos(bar, 0, top_y + i * (BH + GAP));
            lv_obj_set_style_radius(bar, 1, 0);
            lv_obj_set_style_pad_all(bar, 0, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            if (i == filled_bar) {
                lv_obj_set_style_bg_color(bar, white, 0);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(bar, 0, 0);
            } else {
                lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(bar, 1, 0);
                lv_obj_set_style_border_color(bar, white, 0);
            }
        }
        return box;
    }

    // Single label rendered with the icon font. UTF-8 string is the FA-style
    // codepoint(s) from icons_bs_ms_36.
    lv_obj_t* MakeFaIcon(lv_obj_t* parent, const char* glyph) {
        lv_obj_t* lbl = lv_label_create(parent);
        lv_label_set_text(lbl, glyph);
        lv_obj_set_style_text_font(lbl, &icons_bs_ms_36, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        return lbl;
    }

    static const char* kIconWifiOn()    { return "\xEF\x87\xAB"; }  // U+F1EB
    static const char* kIconWifiOff()   { return "\xEF\x9A\xAC"; }  // U+F6AC
    static const char* kIconVolUp()     { return "\xEF\x80\xA8"; }  // U+F028
    static const char* kIconVolOff()    { return "\xEF\x9A\xA9"; }  // U+F6A9
    static const char* kIconSun()       { return "\xEF\x86\x85"; }  // U+F185

    lv_obj_t* AddRow(lv_obj_t* parent, const char* label_text, bool icon = false) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, label_text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        if (icon) lv_obj_set_style_text_font(lbl, &icons_bs_ms_36, 0);
        return row;
    }

    void AddToggleRow(lv_obj_t* parent, const char* label, bool initial,
                      lv_event_cb_t cb) {
        lv_obj_t* row = AddRow(parent, label);
        lv_obj_t* sw = lv_switch_create(row);
        if (initial) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, this);
    }

    void AddSliderRow(lv_obj_t* parent, const char* label, int initial,
                      int min, int max, lv_event_cb_t cb, bool icon = false) {
        lv_obj_t* row = AddRow(parent, label, icon);
        lv_obj_t* sl = lv_slider_create(row);
        lv_slider_set_range(sl, min, max);
        lv_slider_set_value(sl, initial, LV_ANIM_OFF);
        lv_obj_set_width(sl, LV_PCT(60));
        lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, this);
    }

    void AddRotationRow(lv_obj_t* parent, int initial) {
        lv_obj_t* row = AddRow(parent, "Rotation");
        lv_obj_t* btn = lv_button_create(row);
        int rot = (initial == 2) ? 2 : 0;
        lv_obj_set_user_data(btn, (void*)(intptr_t)rot);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, rot == 0 ? "0°" : "180°");
        lv_obj_set_style_pad_all(btn, 6, 0);
        lv_obj_add_event_cb(btn, SettingsRotationCb, LV_EVENT_CLICKED, this);
    }

    void BuildSettingsMenu() {
        Settings s("ui", false);
        bool show_bottom = s.GetBool("show_bottom", true);
        bool show_top    = s.GetBool("show_top", true);
        bool wifi_on     = s.GetBool("wifi_on", true);
        int rotation     = s.GetInt("rotation", 0);
        auto* codec = Board::GetInstance().GetAudioCodec();
        int volume = codec ? codec->output_volume() : 70;
        int brightness = s.GetInt("brightness", 75);

        settings_root_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(settings_root_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(settings_root_, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(settings_root_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(settings_root_, 0, 0);
        lv_obj_set_style_radius(settings_root_, 0, 0);
        lv_obj_set_style_pad_all(settings_root_, 14, 0);
        lv_obj_set_style_pad_top(settings_root_, 28, 0);
        lv_obj_set_style_pad_bottom(settings_root_, 28, 0);
        lv_obj_set_flex_flow(settings_root_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(settings_root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scroll_dir(settings_root_, LV_DIR_VER);
        lv_obj_align(settings_root_, LV_ALIGN_CENTER, 0, 0);

        // Volume — speaker icon (reactive), then slider.
        {
            lv_obj_t* row = MakeRowShell(settings_root_);
            MakeFaIcon(row, volume == 0 ? kIconVolOff() : kIconVolUp());
            lv_obj_t* sl = lv_slider_create(row);
            lv_slider_set_range(sl, 0, 100);
            lv_slider_set_value(sl, volume, LV_ANIM_OFF);
            lv_obj_set_width(sl, LV_PCT(70));
            lv_obj_add_flag(sl, LV_OBJ_FLAG_PRESS_LOCK);
            lv_obj_clear_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);
            lv_obj_add_event_cb(sl, SettingsVolumeCb, LV_EVENT_VALUE_CHANGED, this);
        }
        // Brightness — sun icon, then slider.
        {
            lv_obj_t* row = MakeRowShell(settings_root_);
            MakeFaIcon(row, kIconSun());
            lv_obj_t* sl = lv_slider_create(row);
            lv_slider_set_range(sl, 5, 100);
            lv_slider_set_value(sl, brightness, LV_ANIM_OFF);
            lv_obj_set_width(sl, LV_PCT(70));
            lv_obj_add_flag(sl, LV_OBJ_FLAG_PRESS_LOCK);
            lv_obj_clear_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);
            lv_obj_add_event_cb(sl, SettingsBrightnessCb, LV_EVENT_VALUE_CHANGED, this);
        }
        // Status (top row visibility) — top-filled stack icon + switch.
        {
            lv_obj_t* row = MakeRowShell(settings_root_);
            MakeStackIcon(row, 0);
            lv_obj_t* sw = lv_switch_create(row);
            if (show_top) lv_obj_add_state(sw, LV_STATE_CHECKED);
            lv_obj_add_event_cb(sw, SettingsTopToggleCb, LV_EVENT_VALUE_CHANGED, this);
        }
        // Bottom text visibility — bottom-filled stack icon + switch.
        {
            lv_obj_t* row = MakeRowShell(settings_root_);
            MakeStackIcon(row, 2);
            lv_obj_t* sw = lv_switch_create(row);
            if (show_bottom) lv_obj_add_state(sw, LV_STATE_CHECKED);
            lv_obj_add_event_cb(sw, SettingsBottomToggleCb, LV_EVENT_VALUE_CHANGED, this);
        }
        // WiFi — reactive wifi/wifi_off icon + switch.
        {
            lv_obj_t* row = MakeRowShell(settings_root_);
            MakeFaIcon(row, wifi_on ? kIconWifiOn() : kIconWifiOff());
            lv_obj_t* sw = lv_switch_create(row);
            if (wifi_on) lv_obj_add_state(sw, LV_STATE_CHECKED);
            lv_obj_add_event_cb(sw, SettingsWifiToggleCb, LV_EVENT_VALUE_CHANGED, this);
        }
        // Hotspot — switches OTA URL between home (LAN) and away (Tailscale)
        // endpoints; reboots on toggle. Configure URLs once via MCP tools
        // self.firmware.set_ota_home / set_ota_away.
        // Icon: the wifi glyph rotated 180° = signal radiating downward,
        // the canonical "personal hotspot / tethering" pictogram.
        {
            lv_obj_t* row = MakeRowShell(settings_root_);
            lv_obj_t* icon = MakeFaIcon(row, kIconWifiOn());
            lv_obj_set_style_transform_pivot_x(icon, LV_PCT(50), 0);
            lv_obj_set_style_transform_pivot_y(icon, LV_PCT(50), 0);
            lv_obj_set_style_transform_rotation(icon, 1800, 0);  // 180.0°
            lv_obj_t* sw = lv_switch_create(row);
            if (s.GetBool("hotspot_away", false)) lv_obj_add_state(sw, LV_STATE_CHECKED);
            lv_obj_add_event_cb(sw, SettingsHotspotToggleCb, LV_EVENT_VALUE_CHANGED, this);
        }
        (void)rotation;

        // Close button
        lv_obj_t* close = lv_button_create(settings_root_);
        lv_obj_set_style_margin_top(close, 8, 0);
        lv_obj_t* cl = lv_label_create(close);
        lv_label_set_text(cl, "Close");
        lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_event_cb(close, SettingsCloseCb, LV_EVENT_CLICKED, this);
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
        uint8_t data[1] = {((uint8_t)((255 * brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED1inch8 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        // -1, -1, -1: no auto-sleep, no auto-shutdown. Keeps USB CDC alive.
        power_save_timer_ = new PowerSaveTimer(-1, -1, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(codec_i2c_bus_, I2C_ADDRESS, &io_expander);
        if(ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 |IO_EXPANDER_PIN_NUM_2, IO_EXPANDER_OUTPUT);
        ret |= esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1|IO_EXPANDER_PIN_NUM_2, 1);
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1|IO_EXPANDER_PIN_NUM_2, 0);
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1|IO_EXPANDER_PIN_NUM_2, 1);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(codec_i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = GPIO_NUM_11;
        buscfg.data0_io_num = GPIO_NUM_4;
        buscfg.data1_io_num = GPIO_NUM_5;
        buscfg.data2_io_num = GPIO_NUM_6;
        buscfg.data3_io_num = GPIO_NUM_7;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        // BOOT button: PTT only.
        //   Click  : abort an ongoing reply (no menu — that's on the power key)
        //   Hold   : push-to-talk start (when Idle, Wi-Fi up, no menu open)
        //   Release: push-to-talk stop
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            DeviceState s = app.GetDeviceState();
            if (s == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (s == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
            }
        });
        boot_button_.OnLongPress([this]() {
            auto& app = Application::GetInstance();
            auto& wifi = WifiManager::GetInstance();
            if (!wifi.IsConnected() || wifi.IsConfigMode()) return;
            if (app.GetDeviceState() == kDeviceStateIdle &&
                display_ != nullptr && !display_->IsSettingsOpen()) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnPressUp([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateListening) {
                app.StopListening();
            }
        });
    }

    // The bottom physical button is the AXP2101 PWRON key — it doesn't have
    // its own GPIO. We poll the PMIC's IRQ status register every 50 ms and
    // dispatch short-press → settings menu.
    esp_timer_handle_t pmic_poll_timer_ = nullptr;
    void StartPmicKeyPolling() {
        const esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                auto* self = static_cast<WaveshareEsp32s3TouchAMOLED1inch8*>(arg);
                if (!self->pmic_) return;
                int ev = self->pmic_->PollPowerKey();
                if (ev == 1 && self->display_ != nullptr) {
                    // Schedule on app thread so LVGL ops happen with the lock.
                    Application::GetInstance().Schedule([self]() {
                        if (self->display_) self->display_->ToggleSettings();
                    });
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pmic_key_poll",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &pmic_poll_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(pmic_poll_timer_, 50 * 1000));
    }

    esp_lcd_panel_handle_t panel_handle_ = nullptr;
    void InitializeSH8601Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
            EXAMPLE_PIN_NUM_LCD_CS,
            nullptr,
            nullptr
        );
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const sh8601_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(sh8601_lcd_init_cmd_t),
            .flags ={
                .use_qspi_interface = 1,
            }
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.flags.reset_active_high = 1,
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void *)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel));
        panel_handle_ = panel;

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

    esp_lcd_touch_handle_t tp_handle_ = nullptr;
    void InitializeTouch()
    {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_21,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .flags =
            {
                .disable_control_phase = 1,
            }
        };
        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(codec_i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));
        tp_handle_ = tp;
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(), 
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
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
        // Hotspot-mode OTA URLs. The Settings menu's "Hotspot" toggle swaps
        // wifi/ota_url between these two values and reboots. Set once and
        // forget — survives reboots in the "ui" NVS namespace.
        mcp_server.AddTool("self.firmware.set_ota_home",
            "Set the OTA URL used when on home Wi-Fi (typically a LAN IP, "
            "e.g. http://192.168.1.50:8003/xiaozhi/ota/).",
            PropertyList({Property("url", kPropertyTypeString)}),
            [](const PropertyList& p) -> ReturnValue {
                Settings("ui", true).SetString("ota_home",
                    p["url"].value<std::string>());
                return true;
            });
        mcp_server.AddTool("self.firmware.set_ota_away",
            "Set the OTA URL used when on a phone hotspot or away from home "
            "(typically a Tailscale 100.x.y.z address, e.g. "
            "http://100.64.0.5:8003/xiaozhi/ota/). Requires the phone to be "
            "running Tailscale so the hotspot inherits the tailnet route.",
            PropertyList({Property("url", kPropertyTypeString)}),
            [](const PropertyList& p) -> ReturnValue {
                Settings("ui", true).SetString("ota_away",
                    p["url"].value<std::string>());
                return true;
            });
        mcp_server.AddTool("self.firmware.get_ota_endpoints",
            "Return both stored OTA URLs (home and away) plus which one is "
            "currently active.",
            PropertyList(),
            [](const PropertyList& p) -> ReturnValue {
                Settings ui("ui", false);
                std::string home = ui.GetString("ota_home", "");
                std::string away = ui.GetString("ota_away", "");
                bool is_away = ui.GetBool("hotspot_away", false);
                return std::string("{\"home\":\"") + home +
                       "\",\"away\":\"" + away +
                       "\",\"active\":\"" + (is_away ? "away" : "home") + "\"}";
            });
    }

public:
    WaveshareEsp32s3TouchAMOLED1inch8() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeTca9554();
        InitializeAxp2101();
        InitializeSpi();
        InitializeSH8601Display();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        StartPmicKeyPolling();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    // Apply rotation via the panel's MADCTL (hardware mirror) and matching
    // touch driver flags. Only 0° and 180° are supported here — 90°/270°
    // would need LVGL's framebuffer dimensions swapped to match the panel's
    // post-swap pixel order, which esp_lvgl_port doesn't expose cleanly.
    //   rot == 2 → 180°; anything else → 0°
    void ApplyRotation(int rot) {
        bool flipped = (rot == 2);
        if (panel_handle_) {
            esp_lcd_panel_swap_xy(panel_handle_, false);
            esp_lcd_panel_mirror(panel_handle_, flipped, flipped);
        }
        if (tp_handle_) {
            esp_lcd_touch_set_swap_xy(tp_handle_, false);
            esp_lcd_touch_set_mirror_x(tp_handle_, flipped);
            esp_lcd_touch_set_mirror_y(tp_handle_, flipped);
        }
        Settings("ui", true).SetInt("rotation", flipped ? 2 : 0);
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
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

bool CustomLcdDisplay::cancel_pending_ = false;

void CustomLcdDisplay::SettingsRotationCb(lv_event_t* e) {
    auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
    if (!self) return;
    lv_obj_t* btn = lv_event_get_target_obj(e);
    int rot = (int)(intptr_t)lv_obj_get_user_data(btn);
    rot = (rot == 0) ? 2 : 0;  // toggle 0° ↔ 180°
    lv_obj_set_user_data(btn, (void*)(intptr_t)rot);
    auto* board = static_cast<WaveshareEsp32s3TouchAMOLED1inch8*>(&Board::GetInstance());
    if (board) board->ApplyRotation(rot);
    if (lv_obj_get_child_count(btn) > 0) {
        lv_obj_t* lbl = lv_obj_get_child(btn, 0);
        lv_label_set_text(lbl, rot == 0 ? "0°" : "180°");
    }
}

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED1inch8);
