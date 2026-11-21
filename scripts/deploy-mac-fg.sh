#!/bin/bash
# PS-WRAP-MAC-FG: build (separate settings store / bundle id, no auto-update) + self-contained .app → dist/PS-WRAP-MAC-FG.app
# usage: scripts/deploy-mac-fg.sh   (needs scripts/setup-mac.sh once; python with protobuf on PATH)
set -e
cd "$(dirname "$0")/.."
PSWRAP_FG_BUILD=ON scripts/build-mac.sh
scripts/deploy-mac.sh --no-dmg
rm -rf dist/PS-WRAP-MAC-FG.app dist/PS-WRAP-MAC-v*-macos-arm64.zip*
mv dist/PS-WRAP-MAC.app dist/PS-WRAP-MAC-FG.app
scripts/bundle-rife.sh dist/PS-WRAP-MAC-FG.app
codesign --force --deep --sign - --entitlements ps-wrap/gui/entitlements.xml dist/PS-WRAP-MAC-FG.app > dist/codesign.log 2>&1
codesign --verify --deep dist/PS-WRAP-MAC-FG.app && echo "→ dist/PS-WRAP-MAC-FG.app"
