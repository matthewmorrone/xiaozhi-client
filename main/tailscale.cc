#include "tailscale.h"

#include <esp_log.h>

#include "settings.h"

#if defined(USE_ESP_TAILSCALE) || defined(CONFIG_USE_ESP_TAILSCALE)
extern "C" {
#include "esp_tailscale.h"
}
#define HAS_ESP_TAILSCALE 1
#else
#define HAS_ESP_TAILSCALE 0
#endif

#define TAG "Tailscale"

namespace Tailscale {

namespace {

constexpr const char* kNamespace = "tailscale";
constexpr const char* kKeyAuthKey = "auth_key";
constexpr const char* kKeyHostname = "hostname";
constexpr const char* kKeyLoginSrv = "login_server";
constexpr const char* kKeyEnabled = "enabled";

bool g_connected = false;

}  // namespace

Credentials Load() {
    Settings settings(kNamespace, false);
    Credentials creds;
    creds.auth_key = settings.GetString(kKeyAuthKey);
    creds.hostname = settings.GetString(kKeyHostname);
    creds.login_server = settings.GetString(kKeyLoginSrv);
    creds.enabled = settings.GetBool(kKeyEnabled, false);
    return creds;
}

void Save(const Credentials& creds) {
    Settings settings(kNamespace, true);
    settings.SetString(kKeyAuthKey, creds.auth_key);
    settings.SetString(kKeyHostname, creds.hostname);
    settings.SetString(kKeyLoginSrv, creds.login_server);
    settings.SetBool(kKeyEnabled, creds.enabled);
    ESP_LOGI(TAG, "Saved credentials (enabled=%d, hostname=%s, login_server=%s)",
             creds.enabled,
             creds.hostname.empty() ? "<default>" : creds.hostname.c_str(),
             creds.login_server.empty() ? "<default>" : creds.login_server.c_str());
}

void Clear() {
    Settings settings(kNamespace, true);
    settings.EraseAll();
    g_connected = false;
}

bool IsEnabled() {
    Settings settings(kNamespace, false);
    return settings.GetBool(kKeyEnabled, false);
}

bool Connect() {
    Credentials creds = Load();
    if (!creds.enabled) {
        return true;
    }
    if (creds.auth_key.empty()) {
        ESP_LOGW(TAG, "Tailscale enabled but no auth_key configured; skipping");
        return false;
    }
    if (g_connected) {
        return true;
    }

#if HAS_ESP_TAILSCALE
    esp_tailscale_config_t cfg = {};
    cfg.auth_key = creds.auth_key.c_str();
    cfg.hostname = creds.hostname.empty() ? nullptr : creds.hostname.c_str();
    cfg.login_server = creds.login_server.empty() ? nullptr : creds.login_server.c_str();
    esp_err_t err = esp_tailscale_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_tailscale_start failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Tailscale connected (hostname=%s)",
             creds.hostname.empty() ? "<default>" : creds.hostname.c_str());
    g_connected = true;
    return true;
#else
    ESP_LOGW(TAG,
             "Tailscale enabled in NVS but esp_tailscale component is not linked; "
             "build with the esp_tailscale managed component and define USE_ESP_TAILSCALE");
    return true;
#endif
}

}  // namespace Tailscale
