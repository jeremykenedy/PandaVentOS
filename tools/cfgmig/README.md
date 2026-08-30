# cfgmig — run a real stored config through the real migration

Every time `pv_cfg_t` changes shape, the loader in `pv_cfg.c` has to lift the
older layout forward without losing a setting. This harness feeds a config
blob straight into that code and prints what came out, so the question
"did the migration keep the printer binding" is answered by running it rather
than by reading it.

    cc -Istub -I../../firmware/main -o cfgmig cfgmig.c ../../firmware/main/pv_cfg.c
    ./cfgmig <blob> [out.bin]

`<blob>` is the raw NVS value of the `cfg` key. `tools/nvsgrab.py` pulls one
out of a full-flash image.

## Why it exists

On 2026-08-30 the config reached 2320 bytes. The NVS partition is 12 KB, one
of its three pages is reserved for garbage collection, the Wi-Fi stack's own
keys take about 5 KB of what remains, and `nvs_set_blob` writes the new copy
before releasing the old. Two copies stopped fitting. The write failed, the
failure was logged and otherwise ignored, and the device ran perfectly from
RAM until the next reboot threw every setting away.

Three things came out of that and all three are in the tree:

- colours are stored as three raw bytes rather than seven bytes of text,
  which took the config from 2320 to 1312
- `PV_CFG_MAX_BYTES` in `pv_cfg.c` fails the build if it grows again
- a failed save sets `g_live.cfg_save_failed`, which reaches the state
  document and puts a banner on the page

## Blobs are secrets

A config blob holds the Wi-Fi password, the printer serial and its access
code in plaintext. Keep them in `private/`, which is gitignored. Never commit
one.
