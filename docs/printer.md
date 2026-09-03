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

## What it reads

Pushed by the printer, no polling, and never affected by which mode the printer
is in.

| | |
| --- | --- |
| State | idle, printing, paused, finished, error |
| Progress | the printer's own completion percentage |
| Layer | current and total |
| Time | minutes remaining |
| Job | the file name |
| Nozzle | temperature |
| Bed | temperature |
| Chamber | temperature. On a P2S this arrives as `device.ctc.info.temp`, not `chamber_temper`, and reading only the older field is why this showed a dash for a while |
| Door | open or shut |
| Nozzle fitted | diameter and material, decoded from the four character code the printer sends |
| Filament | whether there is any in the extruder, from the runout sensor |
| Fans | part cooling, auxiliary, chamber |
| Speed | the current level, and the percentage it works out to |
| Lights | chamber and toolhead |
| AMS | humidity as a percentage and as a level, temperature, and every spool with its type, its actual colour and how much is left |
| Faults | HMS codes, spelled the way Bambu's own wiki spells them so a code can be pasted into a search |
| Firmware | whether an update is waiting on the printer |
| Signal | the printer's Wi-Fi strength |

## What it can change, and what it cannot

**Two lights. That is the whole list.**

| Control | |
| --- | --- |
| Chamber light | on, off |
| Toolhead light | on, off, when the printer has one |

Fans, print speed and temperatures are not here. They were, they were correct,
and the printer refused every one of them.

### The measurement

One connection, one command at a time, against a real machine:

| Command | Answer |
| --- | --- |
| `system.ledctrl` chamber light | `result: success` |
| `system.ledctrl` toolhead light | `result: success` |
| `system.ledctrl` `chamber_light2` | `fail`, "did not find the valid led" |
| `print.gcode_line` `M106 P1` part fan | `mqtt message verify failed` |
| `print.gcode_line` `M106 P2` aux fan | `mqtt message verify failed` |
| `print.gcode_line` `M106 P3` chamber fan | `mqtt message verify failed` |
| `print.gcode_line` `M104` nozzle temp | `mqtt message verify failed` |
| `print.print_speed` | `mqtt message verify failed` |
| `print.print_option` | `mqtt message verify failed` |
| `print.set_airduct` | `mqtt message verify failed` |
| `print.ams_get_rfid` | `mqtt message verify failed` |

The line is exact. **`system.ledctrl` is not signature checked. Everything
under `print` is.**

### Developer Mode is not LAN Only Mode

The printer tells you which mode it is in, in the `fun` field of its own status
report. Bit `0x20000000` set means unsigned commands are rejected.

| Mode | Commands |
| --- | --- |
| Cloud | rejected |
| LAN Only Mode | **still rejected** |
| Developer Mode | accepted |

Developer Mode is a separate switch that only appears in the printer's network
settings **after** LAN Only Mode is on and the printer has been restarted.
Turning on LAN Only Mode by itself changes nothing about what the printer will
accept.

**Reading is never gated.** Every value in the table above arrives whatever
mode the printer is in. Only writing is, and only the lights get through.

### If your printer is in Developer Mode

The firmware still speaks all of it: the three fans, the speed levels,
arbitrary G-code. Only the web page stopped drawing controls for them, because
a control that cannot work teaches you that the page lies.

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
