# Code Net

Network-based routing of SMPTE LTC timecode from Ableton Live to Raspberry Pi receivers.

Each Mac running Ableton decodes the LTC audio file playing in the session and sends
small, timestamped frame packets over a dedicated VLAN. Each Raspberry Pi receiver follows
a main and a backup host and generates fresh LTC locally, through a USB soundcard or a
Rosendahl MIF4.

See the design doc for the full architecture.

## Repository layout

| Path | Contents |
| --- | --- |
| `docs/protocol.md` | The wire protocol |
| `python/` | Raspberry Pi receiver, shared protocol and LTC code, test sender |
| `pi/` | Pi install script, systemd service and menu guide |
| `cpp/core/` | C++ LTC decoder and frame packets, shared by the plugin |
| `mac/plugin/` | Code Net Tap, the Ableton plugin (AU and VST3) |
| `mac/app/` | Code Net, the Mac menu-bar app with the receiver dashboard |

See `mac/README.md` to build and install the Mac side.
