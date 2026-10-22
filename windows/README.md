# Code Net Receiver for Windows

Turns a Windows PC into a Code Net receiver, doing the same job as a Raspberry Pi: it follows a
main and a backup host, fails over between them, and regenerates timecode on

- **Audio (LTC)**: any output of a sound card or audio interface (pick the channel, or All), or
- **MIDI (MTC)**: a MIDI port such as a Rosendahl MIF4, which turns it into LTC.

It reports to every Code Net Mac it hears, so it appears in their receiver lists and can be
reassigned, and its output offset set, from any of them. 25 and 30 fps only; no freewheel or
hold, as on the Pi.

## Install

1. Download **Code Net Receiver for Windows** from the latest build (GitHub > Actions >
   windows) and unzip it anywhere, for example `C:\Program Files\Code Net`.
2. Run **Code Net Receiver.exe**. Windows SmartScreen may say it protected your PC, because the
   app isn't signed: click **More info**, then **Run anyway**.
3. When Windows Firewall asks, allow it on **private networks** (and public, if the show network
   is marked public). Without this no timecode arrives.

## Use

- Give the receiver a name (up to 16 bytes, for example `foh-pc`).
- Pick its **Main** and **Backup** hosts once they appear (a host appears as soon as the PC hears
  it), or assign them from the Code Net app on a Mac.
- Pick an **Output**. For LTC choose the channel and level.
- **Output offset** starts each frame this much earlier to cancel the output device's delay, so
  this PC lines up with the other receivers. Hosts can set it from their receiver list.
- **Wi-Fi mode** rides through Wi-Fi sized gaps (6 frames); use wired networking for shows.
- Closing the window keeps the receiver running in the system tray. Right-click the tray icon
  to quit. **Start with Windows** starts it in the tray at sign-in.

Settings are kept in `%APPDATA%\Code Net\Code Net Receiver.settings`.

## Timing

LTC goes out through WASAPI in shared mode at the device's own sample rate, timed from the
device's playback position, so Windows' mixer delay is already allowed for. Some drivers add
delay they don't report; measure it against a Pi with a scope or readback and enter it as the
output offset. MTC is sent by a high-priority thread with 1 ms timer resolution, so expect
around a millisecond of jitter on the MIF4 path.
