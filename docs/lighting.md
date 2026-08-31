# Lighting

Two WS2812 runs, driven independently. Everything on this page is per effect
unless it says otherwise, and every setting that did not exist in the factory
firmware has a value that reproduces the factory behaviour exactly.

## Modes

The mode picker chooses which set of settings is live. It is stock, unchanged.

| Mode | What it is |
| --- | --- |
| **Simple** | One effect for the whole device. The one most people use. |
| **Advanced** | One effect per printer state, so the vent tells you what the printer is doing without you reading anything. |
| **Temperature Warning** | Two states only, safe and hot, driven by the printer's chamber temperature. |

### Advanced, per state

| State | When |
| --- | --- |
| Idle | connected, nothing running |
| Printing | a job is running |
| Paused | a job is paused |
| Finished | a job completed |
| Error | the printer reported a fault |
| Offline | no printer connection |

## Effects

Ids 0 to 6 are the factory's and render bit identical to the factory renderer.
Ids 7 to 17 are added.

| Id | Effect | What it does |
| :---: | --- | --- |
| 0 | Static | one colour, no movement |
| 1 | Breathing | fades the whole strip up and down |
| 2 | Strobing | hard on, hard off |
| 3 | Wave | a travelling brightness wave |
| 4 | Marquee | a lit block that walks along the strip |
| 5 | Color Cycle | the whole strip walks the hue wheel together |
| 6 | Rainbow | the hue wheel spread along the strip |
| 7 | Cylon | one block sweeps out and back |
| 8 | Bounce | one pixel bounces end to end |
| 9 | Progress Bar | fills to the printer's own completion percentage |
| 10 | Marquee Out | two blocks walk outward from the centre |
| 11 | Marquee In | two blocks walk inward to the centre |
| 12 | Fill Out | fills from the centre to both ends |
| 13 | Fill In | fills from both ends to the centre |
| 14 | Bounce Out | two pixels bounce outward |
| 15 | Bounce In | two pixels bounce inward |
| 16 | Bounce Fill Out | bounce that leaves the strip filled behind it, outward |
| 17 | Bounce Fill In | the same, inward |

Progress Bar reads `mc_percent` off the printer's own report, not a timer, so
it is the printer's idea of progress and not an estimate.

The pairs are kept as separate effects on purpose. The reverse switch is
global, so folding each pair into one effect plus reverse would have coupled
them to every other effect.

## The four colours

Every effect carries four colours, not one.

| Slot | Painted where | Painted when |
| --- | --- | --- |
| **Active, open** | the lit pixels | the vent is open |
| **Active, closed** | the lit pixels | the vent is closed |
| **Inactive, open** | the unlit pixels | the vent is open |
| **Inactive, closed** | the unlit pixels | the vent is closed |

The point of the closed pair is that the strip reports where the flap is
without the effect having to change.

The inactive colours are optional. Unset, the unlit pixels stay black, which is
what every effect did before they existed, and the renderer is bit identical to
the old one in that case. The swatch shows a clear button only while it holds a
colour; clearing it sends a null and puts the slot back to unset.

The sync buttons copy one colour onto another so you do not have to pick the
same colour twice.

## Brightness and speed

| Setting | Range | |
| --- | :---: | --- |
| Brightness | 0 to 100 | flat, or the START of a ramp |
| Brightness end | 0 to 100 | optional; set it and brightness ramps from start to end over each cycle, then restarts |
| Speed | 0 to 100 | how fast the effect advances |

Brightness end is optional in the same way an inactive colour is. Unset, the
effect runs at one brightness exactly as it always did.

## Direction

There are three separate flips, and they are combined by exclusive or, so each
one answers "should this be turned around" and none of them silently wins.

| Flip | Scope | Where |
| --- | --- | --- |
| Master reverse | the whole device | Lighting card, stock setting |
| Per strip | one run | LED Count card |
| Per effect | one effect, and in Advanced mode one effect in one state | the effect's own row |

Per strip exists because a run can be physically mounted the other way round,
and forcing the whole device to match it is not a fix.

## Strip length

Stock drives a fixed 16 pixels per run. The hardware is not always 16.

BIQU's own manual says one strip group is 16 LEDs and two groups are 27 in
total, so with two connected at least one run is shorter than the 16 being
driven. Every effect with a centre or a scale then lands wrong on the short
run: a centre computed for 16 shows up around 68% of the way along an 11 LED
run, and a progress bar scaled to 16 reads full at 78%.

Set each run's real length in the LED Count card. 16 and 16 reproduces stock
exactly. The strips cannot be interrogated, because WS2812 is write only, so
this is something you measure and enter.

## Contiguous

Off, each run renders the effect from its own start, so a marquee runs twice
side by side. On, the two runs are treated as one strip of
`length 0 + length 1` pixels and each output gets its own slice, so the light
travels the whole length once.

## Temperature Warning mode

| Setting | Default | |
| --- | :---: | --- |
| Hot threshold | 50 °C | the temperature the mode calls hot |
| Gradient minimum | 25 °C | the cool end of the colour gradient |
| Gradient maximum | 60 °C | the hot end |

Stock burns 50 °C into the comparison. Fifty is right for a machine printing
PLA and wrong for one printing ABS, where the bed sits near a hundred and the
light would read hot for the whole job. Leave it at zero in the field and the
stock number is used.

## Fault colour

Stock shows solid red at brightness 127, with no brightness scaling and no
alternative.

| Setting | |
| --- | --- |
| Fault colour | any colour |
| Fault brightness | 0 to 100 |
| Fault strobe | solid, or strobing |

Red is the one colour a red and green colourblind owner cannot pick out, and a
fault that does not move is a fault that gets walked past. Leave the override
off and the behaviour is stock, byte for byte.

## Barber Pole band width

One spare number per effect, whose meaning belongs to the effect that reads it.
Barber Pole reads it as the band width in pixels. An effect that does not read
it ignores it. Unset, an effect that does read it uses its own default, so
"never set" and "set to zero" stay different answers.

## Custom animation

Upload a frame sequence and the device plays it from RAM.

RAM, deliberately. Flash is the obvious home for an uploaded animation and it
is the wrong one here: the flash budget is fixed by the stock partition layout,
and spending it on an animation is spending the ability to go back to the
factory firmware. An uploaded animation is lost on reboot. That is the trade.

## Follow switches

| Switch | |
| --- | --- |
| Follow printer | the lighting reacts to printer state, which is what makes Advanced mode do anything |
| Follow vent | the colour pair follows the flap position |

Both are stock.

## Related

- [`venting.md`](venting.md) for what "vent open" means to the colour pairs
- [`printer.md`](printer.md) for where the progress percentage comes from
