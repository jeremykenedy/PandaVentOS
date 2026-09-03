# Reference capture: measuring the factory animations

This firmware is written to behave like the stock one where the two overlap,
and the hardest part of that is the ANIMATION MATH: the Breathing curve, the
Strobing duty, the Wave and Marquee spatial patterns, the hue rates, and how
the 0..100 speed slider maps to an actual rate. None of it can be read out of
a compiled image in any form worth trusting.

BIQU ship no source, and the compiled image is not a usable substitute for
one. What IS available is a running vent, which is a black box that can be
driven and watched.

So measure it. Everything this directory produces is an OBSERVATION of a
device's visible behaviour -- periods, duty cycles, intensity ranges, travel
speeds, all read off a video -- and the effects here are fitted to those
numbers. That is the whole method, and it is deliberately the whole method:
what goes into this firmware is what a camera saw, not what a disassembler
said.

## Do this while the vent is still on factory firmware

Once it is flashed, the reference is gone.

1. Put the vent where a camera can see the front and one side at the same
   time. A phone on a tripod or propped against something is enough. Lock the
   exposure if the camera allows it, so auto-brightness does not fight the
   measurement. 60 fps if offered.
2. Start recording.
3. Run:

       python3 capture-reference.py <vent-ip>

   It backs the settings up first, waits 5 seconds, then drives the vent
   through every effect, a speed sweep, a brightness sweep, a colour check and
   a reverse-direction check. About three minutes. It restores every setting
   afterwards, including on Ctrl-C and on error.
4. Stop recording.

If anything goes wrong mid-run, put the settings back with:

       python3 capture-reference.py <vent-ip> restore

## Then

       python3 analyse-reference.py <video-file>

Each segment is bracketed by a 1 s white flash and 1 s of darkness, so the
video aligns to the timeline automatically. The analyser reports, per segment,
the period, the duty cycle, the intensity range and the pattern travel speed
in pixels per second, and writes the raw per-frame intensity traces to
`analysis/` for curve fitting.

Those numbers are what `render_effect()` is fitted to.

## What this does NOT do

It does not flash anything. It sends the same WebSocket messages the factory
web app sends when you move a slider, and it puts every value back when it is
done.
