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

# With a Developer ID (CODENET_SIGN_APP / CODENET_SIGN_INSTALLER, e.g. "Developer ID Application:
# Tourlite Ltd (TEAMID)") the bundles are signed for distribution and packed into a notarised
# installer. Without one they get ad-hoc signatures, which only this Mac trusts.
BUNDLES=("$APP" "$DIST/Code Net Tap.component" "$DIST/Code Net Tap.vst3")
if [[ -n "${CODENET_SIGN_APP:-}" ]]; then
  for b in "${BUNDLES[@]}"; do
    codesign --force --options runtime --timestamp --sign "$CODENET_SIGN_APP" "$b"
  done
else
  for b in "${BUNDLES[@]}"; do codesign --force --deep --sign - "$b"; done
fi

if [[ -n "${CODENET_SIGN_INSTALLER:-}" ]]; then
  VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" app/Support/Info.plist)
  ROOT="$DIST/pkgroot"
  mkdir -p "$ROOT/Applications" "$ROOT/Library/Audio/Plug-Ins/Components" "$ROOT/Library/Audio/Plug-Ins/VST3"
  ditto "$APP" "$ROOT/Applications/Code Net.app"
  ditto "$DIST/Code Net Tap.component" "$ROOT/Library/Audio/Plug-Ins/Components/Code Net Tap.component"
  ditto "$DIST/Code Net Tap.vst3" "$ROOT/Library/Audio/Plug-Ins/VST3/Code Net Tap.vst3"
  pkgbuild --root "$ROOT" --identifier com.tourlite.codenet --version "$VERSION" --install-location / \
    "$DIST/component.pkg"
  productbuild --package "$DIST/component.pkg" --sign "$CODENET_SIGN_INSTALLER" --timestamp \
    "$DIST/Code Net $VERSION.pkg"
  rm -rf "$ROOT" "$DIST/component.pkg"
  # Notarise when Apple credentials are given (CODENET_NOTARY_PROFILE from `notarytool store-credentials`).
  if [[ -n "${CODENET_NOTARY_PROFILE:-}" ]]; then
    xcrun notarytool submit "$DIST/Code Net $VERSION.pkg" --keychain-profile "$CODENET_NOTARY_PROFILE" --wait
    xcrun stapler staple "$DIST/Code Net $VERSION.pkg"
  fi
fi

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
