#!/bin/sh
# SPDX-License-Identifier: Unlicense
# Vendor third-party JS locally so the deployed site loads everything
# same-origin and makes no external request at runtime. Run this before
# serving/deploying web/. Pin every version.
set -e
ESPTOOL_VER=0.7.0
QRC_VER=1.4.4
DIR=$(dirname "$0")

# esptool-js ships a single self-contained ESM bundle (pako inlined, no runtime
# deps) exporting ESPLoader/Transport. app.js imports it lazily to flash in-page
# over the already-granted serial port. Rename it to the name app.js imports.
curl -fsSL "https://unpkg.com/esptool-js@${ESPTOOL_VER}/bundle.js" \
  -o "$DIR/esptool.js"
echo "vendored esptool-js@${ESPTOOL_VER} -> $DIR/esptool.js"

curl -fsSL "https://unpkg.com/qrcode-generator@${QRC_VER}/qrcode.js" \
  -o "$DIR/qrcode.js"
echo "vendored qrcode-generator@${QRC_VER} -> $DIR/qrcode.js"
