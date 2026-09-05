# Stock Wi-Fi salvage check

A vent arriving from BIQU's firmware has a full NVS, so `nvs_flash_init()`
answers `ESP_ERR_NVS_NO_FREE_PAGES` and the only way on is to erase it -- which
takes stock's Wi-Fi with it, long before anything gets to read it. The firmware
reads the credentials straight out of the raw partition first, before the
erase, because NVS cannot be opened at that point.

That path runs exactly once in a vent's life, on the first boot after
conversion, and there is no vent here still on stock firmware to run it on.
So it is tested on the host instead.

`nvscheck.c` `#include`s `firmware/main/pv_wifi.c`. It does not carry a copy of
the parser: what is exercised is the shipping source, compiled against stubs.

    python3 mkimages.py images
    cc -I stub -I ../../firmware/main -o nvscheck nvscheck.c
    ./nvscheck images/nvs-stock.bin images/nvs-stock-populated.bin images/nvs-ours.bin

## The images are generated, not dumped

`mkimages.py` builds NVS partitions in the real on-flash format with invented
values. The same binary was run against genuine dumps -- a vent still on stock
and a vent already converted -- and read them identically, which is what gives
the generated ones their authority.

Those dumps are not in this repository and should not be: an NVS partition
holds the Wi-Fi password, the printer serial and the printer's LAN access code
in plain text. That is the whole reason these are generated.

## What it checks

| | |
|---|---|
| a stock vent never given Wi-Fi | nothing to salvage, and it says so |
| a stock vent that was on a network | ssid and password come back whole |
| a vent already converted | nothing to salvage, and none invented |
| a blank partition | does not walk off the end |

Each was watched to fail: widening the length gate from 97 to 999 turns three
of the six red.
