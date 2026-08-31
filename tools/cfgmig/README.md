# cfgmig / migcheck

Two tools that drive the REAL migration in `firmware/main/pv_cfg.c`, compiled
in rather than reimplemented, so what they exercise is what ships.

## Why they exist

`PV_FX_COUNT` is an array dimension in the MIDDLE of the config struct. Adding
an effect does not append to the stored blob, it moves everything after
`rgb.simple`. A size check alone would throw away every setting on the first
boot after an update, silently, and the only clue would be one line in a serial
log nobody is watching. That has to be proven before it ships, not discovered
after.

## migcheck — the automated one

    cc -I stub -I ../../firmware/main -o migcheck migcheck.c && ./migcheck

Builds blobs in each stored layout with distinctive values, runs the shipping
`pv_cfg_load()`, and asserts that every byte came out the far side: switches,
colours, brightnesses, the H2D tables, the printer, the names, the LED counts.
Also checks what is NEW arrives new rather than as leftover bytes, that the
config is rewritten in the current shape so it migrates once, and that an
unrecognised blob falls back to the factory config instead of being half-read.

Run it on every change to `pv_cfg.c` and on every change to `PV_FX_COUNT` or
`pv_fx_param_t`.

## cfgmig — the one for real recovered bytes

    cc -I stub -I ../../firmware/main -o cfgmig cfgmig.c
    ./cfgmig <blob-from-the-device> [migrated-out.bin]

Feeds the EXACT bytes recovered from a device's flash through the same path and
prints what came out. No device timing, no capture tool, no guessing.

Neither tool takes a device, a network, or a serial cable.
