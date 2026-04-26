# Roadmap

## Done
- [x] **Fix invisible battery / charging / wifi indicators.** Root cause: the C6-2.06 round-AMOLED panel has masked corners. Top-left and top-right of `top_bar_` (where network and battery icons sit) fall inside the corner cutoff. Fix: inset `top_bar_` horizontally by 20% on the C6-2.06 board (`CustomLcdDisplay::SetupUI`).

## In progress
- [ ] **Replace emoji-based emotion display with full-screen solid colors.**
  - Working version exists on branch `wip-mood-ui-and-touch-swap` (`ApplySolidEmotionStyle` + emotion→color map + `SetChatMessage` override). Cherry-pick the UI parts; drop the dev-only artifacts (hardcoded WiFi creds, README deletions, FT5x06 swap).
- [ ] **Replace boot-button chat toggle with press-and-hold on the screen.**
  - `OnScreenLongPressed` handler also lives on `wip-mood-ui-and-touch-swap`. Cherry-pick alongside the solid-color UI.
  - Verify CST9217 input events propagate through LVGL to the active screen object.
- [ ] **Enable a wake word.**
  - Today: `CONFIG_WAKE_WORD_DISABLED=y`, `USE_ESP_WAKE_WORD` unset; AFE pipeline (`CONFIG_AFE_INTERFACE_V1`) is running but no model loaded.
  - Pick a model that fits the C6 RAM budget; flip Kconfig; verify trigger end-to-end.
- [ ] **Verify pipeline: xiaozhi-client ↔ xiaozhi-server ↔ OpenAI / Claude.**
  - Today the device reaches `mqtt.xiaozhi.me` (cloud). Need to confirm whether to point at the local server at `/Volumes/username/xiaozhi-server` and how its LLM backend is wired (OpenAI vs Claude vs other).
- [ ] **Remove diagnostic `ESP_LOGI` calls from `LvglDisplay::UpdateStatusBar`** before any of this lands.

## Notes
- Branch `wip-mood-ui-and-touch-swap` (local only, at `7f6f60b`) holds untested customization work — see `git log -1 wip-mood-ui-and-touch-swap` for the full bundle description.
- `main` is at `473c8cb` (PR #1 merged), matches `github/main`.
