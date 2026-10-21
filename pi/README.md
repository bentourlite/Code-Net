# Raspberry Pi receiver set-up

## Install

On a fresh Raspberry Pi OS (Bookworm) with network access:

```sh
git clone -b phase-1-core https://github.com/bentourlite/code-net.git   # or main once merged
cd code-net
bash pi/install.sh
```

The script installs the system packages and the Python environment, creates
`/etc/codenet/receiver.json`, installs the `codenet-receiver` systemd service and
puts `codenet-menu` on the path. It can also open the menu automatically when you
log in over SSH. Run it again after a `git pull` to update.

## The menu

```sh
ssh <user>@<pi address>
codenet-menu
```

| Item | What it does |
| --- | --- |
| Start receiver | Checks the settings (offers to set them up if incomplete), starts or restarts the service, offers to start it at boot and opens the live monitor |
| Stop receiver | Stops the service |
| Live monitor | Refreshes every second: output timecode, which host is followed, each host's address, state, last packet, longest packet gap, clock offset and round trip, readback confirmation, other hosts heard, recent switches |
| Receiver settings | Name, main and backup hosts, soundcard or MIF4, output level, readback input, device delay, multicast or unicast, Wi-Fi testing mode, auto-return. Saves to `/etc/codenet/receiver.json` and offers to restart the service |
| Network and IP addresses | Shows each interface and its address; set a static IP or DHCP per interface (through NetworkManager), turn Wi-Fi power saving off permanently |
| Service: start at boot, logs | Start, restart or stop the receiver, start it at boot, show or follow its log |
| List audio and MIDI devices | What the Pi can see |
| Return to main host now | Asks the receiver to switch back from backup to main |

`codenet-menu --monitor` goes straight to the live monitor.

Changing the IP address of the interface you are connected through ends your SSH
session; reconnect to the new address.

## Files

| Path | Contents |
| --- | --- |
| `/etc/codenet/receiver.json` | Receiver settings (edited by the menu) |
| `/run/codenet/status.json` | Live status, written every second by the receiver |
| `/etc/systemd/system/codenet-receiver.service` | The service |
