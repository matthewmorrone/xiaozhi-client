#!/usr/bin/env bash
# Build the firmware and serve it over HTTP for the device to fetch on
# long-press of the boot button.
#
# Workflow:
#   1. Run this script (rebuilds and starts a server on port 8123).
#   2. On the device, long-press the BOOT button.
#   3. Device fetches xiaozhi.bin from this Mac, flashes the inactive OTA slot,
#      and reboots.
#
# The default URL baked into the board is http://192.168.0.62:8123/xiaozhi.bin.
# If your IP changes, either edit the OnLongPress default in the board file
# OR call the MCP tool self.firmware.set_ota_url.

set -euo pipefail
cd "$(dirname "$0")/.."

if [ -z "${IDF_PATH:-}" ]; then
  source ~/esp/esp-idf/export.sh >/dev/null
fi

idf.py build
HOST_IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo unknown)"
PORT=8123

cat <<EOF

----------------------------------------------------
Build complete. Serving build/xiaozhi.bin
URL:        http://${HOST_IP}:${PORT}/xiaozhi.bin
On device:  long-press the BOOT button to start OTA.
Stop:       Ctrl-C
----------------------------------------------------
EOF

cd build
exec python3 -m http.server "${PORT}"
