# Code Net

Network-based routing of SMPTE LTC timecode from Ableton Live to Raspberry Pi receivers.

Each Mac running Ableton decodes the LTC audio file playing in the session and sends
small, timestamped frame packets over a dedicated VLAN. Each Raspberry Pi receiver follows
a main and a backup host and generates fresh LTC locally, through a USB soundcard or a
Rosendahl MIF4.

See the design doc for the full architecture.
