# BIQU Panda Vent v1.0.0 wire protocol

Everything here was recovered from the shipping product, not guessed:

* the factory web app (`factory/www/index.html`, extracted byte-exact from
  the stock image and served byte-exact by this firmware),
* a live state capture from a running stock unit,
* the stock image's own data tables.

Where the printed manual disagrees with the shipping firmware, the firmware
wins and the disagreement is called out below.

## Transport

One WebSocket, `ws://<host>/ws`. No REST, no polling, no second socket.

* On connect the device pushes the **complete** state document.
* The UI sends partial documents; every message is a single-key object whose
  key names the section: `wifi`, `sta`, `ap`, `printer`, `settings`,
  `rgb_switch`, `rgb_mode`.
* The device replies either with an updated (usually complete) state
  document, or with a `response` object for operations that raise a dialog.

The only non-WebSocket call in the whole app is the firmware upload:
`POST /` -> `/ota` with header `OTA-Type: ota_fw`, body = raw image.

The UI's send helper is:

```js
function ws_send_data(root, members) {
    let j_obj = {};
    j_obj[root] = {};
    for (const key in members) j_obj[root][key] = members[key];
    ws_send_json(JSON.stringify(j_obj));
}
```

44 call sites use it. All 44 are enumerated below.

## Outbound: the state document

Key order matters only for byte-comparison of captures; the UI reads by
name. This firmware reproduces the factory order anyway.

```
wifi:     ssid str, password str, scan int
sta:      hostname str, ip str, state int, auth_err_reason int
ap:       ssid str, password str, ip str, on int
printer:  name str, sn str, access_code str, ip str, state int, scan int
rgb_mode: rgb_light_mode int
          light_on_off bool, warning_sw bool, is_follow_printer bool,
          is_follow_vent bool, is_reverse bool
          current_simple_effect int
          effects[7]: { id, brightness, speed, color }
          h2d_mode.device_states[6]:
              { device_state_id, active_effect_id,
                effects[7]: { effect_id, brightness, speed, color } }
          warning_hot_mode.safe / .warn:
              { current_effect, params[2]: { index, bg, speed } }
settings: fw_version str, language str
```

Note the asymmetry, which is factory behaviour and must not be tidied up:
the state document spells brightness `brightness` in `effects` but `bg` in
`warning_hot_mode.params`, and inbound messages always spell it `bg`.

### Enumerations

| `sta.state` | meaning |
|---|---|
| 1 | no ssid saved (UI routes to first-use flow) |
| 2 | connecting |
| 3 | connected |
| 4 | reconnecting |
| 5 | password error |

| `printer.state` | meaning |
|---|---|
| 0, 1 | unbound |
| 2 | connecting |
| 3 | connected |
| 4 | IP error |
| 5 | SN error |
| 6 | access code error |
| 7 | unknown error |

| `wifi.scan` / `printer.scan` | meaning |
|---|---|
| 0 | idle |
| 1 | scanning |
| 2 | done |
| 3 | printer only: IP-change scanning |
| 4 | printer only: SN not matched |
| 5 | printer only: IP not changed |
| 6 | printer only: new IP applied |

Effects, in UI order: `0 Static, 1 Breathing, 2 Strobing, 3 Wave,
4 Marquee, 5 Color_Cycle, 6 Rainbow`.

Light modes: `0 Simple, 1 Advance (H2D), 2 Warning Hot`.

H2D device states, in button order: `0 Idle, 1 Preparation, 2 Printing,
3 Paused, 4 Completed, 5 Error`.

Brightness and speed are 0..100 in steps of 5. Colours are uppercase
`"RRGGBB"` with no `#`.

### Scan results

Scan replies are partial documents, not part of the full state push:

```json
{"wifi":    {"scan": 2, "list": [{"ssid": "...", "rssi": -55}]}}
{"printer": {"scan": 2, "list": [{"name":"...","sn":"...","ip":"...","access_code":"..."}]}}
```

## Inbound: every message the app can send

### `wifi` (2 sites)
```json
{"wifi": {"scan": 1}}
{"wifi": {"ssid": "...", "password": "..."}}
```

### `sta` (1 site)
```json
{"sta": {"hostname": "..."}}
```

### `ap` (4 sites)
```json
{"ap": {"on": 0}}
{"ap": {"on": 1}}
{"ap": {"ssid": "...", "password": "..."}}
{"ap": {"ssid": "...", "password": "...", "ip": "..."}}
```
The two-field form is the hotspot name/password form and answers
`set_ap`; the three-field form is the hotspot IP form and answers
`set_hotspot_ip`, after which the UI restarts the device itself.

### `printer` (3 sites)
```json
{"printer": {"scan": 1}}
{"printer": {"disconnect": 1}}
{"printer": {"name": "...", "sn": "...", "access_code": "...", "ip": "..."}}
```

### `settings` (3 sites)
```json
{"settings": {"language": "en"}}
{"settings": {"reset": 1}}
{"settings": {"factory_reset": 1}}
```
`reset` is a plain reboot. `factory_reset` erases NVS and reboots.

