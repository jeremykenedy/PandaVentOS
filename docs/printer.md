# Printer

The vent talks to the printer directly over your own network. Nothing goes
through Bambu's cloud, and nothing needs an account.

## Binding

| Step | |
| :---: | --- |
| 1 | On the Printer page, press Scan. The vent looks for Bambu machines on the local network. |
| 2 | Pick yours from the list, or enter its IP by hand. |
| 3 | Enter its **LAN access code**. |
| 4 | The serial number is filled in by the scan, or type it. |

The access code is on the printer: **Settings, then Network, then LAN Only
Mode**. It is a short code the printer shows you, and it changes if you turn
LAN Only Mode off and on again.

## LAN Only Mode is not optional

Reading the printer's status works either way. **Sending it a command does
not.**

With LAN Only Mode off, the printer accepts the connection, accepts the
message, and then answers every command with:

    mqtt message verify failed

That is the printer refusing, not the vent failing. It was verified here
against five different message shapes from a plain MQTT client with no vent
involved at all. The printer wants cloud signed commands unless it is in LAN
Only Mode.

**Turn LAN Only Mode on if you want the fan, speed and light controls to work.**

## What it reads

Pushed by the printer, no polling.

| | |
| --- | --- |
| State | idle, printing, paused, finished, error |
| Progress | the printer's own completion percentage |
| Layer | current and total |
| Time | minutes remaining |
| Job | the file name |
| Nozzle | temperature |
| Bed | temperature |
| Chamber | temperature |
| Fans | part cooling, auxiliary, chamber |
| Speed | the current speed level |
| Light | whether the chamber light is on |
| AMS | the loaded tray and its filament type |
| Faults | HMS codes and print errors |
| Signal | the printer's Wi-Fi strength |

## What it can change

Requires LAN Only Mode.

| Control | Range |
| --- | --- |
| Part cooling fan | 0 to 100% |
| Auxiliary fan | 0 to 100% |
| Chamber fan | 0 to 100% |
| Print speed | Silent, Standard, Sport, Ludicrous |
| Chamber light | on, off |

The vent does not store any of these. It forwards the command and then shows
you what the printer reports it actually did, which is not always what was
asked. A slider holds still for a moment after you let go so that the incoming
reports do not fight your thumb.

If a command is refused, the page says so, with the printer's own reason.

## Connection

| | |
| --- | --- |
| Transport | MQTT over TLS, port 8883 |
| User | `bblp` |
| Password | the LAN access code |
| Certificate | the printer's self signed certificate is accepted, which is the only thing it offers |

A dropped connection reconnects on its own. The Printer page shows the current
state, including while it is retrying.

## What the printer drives on the vent

| Reads | Drives |
| --- | --- |
| Printer state | Advanced lighting mode, and the Auto vent rule |
| Completion percentage | the Progress Bar effect |
| Chamber temperature | Temperature Warning lighting |
| Bed temperature | the residual heat hold |
| AMS filament type | material aware venting |
| Faults | the fault colour |

## Nothing is stored that does not need to be

The access code is kept because the connection needs it on every reconnect. It
lives in the device's NVS partition in plain text, which is also where the
Wi-Fi password lives. That is the same place and the same exposure as the stock
firmware. **A full flash backup contains both**, so a backup image is a secret,
and `firmware/SAFETY.md` says so in more detail.

## Related

- [`venting.md`](venting.md) for what the vent does with the filament type
- [`lighting.md`](lighting.md) for the effects the printer state selects
- [`../firmware/PROTOCOL.md`](../firmware/PROTOCOL.md) for the exact message shapes
