# Network

## First boot

With no Wi-Fi configured, the vent raises its own access point.

| | |
| --- | --- |
| Network name | `Panda_Vent_` + the twelve hex digits of the MAC |
| Password | `987654321` |
| Address | `http://192.168.254.1` |

It is a WPA2 network, not an open one: the default password is nine
characters, and anything eight or longer makes the access point secured. All
three values are the compiled defaults and all three can be changed on the
Hotspot card once you are in.

Join it, open that address, and the setup page comes up. Pick a language,
choose your network, enter its password.

## Station settings

| Setting | Detail |
| --- | --- |
| **SSID** | pick from a scan, or type it |
| **Password** | WPA2 |
| **Hostname** | what it answers to over mDNS |

A scan takes a few seconds and the page stays usable while it runs.

## Reaching it afterwards

| | |
| --- | --- |
| By name | `http://<hostname>.local` |
| By address | `http://<its IP>` |

mDNS takes about a minute to register after a reboot, so straight after an
update, use the IP.

## The setup hotspot after setup

| Setting | |
| --- | --- |
| **AP enabled** | keep the hotspot up alongside the Wi-Fi connection, or drop it |
| **AP name** | rename it |
| **AP password** | give it one |

Keeping it up is a way back in if the Wi-Fi network changes. Dropping it is one
less radio and one less open network in your house.

## Device name

Separate from the hostname. The hostname is what the network calls it; the
device name is what the page calls it, in the corner and in the title bar.
Stock hard codes "Panda Vent" as a translation string. Here it is a setting.

Useful when you have two.

## What it talks to

| | |
| --- | --- |
| Your printer | MQTT over TLS on the local network, port 8883 |
| Your browser | HTTP and one WebSocket, port 80 |
| Anything else | nothing |

There is no cloud service, no telemetry, no update check and no outbound
connection of any kind other than to the printer you bound it to. It works with
no route to the internet at all.

## Ports

| Port | |
| --- | --- |
| 80 | the web page, the WebSocket at `/ws`, `/ota`, `/backup`, `/anim` |
| 8883 | outbound only, to the printer |
| 5353 | mDNS |

## Related

- [`backup.md`](backup.md) for `GET /backup`
- [`printer.md`](printer.md) for LAN Only Mode
