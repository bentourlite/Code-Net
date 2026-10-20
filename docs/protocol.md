# Code Net wire protocol, version 1

All messages are UDP, big-endian, and start with the 4-byte magic `CNET`, a version
byte (1) and a message type byte.

| Port | Direction | Use |
| --- | --- | --- |
| 5260 | host to receivers | Frame packets, multicast to `239.255.60.60` by default, or unicast |
| 5261 | receivers to host | Clock sync requests; the host replies to the sender's address |

## Frame packet (type 1, 46 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic `CNET` |
| 4 | 1 | Version (1) |
| 5 | 1 | Type (1) |
| 6 | 1 | Flags: bit 0 `PLAYING`, bit 1 `SOURCE_LOST` |
| 7 | 1 | Frame rate (25 or 30) |
| 8 | 16 | Source id: the host's name, UTF-8, zero padded |
| 24 | 2 | Stream id (0 for now) |
| 26 | 4 | Sequence number |
| 30 | 4 | Timecode as frames since 00:00:00:00 |
| 34 | 4 | LTC user bits |
| 38 | 8 | Frame start time, host clock, nanoseconds (signed) |

The host sends one packet for every LTC frame it decodes from the audio file
playing in Ableton. The frame start time is the host clock reading at which that
frame's first sample leaves the host's audio output.

A frame can only be decoded once all 80 bits have been heard, so each packet arrives
about one frame after the frame began. Receivers therefore generate the next frames
themselves from the newest packet: frame `N + k` starts at
`start(N) + k * 1 s / rate`. They generate at most `max_predict_frames` (default 2)
beyond the newest packet; after that the output stops. There is no freewheel and no hold.

When no LTC is playing, the host keeps sending packets at the frame rate without
the `PLAYING` flag, carrying the last position. Receivers treat this as a deliberate
stop, not as a lost source. `SOURCE_LOST` means the host app is running but its
Ableton plugin is not delivering; receivers fail over at once.

## Clock sync request (type 2, 34 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 6 | 16 | Receiver id |
| 22 | 4 | Sequence number |
| 26 | 8 | t1: receiver clock when sent |

## Clock sync response (type 3, 50 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 6 | 16 | Host id |
| 22 | 4 | Sequence number (echoed) |
| 26 | 8 | t1 (echoed) |
| 34 | 8 | t2: host clock when the request arrived |
| 42 | 8 | t3: host clock when the response was sent |

The receiver notes t4 on arrival and computes
`offset = ((t2 - t1) + (t3 - t4)) / 2` and `round trip = (t4 - t1) - (t3 - t2)`.
It keeps the last 16 exchanges per host and uses the offset from the one with the
shortest round trip. Receivers sync with each of their hosts separately, about four
times a second.

## Failover (receiver side)

- A source is valid while packets keep arriving (a gap under 2 frame periods), its
  clock has been synced, and `SOURCE_LOST` is not set.
- The receiver follows main while main is valid, otherwise backup if valid,
  otherwise nothing (silence).
- It returns from backup to main once main has been valid for 10 s, or at once on an
  operator request. Auto-return can be turned off.
- Because the loss timeout (2 frames) is no longer than the prediction window
  (2 frames), a switch to a running backup lands before the output runs out.
