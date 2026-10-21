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

Each receiver in the list has **Main** and **Backup** menus. Choosing a host there tells that
Pi to follow it and saves it on the Pi, so a Pi only needs its name, output and network set
locally. A new Pi with no main set shows up as soon as it hears any Code Net Mac; a spinner shows
until the Pi confirms the change.

The latency trim shifts every frame stamp. Leave it at 0 unless a readback measurement shows
an offset.

## Signed installer (no "malware" warning)

macOS warns about any app that isn't signed with an Apple Developer ID and notarised. Once these
GitHub secrets are set (repo Settings > Secrets and variables > Actions), every build also produces
`Code Net <version>.pkg`, which installs the app and plugin with a double-click and no warning:

| Secret | What it is |
| --- | --- |
| `MAC_CERTS_P12` | Your *Developer ID Application* and *Developer ID Installer* certificates exported together from Keychain Access as one .p12, then `base64 -i certs.p12 \| pbcopy` |
| `MAC_CERTS_PASSWORD` | The password you set when exporting the .p12 |
| `APPLE_ID` | The Apple ID email of the developer account |
| `APPLE_TEAM_ID` | The 10-character Team ID (developer.apple.com > Membership) |
| `APPLE_APP_PASSWORD` | An app-specific password from account.apple.com > Sign-In and Security |

Create the two certificates in Xcode > Settings > Accounts > Manage Certificates (+), or at
developer.apple.com > Certificates. Locally, `mac/build.sh` does the same when
`CODENET_SIGN_APP`, `CODENET_SIGN_INSTALLER` and `CODENET_NOTARY_PROFILE` are set.

Until then, open the app the first time with right-click > Open, and allow the plugin under
System Settings > Privacy & Security > Open Anyway.
