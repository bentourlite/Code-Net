#!/bin/bash
# Removes Code Net (menu-bar app and Code Net Tap plugin) from this Mac so a fresh build can
# be installed. Usage: mac/uninstall.sh
set -uo pipefail

osascript -e 'quit app "Code Net"' 2>/dev/null
pkill -x CodeNetHost 2>/dev/null

if pgrep -xq "Live"; then
  echo "Quit Ableton Live first, then run this again."
  exit 1
fi

removed=0
remove() {
  for path in "$@"; do
    if [[ -e "$path" ]]; then
      if rm -rf "$path" 2>/dev/null || sudo rm -rf "$path"; then
        echo "removed $path"
        removed=$((removed + 1))
      fi
    fi
  done
}

remove "/Applications/Code Net.app" ~/Applications/"Code Net.app" \
  ~/Library/Audio/Plug-Ins/Components/"Code Net Tap.component" \
  ~/Library/Audio/Plug-Ins/VST3/"Code Net Tap.vst3" \
  /Library/Audio/Plug-Ins/Components/"Code Net Tap.component" \
  /Library/Audio/Plug-Ins/VST3/"Code Net Tap.vst3"

# Settings saved by the app (host name, targets).
defaults delete CodeNetHost 2>/dev/null && echo "removed app settings"
defaults delete com.tourlite.codenet.host 2>/dev/null && echo "removed app settings"
pkgutil --pkgs | grep -q '^com.tourlite.codenet$' && sudo pkgutil --forget com.tourlite.codenet >/dev/null

# macOS keeps a cache of every Audio Unit it has seen; it is rebuilt on the next scan.
remove ~/Library/Caches/AudioUnitCache
killall -9 AudioComponentRegistrar 2>/dev/null

echo
echo "Code Net removed ($removed items). Install the new build, then open Ableton and, in"
echo "Settings > Plug-Ins, hold Option (alt) while clicking Rescan for a full rescan."
