#!/bin/bash
# PS-WRAP-MAC-FG: build libPsWrapRife (rife-metal v0.1.8 + PS-WRAP patch, Apache-2.0; Practical-RIFE v4.25 weights, MIT)
# and put it + weights + metallib + license notices into <app>
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$1"
RM="$ROOT/_deps/rife-metal"
if [ ! -d "$RM" ]; then
	git clone -q https://github.com/cinemore/rife-metal.git "$RM"
	(cd "$RM" && git checkout -q v0.1.8 && git apply "$ROOT/ps-wrap/third-party/pswraprife/rife-metal-v0.1.8-pswrap.patch")
fi
(cd "$ROOT/ps-wrap/third-party/pswraprife" && swift build -c release >/dev/null)
B="$ROOT/ps-wrap/third-party/pswraprife/.build/release"
mkdir -p "$APP/Contents/Frameworks" "$APP/Contents/Resources/rife"
cp -f "$B/libPsWrapRife.dylib" "$APP/Contents/Frameworks/"
cp -f "$RM/Sources/RifeMetal/Resources/rife-v4.25.rmw" "$APP/Contents/Resources/rife/"
cp -f "$B/RifeMetal_RifeMetalCore.bundle/default.metallib" "$APP/Contents/Resources/rife/"
cp -f "$RM/LICENSE" "$APP/Contents/Resources/rife/rife-metal-LICENSE-Apache-2.0.txt"
cp -f "$RM/THIRD-PARTY.md" "$APP/Contents/Resources/rife/rife-metal-THIRD-PARTY.md"
cat > "$APP/Contents/Resources/rife/NOTICE.txt" <<NOTE
RIFE frame interpolation in PS-WRAP-MAC-FG:
- cinemore/rife-metal v0.1.8 (Apache-2.0), modified: metallib loaded from Contents/Resources/rife
  (patch: ps-wrap/third-party/pswraprife/rife-metal-v0.1.8-pswrap.patch). See rife-metal-LICENSE-Apache-2.0.txt.
- Practical-RIFE v4.25 weights by hzwer (MIT), converted to .rmw by rife-metal. See rife-metal-THIRD-PARTY.md.
NOTE
codesign --force -s - "$APP/Contents/Frameworks/libPsWrapRife.dylib" >/dev/null 2>&1
echo "RIFE bundled into $APP"
