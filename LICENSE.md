# License

## VentOS

MIT License

Copyright (c) 2026 Jeremy Kenedy

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Third party

The MIT grant above covers the code in this repository. It does not, and
cannot, cover the following, which have their own terms.

| Component | Where | Terms |
| --- | --- | --- |
| BIQU Panda Vent firmware image, web UI, manuals and 3D models | `factory/` | CC BY-NC-ND 4.0, see `factory/docs/BIQU-LICENSE.md` |
| Heroicons | icon sprite in the web UI | MIT, Tailwind Labs |
| iro.js 5.5.2 | `firmware/main/vendor/iro/` | MPL-2.0, text at `firmware/main/vendor/iro/LICENSE.txt` |
| Beer CSS | stylesheet and script in the web UI | MIT |
| ESP-IDF | build dependency, not vendored | Apache 2.0, Espressif |
| cJSON | via ESP-IDF | MIT |

### About `factory/`

`factory/` holds BIQU's own published artifacts, unmodified, kept as the
reference this project is measured against. BIQU releases them under
CC BY-NC-ND 4.0: attribution, non commercial, no derivatives. They are included
here for reference and for the ability to put the stock firmware back on a
device. Nothing in `factory/` is under the MIT grant above, and no adapted or
modified copy of BIQU's work is redistributed by this project.

VentOS is independent firmware for the BIQU Panda Vent. It is not affiliated
with, endorsed by, or supported by Shenzhen BIGTREE Technology Co., Ltd., BIQU
or BIGTREETECH. It is a reimplementation, not a modification of their firmware.
"Panda Vent" is their product name and is used here only to identify the
hardware this runs on. Bambu Lab, AMS and P2S are Bambu Lab's.

### Warranty and risk

Flashing third party firmware to a device is done at your own risk and may void
your warranty. The MIT text above disclaims all warranty, and that disclaimer
is the whole of what is offered. Read `firmware/SAFETY.md` before the first
flash and take the full flash backup it asks for.
