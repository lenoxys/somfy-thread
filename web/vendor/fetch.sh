#!/bin/sh
# SPDX-License-Identifier: Unlicense
# Vendor third-party JS locally so the deployed site loads everything
# same-origin and makes no external request at runtime. Run this before
# serving/deploying web/. Pin every version.
set -e
EWT_VER=10.0.0
QRC_VER=1.4.4
DIR=$(dirname "$0")
curl -fsSL "https://unpkg.com/esp-web-tools@${EWT_VER}/dist/web/install-button.js" \
  -o "$DIR/esp-web-tools.js"
echo "vendored esp-web-tools@${EWT_VER} -> $DIR/esp-web-tools.js"
curl -fsSL "https://unpkg.com/qrcode-generator@${QRC_VER}/qrcode.js" \
  -o "$DIR/qrcode.js"
echo "vendored qrcode-generator@${QRC_VER} -> $DIR/qrcode.js"
