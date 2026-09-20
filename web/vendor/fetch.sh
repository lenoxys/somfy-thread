#!/bin/sh
# SPDX-License-Identifier: Unlicense
# Vendor third-party JS locally so the deployed site loads everything
# same-origin and makes no external request at runtime. Run this before
# serving/deploying web/. Pin every version.
set -e
EWT_VER=10.0.0
QRC_VER=1.4.4
DIR=$(dirname "$0")

# ESP Web Tools ships a split build: install-button.js dynamically imports a
# dozen hashed sibling chunks (the install dialog, per-chip flasher stubs,
# styles). Vendor the whole dist/web dir so those relative imports resolve
# same-origin, then rename the entry point to the name index.html loads.
TMP=$(mktemp -d)
curl -fsSL "https://registry.npmjs.org/esp-web-tools/-/esp-web-tools-${EWT_VER}.tgz" \
  -o "$TMP/ewt.tgz"
tar xzf "$TMP/ewt.tgz" -C "$TMP"
cp "$TMP"/package/dist/web/*.js "$DIR/"
mv "$DIR/install-button.js" "$DIR/esp-web-tools.js"
rm -rf "$TMP"
echo "vendored esp-web-tools@${EWT_VER} (dist/web) -> $DIR/"

curl -fsSL "https://unpkg.com/qrcode-generator@${QRC_VER}/qrcode.js" \
  -o "$DIR/qrcode.js"
echo "vendored qrcode-generator@${QRC_VER} -> $DIR/qrcode.js"
