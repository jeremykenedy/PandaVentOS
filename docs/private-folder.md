# Keeping your device's files out of the repository

`private/` is where anything tied to YOUR device goes. It is listed in
`.gitignore`, and **nothing inside it is tracked** -- not even a README, which
is why this document lives here in `docs/` instead.

Create it when you need it:

    mkdir -p private

## What belongs here

Anything tied to YOUR device rather than to the firmware:

* full-flash backup images (`GOLDEN-*.bin`) taken with `/backup` or over USB
* settings snapshots captured from the WebSocket
* notes holding your Wi-Fi credentials, printer serial or access code

## Why

A full-flash image contains the NVS partition, and NVS stores the Wi-Fi
password, the printer serial number and the printer's LAN access code **in
plain text**. A settings snapshot contains the same three values. Committing
either publishes all of them.

There is nothing sensitive in the firmware itself. The AP defaults it ships
with are the stock ones from the manual: SSID `Panda_Vent_` plus the device's
own MAC address, password `987654321`, address `192.168.254.1`. Those are
public, identical on every unit, and meant to be.
