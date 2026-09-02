# Venting

One flap, driven by a small motor against a hall sensor. There is no position
encoder: the sensor's end bands are the limit switches, which is how the stock
firmware does it and how this does it.

## Modes

| Mode | What the flap does |
| --- | --- |
| **Open** | stays open, whatever the printer is doing |
| **Closed** | stays closed, whatever the printer is doing |
| **Auto** | the firmware decides |

Auto is the factory rule, one line: **open while the printer is printing or
paused, closed otherwise.** That is the whole of stock's logic. It never looks
at what you are printing.

Tap the dial or the pill on the Status page to cycle Auto, Open, Closed and
back. The three named cards are still there, because a cycle is quick and a
named choice is clear, and those are different needs.

## The dial has three states

| State | What you see |
| --- | --- |
| Open | the arc points up, in the accent colour |
| Closed | the arc points down, dimmed |
| Travelling | the arc spins |

Travel takes a few seconds, and those seconds are exactly when somebody is
staring at the dial wondering whether their tap registered. The device reports
that it is moving the moment the motor starts, not only when it arrives.

If your system is set to reduce motion, the arc pulses in place instead of
spinning. It still tells you.

## Material aware venting

An addition. **On by default**; turn the master switch off and the behaviour
is stock, exactly.

Turned on, it sits on top of Auto and reads the filament the printer says is
loaded.

| Situation | What happens |
| --- | --- |
| Printing a **venting** material | the flap opens |
| Printing a **sealing** material | the flap closes |
| Printing something with no rule | stock's answer, unchanged |
| Not printing, bed still hot | held open until the heat is gone |
| Not printing, bed cool | stock's answer, unchanged |

### The nine rules

All nine ship on, and each can be switched off on its own. Switching one off
means that material gets no rule and falls back to stock.

| Material | Default | Why |
| --- | :---: | --- |
| PLA | vent | no meaningful fumes, but heat build up warps prints |
| PETG | vent | same |
| PET | vent | same |
| TPU | vent | same |
| ABS | **seal** | styrene, and it needs the chamber heat to not warp |
| ASA | **seal** | same |
| PC | **seal** | needs the chamber heat |
| PA | **seal** | nylon, hygroscopic and warp prone |
| HIPS | **seal** | usually printed alongside ABS |

The material comes from the printer's own AMS report, so it is whatever the
printer thinks is loaded. An unspooled or unmatched filament is left alone
rather than guessed at.

**Not covered:** PPA-CF, PPA-GF, PPS and PPS-CF get no rule and fall back to
stock, which opens the flap during a print. Those are the hottest materials
Bambu supports and sealing them would probably be right. They are left alone
because guessing on a material nobody has tested here is worse than doing
nothing.

### Residual heat hold

After a print, an enclosure full of hot air is still an enclosure full of hot
air. The flap is held open until the bed reads cool.

| Setting | Default | |
| --- | :---: | --- |
| Open above | 45 °C | hold the flap open while the bed is above this |
| Close below | 35 °C | let it close once the bed is below this |

Two numbers rather than one on purpose. A single threshold makes the flap
chatter open and shut every time the bed drifts across it.

## Vent button ring light

The ring around the physical button.

| Mode | |
| --- | --- |
| **Factory** | stock behaviour: off in Auto, blinking in Manual |
| **Always on** | on, always |
| **Always off** | off, always |
| **On while open** | a light that means the flap is open |
| **Off while closed** | the same idea, stated the other way |

The manual mode blink is a separate switch, so you can keep the factory
behaviour without the blink.

## Endstop recalibration

Runs the flap to both ends and reads the hall value at each, which is how the
firmware knows what open and closed look like on your particular unit.

Takes about ten seconds. The page shows each step as it happens and reports the
two millivolt readings at the end, plus whether each end was reached.

Stock can only do this from the button. This can do it from the page.

## What the physical button does

Stock behaviour, unchanged.

| Press | |
| --- | --- |
| Short press | toggles Auto and Manual |
| Short press, in Manual | toggles the flap |
| Long press, 3 seconds | factory reset |

## No travel timeout

A jammed flap that never reaches its band will drive its motor until the target
changes. That is stock's behaviour, and this is a clone, so it is ours. A stall
guard was carried here for a while and was removed: a deliberate departure is
still a departure, and the place to make one is not silently inside the motor
path.

## Related

- [`lighting.md`](lighting.md) for the colours that follow the flap position
- [`printer.md`](printer.md) for where the filament and bed temperature come from
