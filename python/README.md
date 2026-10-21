# Code Net Python package

The receiver that runs on each Raspberry Pi, the shared protocol and LTC code, and a
test sender that stands in for the Ableton host during bench tests.

## Install

```sh
python3 -m venv .venv
.venv/bin/pip install -e '.[dev]'        # development and tests
.venv/bin/pip install -e '.[pi]'         # on the Pi: audio and MIDI output
.venv/bin/pytest
```

On the Pi, `sounddevice` needs PortAudio (`sudo apt install libportaudio2`) and
`python-rtmidi` needs ALSA MIDI (`sudo apt install libasound2-dev`). On 32-bit Raspberry Pi OS, NumPy needs `libopenblas0`.

## Run a receiver

```sh
# LTC through a USB soundcard, following mac-a with mac-b as backup
codenet-receiver --name pi-foh-1 --main mac-a --backup mac-b --output soundcard --device "USB Audio"

# MTC to a Rosendahl MIF4, with its LTC output looped into a USB line input for confirmation
codenet-receiver --name pi-foh-2 --main mac-a --backup mac-b --output mif4 --readback-device "USB Audio"
```

`codenet-receiver --help` lists every option. Use `--advance-ms` to compensate a
measured device delay.

## Run the test sender

```sh
codenet-sender --name mac-a --wav show-ltc.wav           # an LTC audio file, 16/24/32-bit PCM WAV
codenet-sender --name mac-b --generate 01:00:00:00 --rate 25
```

## Layout

| Module | What it does |
| --- | --- |
| `codenet/timecode.py` | Timecode values at 25 and 30 fps |
| `codenet/ltc.py` | LTC encoder and streaming decoder |
| `codenet/protocol.py` | Wire format (see `docs/protocol.md`) |
| `codenet/clock.py` | Clock offset estimation |
| `codenet/receiver/core.py` | Source selection, failover and frame scheduling |
| `codenet/receiver/render.py` | Frame schedule to LTC audio, sample aligned |
| `codenet/receiver/mtc.py` | MTC messages for the MIF4 |
| `codenet/receiver/readback.py` | Output confirmation from a looped-back input |
| `codenet/receiver/devices.py` | Soundcard and MIDI device adapters |
| `codenet/receiver/service.py` | Network loop |
| `codenet/sender.py` | Test host |
