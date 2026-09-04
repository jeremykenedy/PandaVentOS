# Safety rules for flashing this device

These exist because ignoring them cost a working vent and a day of the
owner's time. They are not suggestions.


## Rule 0a: take a FULL FLASH image before any flash, and test the restore

Not the app. The whole 4 MB, offset 0 to 0x400000: bootloader, partition
table, otadata, both app slots and NVS.

    python -m esptool --chip esp32 --port <YOUR-PORT> -b 115200 \
      read-flash 0 0x400000 golden-$(date +%Y%m%d-%H%M%S)-full-4MB.bin

The port is whatever the CH34x bridge enumerates as: `/dev/cu.wchusbserial*`
on macOS, `/dev/ttyUSB0` on Linux, a `COM` port on Windows.

Restore the same image with `write-flash 0`:

    python -m esptool --chip esp32 --port <YOUR-PORT> -b 115200 \
      write-flash 0 golden-...-full-4MB.bin

This firmware also serves the whole flash over the network at `GET /backup`,
which takes about seventeen seconds and needs no cable:

    curl -o golden-$(date +%Y%m%d-%H%M%S)-full-4MB.bin http://<device>/backup

Restoring still needs the cable. Only the backup side is wireless.

This rule exists because it was not followed. On 2026-08-30 the first flash of
this project wrote the bootloader and partition table with only the app backed
up. BIQU publishes only the app. The factory firmware on that unit can no
longer be booted and there is nothing to restore it from. The cost of the
missing step was thirty seconds.

A backup you have not restored from is not a backup. Test it.


## 0. THE FIRST INSTALL FROM STOCK CANNOT SELF-REVERT

Read this before anything else, because it invalidates the obvious
assumption.

Rollback protection is applied by the firmware that performs the INSTALL,
not by the image being installed. When an ESP-IDF app writes a new image it
marks the new slot either `PENDING_VERIFY` (probation) or `VALID`
(confirmed), and it makes that choice from ITS OWN sdkconfig.

Stock BIQU firmware is built WITHOUT `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.
So when stock installs anything, it marks the new slot valid immediately.
The image that lands has no probation and no way back, no matter what its
own sdkconfig says.

Consequences, stated plainly:

* Going stock -> custom, the self-revert in `health_task` can never fire.
  If that build fails to reach the network, the device is bricked until a
  cable is attached.
* Going custom -> anything, probation works, because THIS firmware sets the
  new slot to `PENDING_VERIFY`.

Therefore: **the first flash of a new custom build onto a stock device is a
cable-required operation.** Do not start it unless the USB cable is already
connected and a tested serial restore path has been confirmed to
run. Subsequent custom -> custom updates are wireless and protected.

## 1. Rollback protection is mandatory in this firmware
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set in `sdkconfig.defaults`.
Every image THIS firmware installs boots on probation. `health_task` in
`app_main.c` confirms the image ONLY after the device holds a station IP
*and* the HTTP server is answering. If that has not happened within 120
seconds, the firmware calls `esp_ota_mark_app_invalid_rollback_and_reboot()`
and the bootloader restores the previous image with no cable and no network.

Never remove this. Never raise the probation window past ~3 minutes. See
rule 0 for the case it cannot cover.

## 2. Network comes up before hardware
`app_main()` starts Wi-Fi and the HTTP/OTA server FIRST, then LEDs, motors
and the printer link. A peripheral fault therefore degrades the vent; it
cannot take it off the air. A device that stays reachable can always be
re-flashed wirelessly.

## 3. No aborts in the boot path
The only `ESP_ERROR_CHECK` call left is `esp_netif_init()` (unsurvivable).
NVS init failure is logged, not asserted. Every peripheral failure is logged
and stepped over. An abort loop serves nothing, prints nothing, and hides
its own cause.

Related: `pv_motor.c`'s button task requires each button to be observed
RELEASED before any press is counted. GPIO12 is the MTDI strapping pin and
reads low at boot; without the arming rule it fires a phantom long-press on
every boot, which writes NVS on every boot, which is one of the ways this
device filled its NVS and crash-looped.

## 4. Always flash over stock
The previous slot is the rollback target, so it must contain the factory
firmware. Flash custom -> stock -> custom, never custom -> custom, unless
the currently running image is already confirmed good.

## 5. Prove the rollback before trusting a build
Before any build is treated as safe, flash a deliberately broken variant
(e.g. wrong Wi-Fi credentials) and confirm the device comes back on stock by
itself. An untested safety net is not a safety net.

That test is only meaningful when it starts from a custom image already
running, because of rule 0. Running it from stock proves nothing and will
brick the device.

## 6. Never flash without being told to
No image goes onto the vent unless the owner has said so in that message,
with the USB cable connected. Building, verifying and committing are not
permission to deploy.

## Physical recovery (works regardless of firmware state)
The vent's USB port exposes a CH340 serial bridge (VID 0x1A86, PID 0x7522).
macOS does NOT drive 0x7522 with its built-in driver: `AppleUSBCHCOM.dext`
claims only 0x7523 and 0x5594, so the device enumerates but no `/dev/cu.*`
node appears. On macOS the vendor CH34x VCP driver
is required, after which the port appears as `/dev/cu.wchusbserial*`.

    python -m esptool --chip esp32 --port <YOUR-PORT> -b 115200 \
      write-flash 0x10000 <BIQU-STOCK-IMAGE.bin>

writes BIQU's stock application at 0x10000 and returns the device to factory.
That image is theirs and is not shipped here; `tools/restore.sh --factory-bin
FILE --factory` does the same thing with a size and hash check first, which is
the safer way in. If the app then reports NVS-full errors (`0x1105`,
`ESP_ERR_NVS_NOT_ENOUGH_SPACE`), the config area may still hold a previous
firmware's data:

    esptool ... erase-region 0x9000 0x3000    # NVS  (clears saved settings)
    esptool ... erase-region 0xc000 0x2000    # otadata (forces boot of ota_0)

Erasing NVS also clears the saved Wi-Fi, so the owner has to redo setup
through the hotspot. Say so before doing it.

## An OTA cannot convert a stock vent

`/ota` writes an application into a partition slot. It does not write the
bootloader and it does not write the partition table, so an application only
runs if the slot it landed in is at the address that application was linked
for. On a vent that has already had a cable install those agree by
construction. On a vent still running BIQU's firmware they are an assumption,
and nothing stock serves lets you check it: there is no endpoint that reports
a partition table.

Uploading this firmware to a stock vent over the network can therefore leave
it running neither one -- with no backup, because stock has no `/backup`
either. Convert with a cable once, per unit. Update over the network forever
after.

## Wireless restore (works whenever the device still serves HTTP)
    curl -X POST -H 'OTA-Type: ota_fw' \
      --data-binary @<BIQU-STOCK-IMAGE.bin> \
      http://<device>/ota

uploads an application image to `/ota` and nothing else. It does not check
what it is sending, does not wait for the device to come back, and does not
preserve any settings: stock will replace them with its own defaults on first
boot. Take a backup first if the settings matter.

The header is `OTA-Type`, with no `X-` prefix -- that is the name the handler
in `pv_http.c` reads. An `X-OTA-Type` header is simply not seen, and the
upload proceeds on the handler's default, which today happens to be the same
value. Relying on that is how a wrong header goes unnoticed until the default
changes.

Use an IP rather than a `.local` name: mDNS goes stale across the reboot.

`tools/restore.sh --factory <host>` wraps this with the confirmation, the
size check and the hash check, and is the recommended path.
