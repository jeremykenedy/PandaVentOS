# PandaVent factory baseline

This project starts over from zero. Phase 0 is an EXACT copy of the factory
BIQU Panda Vent application. Nothing gets built on top of it until the factory
app is re-created 1:1 and verified on the real hardware by eye. NOTHING in this
repo gets deployed to the vent until Jeremy says deploy.

## The factory artifacts (byte-exact, verified)

| artifact | sha256 | bytes |
|---|---|---|
| factory/firmware/panda_vent_v1.0.0.bin | 0f52294e00b41524e11f11236c92aebd55d3ebb0658d2affad498929dbc0c178 | 1,143,152 |
| factory/www/index.html.gz (as embedded in the bin, as served by the device) | e7d1ce4775fa9d38fdc66d11a360fbdb27d95a065e2787127800ded0b3b45faa | 66,765 |
| factory/www/index.html (unpacked) | f8568fd5bd73fcaefcd89e7c9dcf57c94d24ea3e634a51b85c5a376fab608363 | 357,456 |

Provenance and verification, 2026-08-24 evening PDT:
the bin came from github.com/bigtreetech/Panda-Vent (commit 124f056, the only
published branch) and is byte-identical to the archived copy taken from the
device before any custom firmware ever touched it. The web app was carved from
the bin (single gzip blob at image offset 0x1715e) and the RUNNING stock device
served the exact same 66,765 gzip bytes on request. The factory
UI in this repo is therefore certain to be exact, not approximate.

BIQU publishes NO source code. The repo holds only: this bin, user manuals
(EN + ZH), 3D models, and a readme. License on their files: CC-BY-NC-ND-4.0.
The factory/ folder is reference material; if this project ever goes public,
redistribution of BIQU's files has to be reviewed against that license.

## What the factory app is (from the bin + the live device)

ESP-IDF application (ESP32, image: 6 segments, entry 0x40081420). One embedded
web page: the entire UI is the single self-contained index.html above, served
gzipped from flash. Communication is ONE WebSocket at ws://<host>/ws carrying
JSON both ways, keyed by top-level object name. Top-level keys the factory UI
handles on receive: `ap`, `printer`, `response`, `rgb_mode`, `settings`, `sta`,
`wifi`, `ws_theme`. The device pushes state on connect (same pattern as the
Panda Status 2). The only non-WS call in the UI is an XMLHttpRequest upload for
OTA; the bin carries the route string `/ota`. Bambu printer link runs over MQTT
(mqtt client + `DevName.bambu.com:` strings in the bin). mDNS present.
Subsystems visible in the bin: RMT TX (WS2812 strips), LEDC (motor PWM,
`motor_pwm_init`), UART, Wi-Fi STA+AP (`Panda_Vent_` AP prefix, `PandaVent`
identity string).

## Factory feature inventory (from BIQU's own manual, factory/docs/)

Sections of the official manual, which is the acceptance checklist for the
re-creation: first-use flow (power on, hotspot `Panda_Vent_XXXX`, web UI,
language select zh/en), Wi-Fi STA config + hostname + AP settings, Bambu Lab
printer binding (IP + access code + serial, connection status), RGB lighting
(main switch; Mode 1 Simple; Mode 2 Advanced/H2D; Mode 3 Temperature Warning),
fan/motor control via button (AUTO and MANUAL modes, button press guide, motor
groups), OTA upgrade via the web page, factory reset (long-press button or web
UI, with a defined list of what it clears).

## Hardware truths that were learned the hard way (keep these)

Two WS2812 strips, 30 px each, GPIO 14 (SPI-capable) and GPIO 4, button GPIO
12, button ring LED GPIO 27, 4 motor groups (LEDC PWM + hall ADC), no display,
no fan header. EACH STRIP SNAKES THROUGH THREE VISIBLE WINDOWS: roughly px 0-9
behind the front honeycomb grille (diffuse glow), px 10-19 behind a front lens
window, px 20-29 behind the side louver run; the two strips mirror left/right.
Mapped on the real device 2026-08-24 with color-block and walking-dot test
frames and photos. Any lighting renderer that treats a strip as one bare
30-pixel line looks broken on this hardware. The factory firmware's lighting
was designed around these windows; the re-creation must reproduce the factory
LOOK on the windows, not just factory math on a strip.

## Rules for this project

1. Phase 0 (this commit): factory artifacts, exact, verified. Done.
2. Phase 1: re-create the factory app as buildable source that serves the
   byte-exact factory UI and matches factory behavior on the device, feature
   by feature against the manual checklist. Jeremy verifies by eye ONCE per
   feature on real hardware before the feature counts as done.
3. No deploys without Jeremy saying deploy. No pushes to any remote without
   Jeremy saying push. All commits authored Jeremy Kenedy
   <jeremykenedy@gmail.com>.
4. Feature work beyond factory parity starts only after Phase 1 is signed off.
