#!/bin/sh
# SPDX-License-Identifier: Unlicense
# Vendor ESP Web Tools locally. The published dist file is a self-contained ES
# module bundle, so the deployed site loads it same-origin and makes no external
# request at runtime. Run this before serving/deploying doc/. Pin the version.
set -e
VER=10.0.0
DIR=$(dirname "$0")
curl -fsSL "https://unpkg.com/esp-web-tools@${VER}/dist/web/install-button.js" \
  -o "$DIR/esp-web-tools.js"
echo "vendored esp-web-tools@${VER} -> $DIR/esp-web-tools.js"
