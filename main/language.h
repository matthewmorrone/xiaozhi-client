#ifndef LANGUAGE_H_
#define LANGUAGE_H_

#include <string>
#include <vector>

// Runtime language selection.
//
// Compile-time UI strings still come from assets/lang_config.h via Lang::CODE.
// This module layers a runtime override on top so the device can advertise a
// different language to the server (so STT/TTS replies in the user's chosen
// language) without rebuilding the firmware.
//
// Storage: NVS namespace "app", key "language" (BCP-47 code, e.g. "en-US").
namespace Language {

// Returns the active language code. Reads NVS first; falls back to Lang::CODE.
std::string GetCode();

// Persists the language code if it is in ListSupported(); returns false otherwise.
bool SetCode(const std::string& code);

// Clears the runtime override so GetCode() returns Lang::CODE.
void ClearOverride();

// Whether the given code matches one of the bundled locales.
bool IsSupported(const std::string& code);

// All locale codes that ship with the firmware (assets/locales/*).
const std::vector<std::string>& ListSupported();

}  // namespace Language

#endif  // LANGUAGE_H_
