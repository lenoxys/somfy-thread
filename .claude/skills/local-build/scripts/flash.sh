#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
# Flash the locally built somfy-thread firmware to an ESP32-C6 over its native USB.
#
#   scripts/flash.sh app    app-only update — KEEPS nvs (shades/config) + fctry (Matter creds)   [default]
#   scripts/flash.sh wipe    merged image at 0x0 — ERASES nvs (all shades/config). Back up first!
#
# Port defaults to /dev/cu.usbmodem14101; override with SOMFY_PORT.
# Run on the host (esptool needs /dev). Flashing transmits no Somfy RF.
set -euo pipefail

PORT="${SOMFY_PORT:-/dev/cu.usbmodem14101}"
MODE="${1:-app}"
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

case "$MODE" in
  wipe)
    echo "WIPE flash on $PORT — this erases nvs (shades + config). Ctrl-C to abort."
    esptool --chip esp32c6 -p "$PORT" write-flash 0x0 somfy-thread-esp32c6-dev.bin
    ;;
  app)
    echo "App-only flash on $PORT — keeping shades/config."
    esptool --chip esp32c6 -p "$PORT" write-flash \
      0x10000 build/ota_data_initial.bin \
      0x20000 build/somfy_thread.bin
    ;;
  *)
    echo "usage: $0 app|wipe" >&2
    exit 1
    ;;
esac
