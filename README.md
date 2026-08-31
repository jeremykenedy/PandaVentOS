<p align="center">
    <picture>
        <source media="(prefers-color-scheme: dark)" srcset="screenshots/banner-dark.svg">
        <source media="(prefers-color-scheme: light)" srcset="screenshots/banner-light.svg">
        <img src="screenshots/banner-light.svg" alt="PandaVent OS" width="820">
    </picture>
</p>

<p align="center">
Open firmware for the BIQU Panda Vent. An exact re-creation of the factory application,<br>
then 18 lighting effects, material aware venting, printer control, and 24 languages on top of it.
</p>

<p align="center">
    <a href="https://github.com/jeremykenedy/PandaVentOS/releases"><img src="https://img.shields.io/github/v/release/jeremykenedy/PandaVentOS?display_name=tag&amp;sort=semver&amp;color=2e7d32" alt="Latest Release"></a>
    <a href="LICENSE.md"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
    <a href="#requirements"><img src="https://img.shields.io/badge/ESP--IDF-v5.3.1-informational" alt="ESP-IDF v5.3.1"></a>
    <a href="#requirements"><img src="https://img.shields.io/badge/target-ESP32-blue" alt="Target ESP32"></a>
    <a href="#languages"><img src="https://img.shields.io/badge/languages-24-orange" alt="24 languages"></a>
</p>

## Table of Contents

