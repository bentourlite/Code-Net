# Bench test

How to try Phase 1 on real hardware. The Ableton plugin is not built yet, so a
laptop runs the **test sender** in its place: it plays LTC onto the network exactly
as the plugin will.

## What you need

| Item | Notes |
| --- | --- |
| A Mac (or any computer) with Python 3.10 or newer | Stands in for the Ableton host |
| Raspberry Pi 4 or 5 with Raspberry Pi OS (Bookworm, Lite is fine) | The receiver |
| A network switch, both on wired Ethernet | Same VLAN / subnet |
| A USB soundcard for the Pi | For the LTC audio output |
| Something that reads LTC | The MIF4's display, a lighting console, or any LTC reader |
| Optional: Rosendahl MIF4 | For the MTC path |
| Optional: a second computer | For the failover test |

## 1. Get the code

The code is on the `phase-1-core` branch until PR #1 is merged.

```sh
git clone -b phase-1-core https://github.com/bentourlite/code-net.git
cd code-net/python
```

## 2. Set up the Pi

Run these one line at a time, not as a pasted block, so a failure is easy to spot.

```sh
sudo apt update
sudo apt install -y git python3-venv libportaudio2 libasound2-dev libopenblas0
git clone -b phase-1-core https://github.com/bentourlite/code-net.git
cd code-net/python
python3 -m venv .venv
.venv/bin/pip install -e '.[pi,dev]'
.venv/bin/pytest -q                      # should pass
.venv/bin/python -m sounddevice          # lists audio devices; note the USB soundcard's name
```

## 3. Set up the Mac

```sh
cd code-net/python
python3 -m venv .venv
.venv/bin/pip install -e '.[dev]'
```

If macOS asks whether Python may accept incoming network connections, allow it
(the Pi sends clock sync requests to the Mac).

## 4. Basic test: LTC out of the Pi

On the Mac, start sending generated timecode:

```sh
.venv/bin/codenet-sender --name mac-a --generate 01:00:00:00 --rate 25
```

Or play your own LTC audio file (16, 24 or 32-bit PCM WAV, 25 or 30 fps):

```sh
.venv/bin/codenet-sender --name mac-a --wav /path/to/ltc.wav
```

On the Pi, use part of the soundcard's name from step 2:

```sh
.venv/bin/codenet-receiver --name pi-1 --main mac-a --output soundcard --device "USB"
```

What to expect:

- The Pi logs `host mac-a is at <ip>`, then `now following main`, then
  `output 01:00:00:xx` once a second.
- Connect the soundcard output to your LTC reader; it should show the same running
  timecode. Use `--level 0.3` to `--level 1.0` if the reader wants a different level.
- Stop the sender (Ctrl-C): the Pi logs `now following nothing` and the LTC
  output goes silent within 2 frames.

If nothing arrives, the network may be blocking multicast. Send directly to the Pi
instead: add `--to <pi ip address>` on the Mac and `--group none` on the Pi.

### Testing over Wi-Fi

Wi-Fi power saving holds packets back and releases them in bursts, with gaps of
100 ms or more. The receiver then drops the host and the LTC cuts in and out. For
desk testing over Wi-Fi:

```sh
sudo iw wlan0 set power_save off         # on the Pi, after every reboot
.venv/bin/codenet-receiver --name pi-1 --main mac-a --output soundcard --device "USB" --wifi
```

`--wifi` lets the receiver ride through gaps of up to 6 frames (240 ms at 25 fps).
The cost is that the output carries on for up to 6 frames after the host really
stops. Use a wired network and the default settings for shows.

## 5. Failover test

Run a second sender on another computer at the same position:

```sh
.venv/bin/codenet-sender --name mac-b --generate 01:00:00:00 --rate 25
```

Start the two senders as close together as you can, then restart the receiver with a
backup:

```sh
.venv/bin/codenet-receiver --name pi-1 --main mac-a --backup mac-b --output soundcard --device "USB"
```

Stop `mac-a`. The Pi logs `now following backup (main lost)`. The LTC reader keeps
running and shows whatever position `mac-b` is at. Start `mac-a` again; after 10
seconds the Pi returns to main. Add `--no-auto-return` to keep it on backup instead.

Two generated senders won't be in step, because each one starts when you launch it,
so the reader may jump at the switch. In a real show both Macs play the same LTC
file in sync.

## 6. MIF4 test (MTC over USB)

Plug the MIF4 into the Pi by USB and set it to read MTC from USB (see its manual).
Then:

```sh
.venv/bin/codenet-receiver --name pi-1 --main mac-a --output mif4
```

The Pi logs `sending MTC to <port>`. The MIF4's display and its LTC XLR output
should follow the sender. Use `--midi-port` if the port name doesn't contain "MIF".

### Output confirmation

Loop the MIF4's LTC XLR output into a USB soundcard's line input on the Pi, then
add `--readback-device`:

```sh
.venv/bin/codenet-receiver --name pi-1 --main mac-a --output mif4 --readback-device "USB"
```

Each status line then ends with `readback confirmed +x.xx ms`: the LTC really leaving
the MIF4, and how far it is from where the Pi scheduled it. That number is the MIF4's
own delay; pass it back as `--advance-ms` to cancel it out. If you see
`readback missing`, nothing decodable is reaching the input. Check the cable and
input level.

## What to report back

- Whether each step worked, and the log lines where it didn't.
- The `readback ... ms` figure from step 6.
- Any `audio output underflow` warnings. Those mean the Pi needs a larger
  `--blocksize` or a real-time kernel.
