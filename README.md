# PandaVent OS

Open firmware for the [BIQU Panda Vent](https://neo.bttwiki.com/en/docs/panda-series/module/panda-status-p2/),
built as an exact re-creation of the factory application first, and extended
only after that copy was proven on real hardware.

The factory web UI is served byte-for-byte from this firmware, the factory
WebSocket protocol is spoken unchanged, and the stock partition layout is kept
exactly as BIQU ships it. That last point matters: it is what lets you go back
to the factory firmware with a single upload, and nothing in this project is
worth giving that up for.

---

## What it adds

**Lighting.** Eighteen effects instead of seven. The stock Static, Breathing,
Strobing, Wave, Marquee, Color Cycle and Rainbow keep their original ids and
behaviour, joined by Cylon, Bounce, Progress Bar (driven by the printer's own
completion percentage), and four outward/inward pairs: Marquee, Fill, Bounce
and Bounce Fill.

**Two colours per effect.** Every effect carries a vent-open colour and a
vent-closed colour, with buttons to sync one onto the other. The strip tells
you where the flap is without the effect having to change.

**Vent control.** Open, Closed and Auto from the web page, with a live status
card showing where the flap actually is as opposed to what was asked for.
Auto is the factory rule: open while printing or paused.

**Material-aware venting.** Optional. Reads the loaded filament from the
printer's AMS and decides per material whether to vent or seal, with a
residual-heat hold on the bed temperature. Nine materials, each switchable,
and a master switch that restores stock venting exactly when it is off.

**Vent button ring light.** Five modes: the factory behaviour, always on,
always off, on while the vent is open, and off while the vent is closed, plus
a toggle for the manual-mode blink.

**Twenty-four languages.** English and Simplified Chinese as shipped, plus
Spanish, French, German, Italian, Portuguese, Dutch, Polish, Indonesian,
Filipino, Vietnamese, Japanese, Korean, Cantonese, Russian, Ukrainian,
Serbian, Greek, Arabic (right-to-left), Thai, Hindi, Bengali and Punjabi.

**Network backup.** `GET /backup` streams the entire 4 MB flash in about
seventeen seconds, so a full image no longer needs a cable.

**Quality-of-life.** A device name of your choosing, a settings snapshot that
survives every firmware update, and a warning banner if the device ever fails
to persist a setting.

---

## Before you install anything

**Take a full-flash backup and test restoring it.** Not the app: the whole
4 MB, offset 0 to 0x400000, including the bootloader, partition table, otadata,
both app slots and NVS. BIQU publishes only the app image, so if you lose the
bootloader there is nothing public to put back.

    python -m esptool --chip esp32 --port <YOUR-PORT> -b 115200 \
      read-flash 0 0x400000 golden-full-4MB.bin

Read `firmware/SAFETY.md` in full before the first flash. It is short, and
every rule in it is there because skipping it cost somebody a working vent.

---

## Installing

### First install, over USB

The Panda Vent uses a CH34x USB-serial bridge; on macOS you may need the
vendor VCP driver before a `/dev/cu.wchusbserial*` node appears.

    git clone https://github.com/jeremykenedy/PandaVentOS.git
    cd PandaVentOS/firmware
    idf.py build
    idf.py -p <YOUR-PORT> flash

Built against ESP-IDF v5.3.1, target ESP32.

### Updating, over the network

Once it is running, every later update is a single upload. No cable.

    curl -X POST -H 'X-OTA-Type: ota_fw' \
      --data-binary @firmware/build/panda_vent.bin \
      http://<device>/ota

The device reboots itself when the image lands. Your settings are migrated
forward automatically; see below.

### Going back to the factory firmware

    curl -X POST -H 'X-OTA-Type: ota_fw' \
      --data-binary @factory/firmware/panda_vent_v1.0.0.bin \
      http://<device>/ota

---

## First-time setup

1. Power the vent. It raises a hotspot named `Panda_Vent_<MAC>`.
2. Join it and open `http://192.168.4.1`.
3. Pick a language, choose your Wi-Fi, enter its password.
4. Bind the printer: scan, pick it, and enter its LAN-mode access code.
5. From then on reach it at `http://<hostname>.local` or its IP.

---

## Settings are migrated, not discarded

The stored configuration has changed shape seven times. Every older layout is
read and lifted forward field by field, so updating never asks you to set the
device up again. `tools/cfgmig` runs a real stored blob through the real
migration so that claim can be tested rather than believed.

The configuration lives in a 12 KB NVS partition shared with the Wi-Fi stack,
and `PV_CFG_MAX_BYTES` in `pv_cfg.c` fails the build if it grows past its
budget. That check exists because the config once outgrew the partition, saves
began failing, and a reboot lost everything. It cannot happen quietly again:
a failed save is reported in the state document and shown on the page.

---

## Layout

    factory/     the stock firmware, its web UI and BIQU's documentation,
                 kept as the reference this project is measured against
    firmware/    the ESP-IDF application
    tools/       host harnesses: effect renderer, config-migration runner,
                 UI comparison, NVS reader
    private/     gitignored; where device-specific backups and snapshots go

---

## Licence and attribution

The contents of `factory/` are BIQU's, included for reference under the terms
in `factory/docs/BIQU-LICENSE.md`. This project is not affiliated with or
endorsed by BIQU or BigTreeTech.