- [What Makes It Different](#what-makes-it-different)
- [Features](#features)
  - [Lighting](#lighting)
  - [Venting](#venting)
  - [Printer](#printer)
  - [Device](#device)
- [Languages](#languages)
- [Screenshots](#screenshots)
- [Requirements](#requirements)
- [Installation](#installation)
  - [Back Up First](#back-up-first)
  - [First Install, By Hand](#first-install-by-hand)
  - [Updating, Over The Network](#updating-over-the-network)
  - [Going Back](#going-back)
- [First Time Setup](#first-time-setup)
- [Settings Migration](#settings-migration)
- [Documentation](#documentation)
- [Project Layout](#project-layout)
- [Testing](#testing)
- [License](#license)

## What Makes It Different

The factory application is re-created first, exactly, and everything below is
added on top of that copy. The stock partition layout is untouched, so one
upload puts BIQU's firmware back.

| | Factory v1.0.0 | PandaVent OS |
| --- | :---: | :---: |
| **Lighting effects** | 7 | **18** |
| **Colours per effect** | 1 | **4** |
| **Vent aware colour** | :x: | :white_check_mark: separate open and closed colour |
| **Unlit pixel colour** | always black | :white_check_mark: settable, per effect |
| **Per strip LED count** | fixed at 16 | :white_check_mark: set each strip |
| **Strip direction** | forward only | :white_check_mark: 3 reverse modes |
| **Custom animation upload** | :x: | :white_check_mark: |
| **Brightness ramp** | :x: fixed brightness | :white_check_mark: ramps over each cycle |
| **Two runs as one strip** | :x: each renders separately | :white_check_mark: contiguous mode |
| **Fault colour** | red at 127, fixed | :white_check_mark: any colour, brightness, solid or strobe |
| **Warning temperature** | 50 °C, compiled in | :white_check_mark: yours |
| **Warning gradient range** | compiled in | :white_check_mark: both ends settable |
| **Vent modes** | Open, Closed, Auto | Open, Closed, Auto |
| **AUTO rule** | open while printing | :white_check_mark: plus per material rules |
| **Reads the AMS** | :x: | :white_check_mark: 9 materials, each switchable |
| **Residual heat hold** | :x: | :white_check_mark: bed temperature hysteresis |
| **Live flap position** | :x: target only | :white_check_mark: target, actual and travelling |
| **Endstop recalibration** | button only | :white_check_mark: from the web page |
| **Vent button ring light** | 1 behaviour | :white_check_mark: 5 modes |
| **Printer fan control** | :x: | :white_check_mark: part, aux and chamber |
| **Print speed control** | :x: | :white_check_mark: 4 levels |
| **Chamber light control** | :x: | :white_check_mark: |
| **Printer telemetry** | connection only | :white_check_mark: temps, layer, ETA, HMS errors |
| **Languages** | 2 | **24**, Arabic RTL |
| **Device name** | fixed | :white_check_mark: yours |
| **Full flash backup** | USB cable only | :white_check_mark: `GET /backup`, about 17 seconds |
| **Plain restart** | power cycle | :white_check_mark: from the web page |
| **Failed save reporting** | silent | :white_check_mark: reported and shown |
| **Settings across updates** | :x: | :white_check_mark: migrated field by field |
| **Web UI** | BIQU stock | Material 3, dark mode, masonry dashboard |
| **Partition layout** | stock | :white_check_mark: identical |
| **Source published** | :x: | :white_check_mark: MIT |

## Features

### Lighting

| | |
| --- | --- |
| **Stock effects, unchanged ids** | Static, Breathing, Strobing, Wave, Marquee, Color Cycle, Rainbow |
| **Added effects** | Cylon, Bounce, Progress Bar, Marquee Out, Marquee In, Fill Out, Fill In, Bounce Out, Bounce In, Bounce Fill Out, Bounce Fill In |
| **Progress Bar** | driven by the printer's own `mc_percent`, not a timer |
| **Four colours per effect** | active and inactive, for vent open and vent closed, with sync buttons |
| **Inactive colour** | optional; unset renders bit identical to the stock renderer |
| **Per strip length** | each of the two strips has its own count, one shared animation phase |
| **Direction** | forward, reversed, or mirrored, per strip |
| **Temperature warning** | stock Mode 3 preserved |
| **H2D advanced mode** | stock Mode 2 preserved, six state blobs |
| **Custom animation** | upload frames and play them from RAM |
| **Brightness ramp** | optional; ramps from a start to an end brightness over each cycle |
| **Contiguous** | treat the two runs as one strip so the light travels the whole length once |
| **Fault colour** | any colour, any brightness, solid or strobing, instead of stock's fixed red |

### Venting

| | |
| --- | --- |
| **Manual** | Open and Closed from the page or the button |
| **Auto** | the factory rule: open while printing or paused |
| **Material aware** | optional layer on top of Auto, off by default preserves stock exactly |
| **Vents** | PLA, PETG, PET, TPU |
| **Seals** | ABS, ASA, PC, PA, HIPS |
| **Unmatched filament** | falls back to the stock answer, never guesses |
| **Residual heat hold** | holds open until the bed cools, 45 °C open and 35 °C close, both editable |
| **Live dial** | shows where the flap actually is, and spins while it travels |
| **Endstop** | recalibrate from the page |
| **Ring light** | factory behaviour, always on, always off, on while open, off while closed |

### Printer

| | |
| --- | --- |
| **Link** | Bambu LAN MQTT over TLS, scan and bind or enter by hand |
| **Reads** | state, progress, layer, ETA, nozzle, bed and chamber temperature, Wi-Fi signal, AMS trays, HMS and print errors |
| **Fans** | part cooling, auxiliary, chamber |
| **Speed** | silent, standard, sport, ludicrous |
| **Chamber light** | on and off |
| **Requirement** | the printer must be in LAN Only Mode or it refuses every command |

### Device

| | |
| --- | --- |
| **Name** | set your own, used for mDNS and the page title |
| **Wi-Fi** | STA with static IP option, plus the setup hotspot |
| **Backup** | `GET /backup` streams the whole 4 MB image over the network |
| **Restart** | plain restart, and factory reset |
| **OTA** | one upload, no cable, settings carried forward |
| **Save failures** | reported in the state document and shown on the page |
| **Logs** | live device log in the browser |

## Languages

Twenty four, every one of them at full coverage. There is no partial
translation and no English fallback hiding in a corner: a language ships when
all 437 strings are in it, and a build check fails if one is missing.

| Language | Native name | Code | |
| --- | --- | :---: | --- |
| English | English | `en` |  |
| Chinese (Simplified) | 简体中文 | `zh` |  |
| Spanish | Español | `es` |  |
| French | Français | `fr` |  |
| German | Deutsch | `de` |  |
| Italian | Italiano | `it` |  |
| Portuguese | Português | `pt` |  |
| Dutch | Nederlands | `nl` |  |
| Polish | Polski | `pl` |  |
| Indonesian | Bahasa Indonesia | `id` |  |
| Filipino | Filipino | `fil` |  |
| Vietnamese | Tiếng Việt | `vi` |  |
| Japanese | 日本語 | `ja` |  |
| Korean | 한국어 | `ko` |  |
| Cantonese | 粵語 | `yue` |  |
| Russian | Русский | `ru` |  |
| Ukrainian | Українська | `uk` |  |
| Serbian | Српски | `sr` |  |
| Greek | Ελληνικά | `el` |  |
| Arabic | العربية | `ar` | right to left |
| Thai | ไทย | `th` |  |
| Hindi | हिन्दी | `hi` |  |
| Bengali | বাংলা | `bn` |  |
| Punjabi | ਪੰਜਾਬੀ | `pa` |  |

Arabic is right to left. The whole layout mirrors, not just the text.

Pick a language on first boot, or change it any time from the Settings page.

## Screenshots

| Dashboard | Lighting |
| --- | --- |
| <img src="screenshots/dashboard-light.png" alt="Dashboard"> | <img src="screenshots/lighting-light.png" alt="Lighting"> |

| Vent Policy | Dark Mode |
| --- | --- |
| <img src="screenshots/policy-light.png" alt="Vent policy"> | <img src="screenshots/dashboard-dark.png" alt="Dark mode"> |

## Requirements

| | |
| --- | --- |
| **Hardware** | BIQU Panda Vent, ESP32-U4WDH, 4 MB flash |
| **Toolchain** | ESP-IDF v5.3.1, target ESP32 |
| **Printer** | Bambu Lab, in LAN Only Mode |
| **USB** | CH34x serial bridge; macOS may need the vendor VCP driver |

## Installation

There is a script, and it takes a backup before it writes anything.

```bash
git clone https://github.com/jeremykenedy/PandaVentOS.git
cd PandaVentOS
tools/install.sh
```

| It does | |
| :---: | --- |
| 1 | finds your device on USB, or takes `--port` |
| 2 | reads the **whole 4 MB** off it and verifies the size |
| 3 | tells you where that backup is, and that it holds your Wi-Fi password |
| 4 | builds, or takes a release image with `--image` |
| 5 | asks once more, then flashes |

Any failure before step 5 leaves the device untouched. The backup is not
optional and there is no flag to skip it.

### Back Up First

If you would rather do it by hand, this is the part that matters. The whole
4 MB, offset 0 to 0x400000, including the bootloader, partition table, otadata,
both app slots and NVS. BIQU publishes only the app image, so if you lose the
bootloader there is nothing public to put back.

```bash
python3 -m esptool --chip esp32 --port <YOUR-PORT> -b 115200 \
  read-flash 0 0x400000 golden-full-4MB.bin
```

Read [`firmware/SAFETY.md`](firmware/SAFETY.md) before the first flash. It is
short, and every rule in it is there because skipping it cost somebody a
working vent.

### First Install, By Hand

```bash
cd firmware
idf.py build
idf.py -p <YOUR-PORT> flash
```

### Updating, Over The Network

Every later update is a single upload. No cable.

```bash
tools/install.sh --network <device-ip>
```

That takes a fresh backup over the network first, then uploads, then waits for
the device to come back. By hand it is:

```bash
curl -X POST -H 'X-OTA-Type: ota_fw' \
  --data-binary @firmware/build/panda_vent.bin \
  http://<device>/ota
```

Settings are migrated forward automatically.

### Going Back

```bash
tools/restore.sh --factory <device-ip>       # BIQU's firmware, over the network
tools/restore.sh --factory --port /dev/...   # BIQU's firmware, over a cable
tools/restore.sh --image <your-backup.bin>   # your own 4 MB image, cable only
tools/restore.sh --list                      # what backups you have
```

`--factory` puts BIQU's application image back. It does not restore your
settings: the stock firmware does not understand this one's configuration and
replaces it with its own defaults.

`--image` writes a full 4 MB image taken off your own device, which is the only
thing that recovers a device that will not boot.

## First Time Setup

| Step | |
| :---: | --- |
| 1 | Power the vent. It raises a hotspot named `Panda_Vent_<MAC>`. |
| 2 | Join it and open `http://192.168.4.1`. |
| 3 | Pick a language, choose your Wi-Fi, enter its password. |
| 4 | Bind the printer: scan, pick it, enter its LAN mode access code. |
| 5 | Reach it afterwards at `http://<hostname>.local` or its IP. |

## Settings Migration

The stored configuration has changed shape several times. Every older layout is
read and lifted forward field by field, so updating never asks you to set the
device up again. `tools/cfgmig` runs a real stored blob through the real
migration so that claim can be tested rather than believed.

The configuration lives in a 12 KB NVS partition shared with the Wi-Fi stack,
and `PV_CFG_MAX_BYTES` fails the build if it grows past its budget. That check
exists because the config once outgrew the partition, saves began failing, and a
reboot lost everything. It cannot happen quietly again: a failed save is
reported in the state document and shown on the page.

## Documentation

Every setting, what it does, and why it is there.

| Document | Covers |
| --- | --- |
| [`docs/lighting.md`](docs/lighting.md) | Modes, all 18 effects, the four colours, brightness ramps, direction, strip length |
| [`docs/venting.md`](docs/venting.md) | Vent modes, the nine material rules, residual heat hold, the ring light, endstop |
| [`docs/printer.md`](docs/printer.md) | Binding, LAN Only Mode, what it reads, what it can change |
| [`docs/network.md`](docs/network.md) | Wi-Fi, static IP, mDNS, the setup hotspot, what it talks to |
| [`docs/backup.md`](docs/backup.md) | Full image, settings snapshot, restore, going back to stock |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | It does not do the thing. Start here |
| [`firmware/SAFETY.md`](firmware/SAFETY.md) | What will brick the device, and what will not |
| [`firmware/PROTOCOL.md`](firmware/PROTOCOL.md) | Every WebSocket message shape, stock and added |

## Project Layout

| Path | |
| --- | --- |
| `factory/` | the stock firmware, its web UI and BIQU's documentation, kept as the reference this project is measured against |
| `firmware/` | the ESP-IDF application |
| `tools/` | `install.sh`, `restore.sh`, and the host harnesses: effect renderer, config migration runner, UI comparison |
| `docs/` | what every setting actually does |
| `screenshots/` | images used by this page |
| `private/` | gitignored; where device specific backups and snapshots go |

## Testing

| Harness | |
| --- | --- |
| `tools/fxdump` | compiles the real `pv_rgb.c` and prints frames as ASCII |
| `tools/cfgmig` | runs a real stored NVS blob through the real migration |
| `tools/uicmp` | compares the served page against the factory page |
| headless suite | the web app driven against a mock replaying captured device state |

## License

Released under the [MIT license](LICENSE.md).

The contents of `factory/` are BIQU's, included for reference under the terms in
`factory/docs/BIQU-LICENSE.md`. Icons are [Heroicons](https://heroicons.com),
MIT. This project is not affiliated with or endorsed by BIQU or BigTreeTech.
