#include "language.h"

#include <algorithm>

#include <esp_log.h>

#include "assets/lang_config.h"
#include "settings.h"

#define TAG "Language"

namespace Language {

namespace {

constexpr const char* kSettingsNamespace = "app";
constexpr const char* kSettingsKey = "language";

// Mirrors the directory listing under main/assets/locales/. Keep alphabetical.
const std::vector<std::string>& SupportedCodes() {
    static const std::vector<std::string> kCodes = {
        "ar-SA", "bg-BG", "ca-ES", "cs-CZ", "da-DK", "de-DE", "el-GR",
        "en-US", "es-ES", "fa-IR", "fi-FI", "fil-PH", "fr-FR", "he-IL",
        "hi-IN", "hr-HR", "hu-HU", "id-ID", "it-IT", "ja-JP", "ko-KR",
        "ms-MY", "nb-NO", "nl-NL", "pl-PL", "pt-PT", "ro-RO", "ru-RU",
        "sk-SK", "sl-SI", "sr-RS", "sv-SE", "th-TH", "tr-TR", "uk-UA",
        "vi-VN", "zh-CN", "zh-TW",
    };
    return kCodes;
}

}  // namespace

std::string GetCode() {
    Settings settings(kSettingsNamespace, false);
    std::string override_code = settings.GetString(kSettingsKey);
    if (!override_code.empty() && IsSupported(override_code)) {
        return override_code;
    }
    return std::string(Lang::CODE);
}

bool SetCode(const std::string& code) {
    if (!IsSupported(code)) {
        ESP_LOGW(TAG, "Rejected unsupported language code: %s", code.c_str());
        return false;
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kSettingsKey, code);
    ESP_LOGI(TAG, "Language set to %s", code.c_str());
    return true;
}

void ClearOverride() {
    Settings settings(kSettingsNamespace, true);
    settings.EraseKey(kSettingsKey);
}

bool IsSupported(const std::string& code) {
    const auto& codes = SupportedCodes();
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

const std::vector<std::string>& ListSupported() {
    return SupportedCodes();
}

}  // namespace Language
