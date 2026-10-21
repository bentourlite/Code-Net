#!/bin/bash
# Build the Code Net plugin (AU + VST3) and the menu-bar app into mac/dist.
# Needs Xcode command line tools and CMake. Usage: mac/build.sh [--install]
set -euo pipefail
cd "$(dirname "$0")"
DIST="$PWD/dist"
rm -rf "$DIST" && mkdir -p "$DIST"

cmake -S plugin -B plugin/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build plugin/build --config Release --parallel
ARTEFACTS=plugin/build/CodeNetTap_artefacts/Release
cp -R "$ARTEFACTS/AU/Code Net Tap.component" "$ARTEFACTS/VST3/Code Net Tap.vst3" "$DIST/"

(cd app && swift build -c release --arch arm64 --arch x86_64)
BIN=$(cd app && swift build -c release --arch arm64 --arch x86_64 --show-bin-path)
APP="$DIST/Code Net.app"
mkdir -p "$APP/Contents/MacOS"
cp "$BIN/CodeNetHost" "$APP/Contents/MacOS/"
cp app/Support/Info.plist "$APP/Contents/"

# Ad-hoc signatures so Gatekeeper and Ableton will load them on this Mac.
for b in "$APP" "$DIST/Code Net Tap.component" "$DIST/Code Net Tap.vst3"; do
  codesign --force --deep --sign - "$b"
done

if [[ "${1:-}" == "--install" ]]; then
  mkdir -p ~/Library/Audio/Plug-Ins/Components ~/Library/Audio/Plug-Ins/VST3
  rm -rf ~/Library/Audio/Plug-Ins/Components/"Code Net Tap.component" ~/Library/Audio/Plug-Ins/VST3/"Code Net Tap.vst3" "/Applications/Code Net.app"
  cp -R "$DIST/Code Net Tap.component" ~/Library/Audio/Plug-Ins/Components/
  cp -R "$DIST/Code Net Tap.vst3" ~/Library/Audio/Plug-Ins/VST3/
  cp -R "$APP" /Applications/
  killall -9 AudioComponentRegistrar 2>/dev/null || true
  echo "Installed. Rescan plugins in Ableton, then open Code Net from Applications."
fi
echo "Built into $DIST"
