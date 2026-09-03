# Backup and restore

Read this before the first flash. There are two different things called a
backup here and they protect against two different failures.

## The two backups

| | Full flash image | Settings snapshot |
| --- | --- | --- |
| **Size** | 4 MB, the whole chip | a few KB of JSON |
| **Contains** | bootloader, partition table, otadata, both app slots, NVS | your settings, as the page sees them |
| **Needs** | a USB cable, or `GET /backup` over the network | the network only |
| **Protects against** | a device that will not boot at all | losing your settings |
| **Can restore** | everything, including the stock firmware | settings only |
| **Is a secret** | **yes** | **yes** |

Both contain your Wi-Fi password and the printer's access code in plain text.
Neither belongs in a git repository, a shared folder, or a forum post.

## Take a full image first

Before you flash anything, take the whole chip and test putting it back.

```bash
python -m esptool --chip esp32 --port <YOUR-PORT> -b 115200 \
  read-flash 0 0x400000 golden-full-4MB.bin
```

**Why all 4 MB and not just the app.** BIQU publishes the application image and
nothing else. There is no public copy of this device's bootloader. If you lose
the bootloader, no upload can put it back, because there is nothing running to
receive the upload. The 4 MB image is the only thing that recovers that, and it
needs a cable either way.

It takes a few minutes at 115200, during which the vent is off the network.

**Flash the same image twice before taking a golden.** An OTA writes only the
inactive slot, so a single flash leaves the other slot holding something else.

## Over the network, no cable

```bash
curl -o backup-$(date +%Y%m%d).bin http://<device>/backup
```

Streams the same 4 MB, in about seventeen seconds. This is an addition; stock
has no such thing.

It is a complete image and it can be written back with esptool over a cable. It
cannot be written back over the network, because the thing that would receive
it is part of what is being replaced.

## Restoring a full image

```bash
python -m esptool --chip esp32 --port <YOUR-PORT> -b 115200 \
  write-flash 0 golden-full-4MB.bin
```

This puts the device back to exactly the moment the image was taken: firmware,
settings, Wi-Fi, printer binding, all of it.

## Settings snapshots

A snapshot is what the page can see, captured over the WebSocket. It is not a
flash image and it will not recover a device that does not boot. It is the fast
way back from "I changed forty things and now the lights look wrong".

Settings survive firmware updates on their own, so a snapshot is for undoing
your own changes, not for surviving an update.

## Going back to the stock firmware

The partition layout is BIQU's, unchanged, which is the point of keeping it.

```bash
curl -X POST -H 'OTA-Type: ota_fw' \
  --data-binary @<BIQU-STOCK-IMAGE.bin> \
  http://<device>/ota
```

One upload, no cable. The stock firmware finds a configuration it does not
recognise and rewrites it with its own defaults, so expect to set the device up
again after going back.

## What survives what

| | Firmware update | Factory reset | Full image restore |
| --- | :---: | :---: | :---: |
| Settings | kept, migrated | cleared | as of the image |
| Wi-Fi | kept | cleared | as of the image |
| Printer binding | kept | cleared | as of the image |
| Language | kept | cleared | as of the image |
| Uploaded animation | lost, it lives in RAM | lost | lost |

## Related

- [`../firmware/SAFETY.md`](../firmware/SAFETY.md) for what will actually brick it
- [`network.md`](network.md) for reaching the device after an update
