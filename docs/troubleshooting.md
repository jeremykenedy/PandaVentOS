# Troubleshooting

## The page

**The page loads but every value is blank or dashes.**
The page is up, the WebSocket is not. Reload. If it stays blank, the device
answers plain HTTP but not the socket, which usually means it is busy: a flash
read or a scan. Wait thirty seconds.

**A value shows a dash instead of a number.**
That is deliberate. The device has not reported it. A missing value is said to
be missing rather than drawn as zero, because a zero that means "we do not
know" is worse than no number at all.

**Settings could not be saved to the device.**
The NVS partition rejected the write. Settings are live but will not survive a
reboot. The banner withdraws itself the moment a save succeeds. If it keeps
coming back, the configuration has outgrown its budget, which is a firmware
bug, not something you can fix from the page.

**The page is in a language I did not pick.**
It follows the browser on first load only, and after that the stored setting.
Settings page, Language.

## The vent

**The dial spins and never stops.**
The flap is being driven and is not reaching its end band. Something is
physically in the way, or the endstop needs recalibrating. There is no
time-based timeout, but it is not driven forever either: after four checks,
about eight hundred milliseconds, the group is given up on and latches a
fault, which is what turns the strip red. Set the mode to the position it
is already in to stop it.

**Auto does nothing.**
Auto follows the printer. With no printer bound, or the printer offline, there
is no state to follow. Check the Printer page.

**Material aware venting does nothing.**
| Check | |
| --- | --- |
| Master switch | confirm it is on (it ships on) |
| Printer | it needs the AMS report, so it needs a bound, connected printer |
| Material | the loaded filament needs a rule, and the rule needs to be on |
| State | rules apply during a print, not between prints |

**The flap moves the wrong way.**
Recalibrate the endstop. The hall bands are read from your unit, and if they
were learned against a jam they will be wrong.

## The lights

**An effect looks off centre, or a progress bar fills early.**
The strip length is wrong. Stock drives a fixed 16 and the hardware is not
always 16. Count the LEDs on each run and set them in the LED Count card.

**One run runs backwards.**
Turn that run around on its own in the LED Count card, rather than flipping the
master reverse, which turns both.

**The colours change when the vent moves.**
That is the feature. Each effect has a vent open colour and a vent closed
colour. Use the sync button to make them the same if you do not want it.

**Picking an effect wiped my colour.**
It should not, and it does not any more. If you see it, the page and the device
disagree about how many effects exist, which means one of them is old. Reload.

**The uploaded animation is gone after a reboot.**
It lives in RAM. That is the trade: flash is fixed by the stock partition
layout and spending it on an animation spends the ability to go back to the
factory firmware.

## The printer

**Commands are refused: "mqtt message verify failed".**
The printer is not in **Developer Mode**. LAN Only Mode on its own does not
help: measured on real hardware, everything under the printer's `print`
namespace comes back refused in LAN Only Mode exactly as it does from the
cloud, and only `system.ledctrl` -- the lights -- gets through. Developer Mode
is a separate switch that only appears underneath LAN Only Mode once LAN Only
Mode is on and the printer has restarted. See
[Developer Mode is not LAN Only Mode](printer.md#developer-mode-is-not-lan-only-mode).

Reading is never affected: every value the vent shows arrives whatever mode
the printer is in.

If you do toggle LAN Only Mode, the access code changes, so re-enter it.

**Scan finds nothing.**
The vent and the printer have to be on the same subnet. A guest network, a
mesh node with client isolation, or two VLANs will all do this.

**It connects, then drops, repeatedly.**
Usually a wrong access code. The printer accepts the TCP connection and rejects
the credentials, which looks like a flapping link rather than an auth failure.

## Updating

**After an update I cannot reach `<hostname>.local`.**
mDNS takes about a minute to re-register. Use the IP.

**The upload finishes and nothing happens.**
It reboots itself when the image lands, which takes about eight seconds. If it
never comes back, the image did not verify and the device is running the old
slot, which is the point of having two.

**Did my settings survive?**
Yes. Every older configuration layout is read and lifted forward field by
field. If a setting is back at its default after an update, that is a bug worth
reporting.

## Recovery

**It does not boot at all.**
Cable, and write back the full 4 MB image from
[`backup.md`](backup.md). This is the failure that image exists for, and it is
why the first instruction in this project is to take one.

**I never took a backup.**
BIQU publishes the application image, so the stock app can be put back. The
bootloader is not published. If the bootloader is intact, flash the stock app
at 0x10000 over a cable. If it is not, there is no public image to recover
with.

## Related

- [`backup.md`](backup.md)
- [`../firmware/SAFETY.md`](../firmware/SAFETY.md)
