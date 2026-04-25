#ifndef TAILSCALE_H_
#define TAILSCALE_H_

#include <string>

// Stores Tailscale credentials in NVS so the device can reach the server over a
// tailnet when it isn't on the same LAN as the host.
//
// Bringing up the actual VPN tunnel is delegated to the optional
// `esp_tailscale` ESP-IDF component
// (https://github.com/Espressif-Systems-and-Services/esp_tailscale).
// When that component is present the build defines USE_ESP_TAILSCALE and
// Connect() forwards the stored auth key to it. When it isn't present
// Connect() logs the configuration and returns without an error so the rest of
// the firmware behaves as before.
//
// NVS namespace: "tailscale". Keys: "auth_key", "hostname", "login_server",
// "enabled".
namespace Tailscale {

struct Credentials {
    std::string auth_key;       // tskey-auth-... (required)
    std::string hostname;       // optional Tailscale machine name
    std::string login_server;   // optional control plane URL (Headscale, etc.)
    bool enabled = false;
};

Credentials Load();
void Save(const Credentials& creds);
void Clear();

bool IsEnabled();

// Brings up the tailnet using stored credentials. Safe to call when disabled
// (no-op) or when esp_tailscale is not linked (logs and returns true).
// Should run after the underlying network (WiFi/Ethernet) is up.
bool Connect();

}  // namespace Tailscale

#endif  // TAILSCALE_H_
