# What has actually been proven about the clone

Nothing in this file is an opinion about the code. Every line is a check that
was run, with the result it produced. Anything not listed here is not proven.

Checks re-run 2026-08-25 against a live stock unit on the local network.
(Device identifiers are deliberately not recorded in this repo.)

## 1. The UI is the factory UI, byte for byte

| check | result |
|---|---|
| `factory/www/index.html.gz` sha256 | `e7d1ce...b3b45faa`, 66,765 bytes |
| same blob carved from the stock image at offset `0x1715e` | identical |
| same blob served by the running stock device on `GET /` | identical |
| same blob embedded in the built clone image | identical, found at offset `0x17f98` |

The clone does not re-implement the UI. It serves the factory bytes with
`Content-Encoding: gzip`, which is what stock does.

## 2. The state document is structurally identical

The firmware's own `main/pv_json.c` was compiled natively against cJSON with
a harness that seeds the factory defaults, and its output was compared key by
key against a live capture from the stock device.

```
live nodes: 64   mine nodes: 64
STRUCTURE IDENTICAL: same keys, same order, same types, same array lengths
```

This is not a hand-written comparison of a hand-written spec. It compiles the
shipping source file and diffs its real output.

## 3. Value comparison, and what the remaining differences are

After the structure check, every scalar was compared. The first pass found 25
value differences. Eight of them were real defects in the clone's defaults and
are now fixed:

| field | was | corrected to | authority |
|---|---|---|---|
| AP ssid format | `Panda_Vent_%02X%02X` (2 MAC bytes, SoftAP MAC) | `Panda_Vent_` + all six STA MAC bytes | format string `%s%02X%02X%02X%02X%02X%02X` in the stock image; a live unit's ssid carries all six of its STA MAC bytes |
| AP password | a 20-digit string that is not the AP password at all (redacted, see note below) | `987654321` | string present in the stock image; manual section 3 step 2; live unit |
| Hostname default | `panda_vent` | `PandaVent` | string present in the stock image; manual section 4 |
| H2D Preparation colour | `F8A323` | `FF8000` | RGB triple table at image offset `0x1707c`; live unit |
| H2D Completed colour | `00FF2A` | `00FF00` | same table; live unit |

`F8A323` and `00FF2A` appear nowhere in the shipping firmware, in any
encoding. The printed manual lists them. The manual is wrong.

The old AP password default was not merely wrong, it was a live credential
that had been pasted into the source by mistake. It has been removed. See
the note in the repository about scrubbing it from history before this repo
is published anywhere.

The 17 differences that remain are live user state on that specific unit, not
default mismatches: the light mode and active effect selections that were
changed during testing, four hand-picked colours (`1FF8FF`, `3C00FF`,
`AAFF00`, `FFB10A`), and the `FFFFFF` that effects 5 and 6 always read back
because the firmware generates their colours itself.

## 3a. Rendered side by side, the UI is pixel identical

The byte-identical file argument only proves the HTML is the same. It does
not prove the clone's state document drives that HTML correctly. So the
factory page was loaded twice in a real browser behind a stubbed WebSocket:
once fed the live stock device's state, once fed the document produced by
compiling the clone's own `main/pv_json.c` seeded with that same device's
values.

First, the documents themselves: seeded with the live values, the clone's
`pv_json_state()` output is **exactly equal** to the live capture. Not
structurally similar. Equal, key for key and value for value.

Then ten screens were rendered and captured at 2x from both:

| screen | pixel diff |
|---|---|
| Language | 0 |
| Wi-Fi setup | 0 |
| Control Panel | 0 |
| Hostname / IP | 0 |
| Hotspot (AP) | 0 |
| Printer bind | 0 |
| Settings / OTA | 0 |
| RGB Simple Mode | 0 |
| RGB Advance Mode | 0 |
| RGB Warning Hot Mode | 0 |

Every pair is not merely visually the same, it is the same PNG: the sha256 of
each stock capture equals the sha256 of its clone counterpart. Harness in
`tools/uicmp/`.

## 4. The inbound protocol matches all 44 call sites

Every `ws_send_data()` call in the factory app was extracted and grouped:
`wifi` 2, `sta` 1, `ap` 4, `printer` 3, `settings` 3, `rgb_switch` 11,
`rgb_mode` 18, plus the helper's own two generic sites. Each shape is handled
by `main/pv_apply.c`, including the field spellings that differ from the
outbound document (`bg` for brightness, `rgb` for colour, `mode` for the H2D
device state). Full grammar in `firmware/PROTOCOL.md`.

The `response.type` values the app acts on were extracted from its message
handler: `set_hostname`, `set_ap`, `set_hotspot_ip`, `factory_reset`,
`ota_fw`, `ota_img`, `ota_gif`, `ota_get_img`, `ota_unknown`. The clone emits
every one that applies to a vent. `ota_img`, `ota_gif` and `ota_get_img`
belong to the Panda Status products that share this web app; a stock vent
never sends them, and the live capture confirms it.

## 5. The build is clean

ESP-IDF v5.3.1, target esp32, stock partition layout. No warnings.
Image checksum and validation hash both report valid.
`panda_vent.bin`, 1,139,664 bytes, 44% of the app partition free.

## 6. Recovery has been proven, twice, on real hardware

* **Wireless:** `vent-restore.sh` uploads the stock image to `/ota`, waits for
  the device, replays the saved settings and verifies them field by field.
  Proven end to end in about 60 seconds.
* **Cable:** `restore-stock-usb.sh` writes the stock image to `0x10000` over
  the CH340 bridge. Required the WCH vendor driver, because macOS does not
  claim PID `0x7522`.

## 7. The limit of the safety net, proven the hard way

Rollback protection is applied by the firmware doing the INSTALL, not by the
image being installed. Stock is built without it, so stock marks anything it
installs valid immediately. **A first install of custom firmware from stock
can never self-revert.** That step needs the cable connected. Custom to custom
is protected. See `firmware/SAFETY.md` rule 0.

## What is NOT proven

* The clone has not been flashed to the device. Nothing here has run on the
  hardware.
* Lighting output, motor behaviour and the Bambu link are implemented against
  the recovered protocol and the mapped hardware, and are unverified by eye.
* Feature parity is verified against the manual and the app, not against a
  side-by-side session on the device.
