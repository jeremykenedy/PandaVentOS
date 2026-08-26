# Safety rules for flashing this device

These exist because ignoring them cost a working vent and a day of the
owner's time. They are not suggestions.

## 1. Rollback protection is mandatory
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set in `sdkconfig.defaults`.
Every image boots on probation. `health_task` in `app_main.c` confirms the
image ONLY after the device holds a station IP *and* the HTTP server is
answering. If that has not happened within 120 seconds, the firmware calls
`esp_ota_mark_app_invalid_rollback_and_reboot()` and the bootloader restores
the previous image with no cable and no network.

Never remove this. Never raise the probation window past ~3 minutes.

## 2. Network comes up before hardware
`app_main()` starts Wi-Fi and the HTTP/OTA server FIRST, then LEDs, motors
and the printer link. A peripheral fault therefore degrades the vent; it
cannot take it off the air. A device that stays reachable can always be
re-flashed wirelessly.

## 3. No aborts in the boot path
The only `ESP_ERROR_CHECK` calls left are `esp_netif_init()` (unsurvivable)
and NVS. Every peripheral failure is logged and stepped over. An abort loop
serves nothing, prints nothing, and hides its own cause.

## 4. Always flash over stock
The previous slot is the rollback target, so it must contain the factory
firmware. Flash custom -> stock -> custom, never custom -> custom, unless
the currently running image is already confirmed good.

## 5. Prove the rollback before trusting a build
Before any build is treated as safe, flash a deliberately broken variant
(e.g. wrong Wi-Fi credentials) and confirm the device comes back on stock by
itself. An untested safety net is not a safety net.

## Physical recovery (works regardless of firmware state)
The vent's USB port exposes a CH340 serial bridge (VID 0x1A86, PID 0x7522).
macOS does NOT drive 0x7522 with its built-in driver; the vendor driver
(`CH34xVCPDriver.pkg`, in `~/vent-control/`) is required, after which the
port appears as `/dev/cu.wchusbserial*`.

    zsh ~/vent-control/restore-stock-usb.sh

flashes `panda_vent_v1.0.0-STOCK.bin` to 0x10000 and returns the device to
factory. If the app then reports NVS-full errors, the config area may still
hold a previous firmware's data:

    esptool ... erase-region 0x9000 0x3000    # NVS  (clears saved settings)
    esptool ... erase-region 0xc000 0x2000    # otadata (forces boot of ota_0)
