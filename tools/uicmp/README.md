# UI comparison harness

Proves the clone's state document drives the factory web app identically to
the stock device.

Both harnesses compile the FIRMWARE'S OWN `firmware/main/pv_json.c`. They do
not carry a copy of it, which is the whole point: what is tested is the
shipping source file, not a reimplementation of it.

* `harness.c` compiles `main/pv_json.c` natively against cJSON with the
  factory defaults and prints the state document the firmware would emit.
* `harness_live.c` does the same but seeded from a capture of a live stock
  device, so its output can be compared to that capture directly.
* `shot.js` serves `factory/www/index.html`, stubs `window.WebSocket` to
  deliver a given state document, walks every screen and captures each at 2x.

Run it twice, once per document, then diff the two directories. The captures
must be byte-identical.

    gcc -I. -I<cjson> -I../../firmware/main -o harness_live \
        harness_live.c ../../firmware/main/pv_json.c <cjson>/cJSON.c
    ./harness_live > mine.json
    node shot.js live.json  shots-stock
    node shot.js mine.json  shots-clone

Do not commit captures. They contain the printer serial and the network
values of whatever device produced them.

`harness_live.c` expects a `seed_live.h` generated from a capture of your own
device. That file and the capture stay out of this repo: they hold the
printer serial, the access code and the network credentials.
