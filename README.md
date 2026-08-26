# PandaVent

Ground-up rebuild for the BIQU Panda Vent, starting from an exact copy of the
factory application. Read FACTORY-BASELINE.md first. Phase 0 = factory copy
(this tree). Phase 1 = re-create the factory app as source, verified on the
real hardware, before any new feature.

## Before this repo is published anywhere

Commits `f425104` and `bb717e8` contain a live Wi-Fi credential that was
pasted into `firmware/main/pv_cfg.c` by mistake as the AP password default.
The working tree no longer has it, but the history does. Nothing has been
pushed and there is no remote configured, so the history can still be
rewritten cleanly. Do that before adding a remote.
