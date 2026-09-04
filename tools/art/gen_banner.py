#!/usr/bin/env python3
"""Build the README banner, light and dark, from the app's own M3 tokens.

One generator, two palettes, so the two files can never drift apart. The panda
is drawn rather than embedded: a banner is the one image that gets scaled to
whatever width a reader's browser feels like, and a 48 px PNG blown up to 120
looks like a mistake.

    python3 tools/art/gen_banner.py        writes screenshots/banner-*.svg
"""

import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "screenshots")

LIGHT = dict(
    surface="#F7FAF5", on_surface="#2B352F", on_surface_variant="#58615A",
    primary="#316A4D", outline="#737D76", face="#FFFFFF", ink="#2B2B2B",
)

DARK = dict(
    surface="#0B0F0C", on_surface="#DEE8DF", on_surface_variant="#A4AEA5",
    primary="#A4D1B5", outline="#6E7870", face="#F4F7F3", ink="#20261F",
)

PINK = "#F26B8A"

# The four colours a strip can carry, used as the LED motif on the right.
LEDS = ["#8FD3A6", "#5FBF88", "#3FA36C", "#2E7D32"]

FONT = ("ui-sans-serif,-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,"
        "'Helvetica Neue',Arial,sans-serif")


def panda(p, x, y, s):
    """The face mark, drawn in a 100x100 box scaled by s and placed at x,y."""
    ink, face = p["ink"], p["face"]
    return f'''  <g transform="translate({x},{y}) scale({s})">
    <circle cx="23" cy="25" r="17" fill="{ink}"/>
    <circle cx="77" cy="25" r="17" fill="{ink}"/>
    <circle cx="23" cy="25" r="8.5" fill="{PINK}"/>
    <circle cx="77" cy="25" r="8.5" fill="{PINK}"/>
    <ellipse cx="50" cy="56" rx="43" ry="38" fill="{face}"/>
    <ellipse cx="31" cy="53" rx="13" ry="15.5" fill="{ink}" transform="rotate(-14 31 53)"/>
    <ellipse cx="69" cy="53" rx="13" ry="15.5" fill="{ink}" transform="rotate(14 69 53)"/>
    <circle cx="32.5" cy="52" r="5.6" fill="{face}"/>
    <circle cx="67.5" cy="52" r="5.6" fill="{face}"/>
    <circle cx="33.5" cy="53.5" r="3.1" fill="{ink}"/>
    <circle cx="66.5" cy="53.5" r="3.1" fill="{ink}"/>
    <ellipse cx="13" cy="69" rx="7.5" ry="5" fill="{PINK}" opacity=".75"/>
    <ellipse cx="87" cy="69" rx="7.5" ry="5" fill="{PINK}" opacity=".75"/>
    <ellipse cx="50" cy="68" rx="4.6" ry="3.4" fill="{ink}"/>
    <path d="M50 71.5 v3.5" stroke="{ink}" stroke-width="2.2" stroke-linecap="round" fill="none"/>
    <path d="M43.5 75 q6.5 6 13 0" stroke="{ink}" stroke-width="2.2"
          stroke-linecap="round" fill="none"/>
  </g>'''


def dial(p, cx, cy, r):
    """The vent dial: the same 180 degree arc the page draws."""
    track, arc = p["outline"], p["primary"]

    def pt(deg):
        a = math.radians(deg)
        return f"{cx + r * math.cos(a):.2f} {cy + r * math.sin(a):.2f}"

    return f'''  <g fill="none" stroke-linecap="round">
    <circle cx="{cx}" cy="{cy}" r="{r}" stroke="{track}" stroke-width="7" opacity=".22"/>
    <path d="M {pt(135)} A {r} {r} 0 1 1 {pt(45)}" stroke="{arc}" stroke-width="7"/>
    <circle cx="{cx}" cy="{cy}" r="{r - 17}" stroke="{track}" stroke-width="2" opacity=".28"/>
  </g>'''


def leds(p, x, y, gap, rr):
    out = []
    for i, c in enumerate(LEDS):
        out.append(f'    <circle cx="{x + i * gap}" cy="{y}" r="{rr}" fill="{c}"/>')
    for i in range(len(LEDS), len(LEDS) + 3):
        out.append(f'    <circle cx="{x + i * gap}" cy="{y}" r="{rr}" '
                   f'fill="{p["outline"]}" opacity=".22"/>')
    return "  <g>\n" + "\n".join(out) + "\n  </g>"


def build(p):
    W, H = 880, 215
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}"
     viewBox="0 0 {W} {H}" role="img" aria-label="PandaVentOS">
  <rect width="{W}" height="{H}" rx="28" fill="{p['surface']}"/>
  <rect x="1" y="1" width="{W - 2}" height="{H - 2}" rx="27" fill="none"
        stroke="{p['outline']}" stroke-width="1" opacity=".35"/>
{panda(p, 56, 50, 1.16)}
  <text x="206" y="98" font-family="{FONT}" font-size="52" font-weight="700"
        letter-spacing="-1" fill="{p['on_surface']}">PandaVent<tspan
        fill="{p['primary']}" font-weight="600"> OS</tspan></text>
  <text x="208" y="132" font-family="{FONT}" font-size="17" font-weight="400"
        letter-spacing=".2" fill="{p['on_surface_variant']}">Open firmware for the BIQU Panda Vent</text>
  <text x="208" y="160" font-family="{FONT}" font-size="14" font-weight="500"
        letter-spacing="1.4" fill="{p['primary']}">18 EFFECTS  &#183;  MATERIAL AWARE VENTING  &#183;  24 LANGUAGES</text>
{dial(p, 764, 90, 44)}
{leds(p, 702, 168, 21, 6.5)}
</svg>
'''


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, pal in (("light", LIGHT), ("dark", DARK)):
        path = os.path.join(OUT, f"banner-{name}.svg")
        with open(path, "w") as fh:
            fh.write(build(pal))
        print(f"{os.path.relpath(path, ROOT)}  {os.path.getsize(path)} bytes")