### `rgb_switch` (11 sites)
Each is a single key with `0` or `1`, except the last:
```json
{"rgb_switch": {"total_switch": 0|1}}
{"rgb_switch": {"follow_printer": 0|1}}
{"rgb_switch": {"warning_overide": 0|1}}
{"rgb_switch": {"reverse_light": 0|1}}
{"rgb_switch": {"follow_vent": 0|1}}
{"rgb_switch": {"current_light_mode": 0|1|2}}
```
`warning_overide` is spelled that way in the product. It is not a typo to
fix; the app sends exactly that string.

### `rgb_mode` (18 sites)
```json
{"rgb_mode": {}}                                  // query, answer with full state
{"rgb_mode": {"reset": <light_mode>}}             // reset that mode to defaults

{"rgb_mode": {"simple_mode": {"effect": <fx>}}}
{"rgb_mode": {"simple_mode": {"effect": <fx>, "rgb":   "RRGGBB"}}}
{"rgb_mode": {"simple_mode": {"effect": <fx>, "bg":    <0..100>}}}
{"rgb_mode": {"simple_mode": {"effect": <fx>, "speed": <0..100>}}}

{"rgb_mode": {"h2d_mode": {"mode": <state>, "effect": <fx>,
                           "bg": <n>, "speed": <n>, "rgb": "RRGGBB"}}}
{"rgb_mode": {"h2d_mode": {"mode": <state>, "effect": <fx>, "rgb":   "RRGGBB"}}}
{"rgb_mode": {"h2d_mode": {"mode": <state>, "effect": <fx>, "bg":    <n>}}}
{"rgb_mode": {"h2d_mode": {"mode": <state>, "effect": <fx>, "speed": <n>}}}

{"rgb_mode": {"warning_hot_mode": {"safe": {"effect": 0|1, "bg":    <n>}}}}
{"rgb_mode": {"warning_hot_mode": {"safe": {"effect": 0|1, "speed": <n>}}}}
{"rgb_mode": {"warning_hot_mode": {"warn": {"effect": 0|1, "bg":    <n>}}}}
{"rgb_mode": {"warning_hot_mode": {"warn": {"effect": 0|1, "speed": <n>}}}}
```

Names to get right, because they differ from the outbound document:
`bg` is brightness, `rgb` is the colour, `mode` is the H2D **device state**
id and `effect` is the effect id. `warning_hot_mode.effect` is an index into
`['Static Mode', 'Strobing Mode']`, so 0 or 1, not an effect id.

The colour arrives as an uppercase `"RRGGBB"` string. The app also carries a
`hexToRgb()` helper that can yield `{r,g,b}`, so a parser should accept both.

## `response` messages

```json
{"response": {"type": "<name>", "ok": 0|1}}
```

Types the app acts on:

| type | UI behaviour |
|---|---|
| `set_hostname` | dialog, OK sends `settings.reset` (reboot) |
| `set_ap` | dialog |
| `set_hotspot_ip` | dialog, OK sends `settings.reset` (reboot) |
| `factory_reset` | dialog |
| `ota_fw` | dialog, OK reloads the page |
| `ota_img`, `ota_gif`, `ota_get_img` | Panda Status family only, no vent path |
| `ota_unknown` | "unknown upload type" dialog |

The app also handles `settings.img_version`, `settings.printing_ui_type`,
`settings.list2`, `settings.list3`, `ws_theme.*` and similar. Those belong to
the Panda Status products that share this web app. A vent never sends them,
and a stock vent capture confirms it does not.

## Firmware defaults, as shipped

Taken from the default tables inside the stock image and confirmed against a
live unit.

| Setting | Default |
|---|---|
| Hotspot ssid | `Panda_Vent_` + the six STA MAC bytes, uppercase hex |
| Hotspot password | `987654321` |
| Hotspot IP | `192.168.254.1` |
| Hotspot enabled | yes |
| Hostname | `PandaVent` |
| Light mode | Simple |
| Simple colour, all effects | `FF3700` |
| Brightness / speed, everywhere | 50 |

H2D per-state colours, from the six RGB triples at file offset `0x1707c` of
`panda_vent_v1.0.0.bin`:

| State | Colour |
|---|---|
| Idle | `FFFFFF` |
| Preparation | `FF8000` |
| Printing | `FFFFFF`, ships on the Rainbow effect |
| Paused | `FFFFFF` |
| Completed | `00FF00` |
| Error | `FF0000` |

The printed manual lists Preparation as `F8A323` and Completed as `00FF2A`.
Neither value appears anywhere in the shipping image, and a live unit uses
`FF8000` and `00FF00`. The manual is wrong.

The hotspot ssid derivation is `ESP_MAC_WIFI_STA`, not `ESP_MAC_WIFI_SOFTAP`:
a unit whose STA MAC is `AA:BB:CC:DD:EE:10` advertises
`Panda_Vent_AABBCCDDEE10`, and its SoftAP MAC would end `11`.

## Quirks worth knowing

* **Effects 5 (Color_Cycle) and 6 (Rainbow) generate their own colours.**
  The firmware resets any colour written for them, so a settings restore
  that replays colours for those effects will always read back `FFFFFF`.
  The factory UI guards this with `if (currentEffect < 5)`. Any tool that
  replays settings must skip `rgb` for effect ids >= 5 or it will report
  false mismatches.
* Sending `{"rgb_mode": {}}` is the app's way of asking for a refresh.
* `auth_err_reason` is always present in `sta` and is 0 in normal operation.
* mDNS goes stale across a device reboot. Anything that waits for the device
  to come back should resolve the hostname to an IP once and then use the
  IP.
