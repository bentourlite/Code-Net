# Code Net on the Mac

Two parts run on every Mac with Ableton:

- **Code Net Tap** (AU / VST3). Put it on the audio track that plays your LTC file. It decodes
  the LTC and stamps each frame with the time its first sample leaves the Mac's audio output.
  It shows the timecode it hears, a latency trim (ms) and a mute switch.
- **Code Net** (menu-bar app). Takes frames from the plugin, sends them to the receivers, sends
  stopped/lost heartbeats when Ableton stops or the plugin goes quiet, and lists every receiver
  with its state, the host it follows and its audio device (MIF4 or soundcard).

## Build and install

Needs Xcode (or its command line tools) and CMake (`brew install cmake`).

```
cd ~/code-net
git pull
mac/build.sh --install
```

This puts the plugin in `~/Library/Audio/Plug-Ins` and the app in `/Applications`. If you
download the build from GitHub Actions instead, run `xattr -cr` on each item before opening.

## Use

1. Open **Code Net** from Applications. An antenna icon appears in the menu bar (it shows radio waves while timecode is playing); allow Local
   Network access when macOS asks.
2. In the menu, set this host's name (e.g. `ableton-a`; receivers use it as main or backup) and
   either keep multicast on or list the receiver IPs.
3. In Ableton, put **Code Net Tap** on the LTC track. Rescan plugins if it is missing.
   The LTC track must still be routed to an output (send it to an unused output, or silence it
   with the plugin's mute switch; the plugin reads the audio before muting it).
4. Press play. The plugin and the app both show the running timecode, and receivers start.

The latency trim shifts every frame stamp. Leave it at 0 unless a readback measurement shows
an offset.
