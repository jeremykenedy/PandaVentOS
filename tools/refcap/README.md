# Reference capture: measuring the factory animations

The clone reproduces the factory UI byte for byte and the factory protocol and
defaults exactly. The one thing that could not be recovered from the binary is
the ANIMATION MATH: the Breathing curve, the Strobing duty, the Wave and
Marquee spatial patterns, the hue rates, and how the 0..100 speed slider maps
to an actual rate.

BIQU ship no source. The render loop was not pinned in the image: the RGB
literal pool is shared across all of IROM, so there is no clean anchor, and
Xtensa's variable length encoding makes a blind scan unreliable. What IS
available is the running factory firmware itself.

So measure it.

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

Those numbers are what the clone's `render_effect()` gets rebuilt against.

## What this does NOT do

It does not flash anything. It sends the same WebSocket messages the factory
web app sends when you move a slider, and it puts every value back when it is
done.
