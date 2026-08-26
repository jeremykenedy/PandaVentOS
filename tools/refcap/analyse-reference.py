#!/usr/bin/env python3
"""
Turn a video of the STOCK vent running capture-reference.py's sequence into
the numbers the clone has to reproduce.

    python3 analyse-reference.py <video> [reference-timeline.json]

Method:
  1. decode to grayscale frames at a known fps (ffmpeg)
  2. find the 1 s white / 1 s dark sync flashes that bracket each segment
  3. per segment, measure
       - mean intensity over time  -> period, duty cycle, waveform shape
       - per-column intensity      -> spatial pattern, wavefront width,
                                      travel direction and pixels per second
  4. print a table, and dump raw traces to analysis/ for curve fitting

Needs ffmpeg and numpy. Nothing here touches the device.
"""
import sys, os, json, subprocess, shutil

def die(m): raise SystemExit("error: " + m)

def decode(video, fps, outdir):
    if shutil.which("ffmpeg") is None: die("ffmpeg not found")
    os.makedirs(outdir, exist_ok=True)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", video,
                    "-vf", "fps=%d,format=gray,scale=320:-1" % fps,
                    os.path.join(outdir, "f%06d.png")], check=True)
    return sorted(os.listdir(outdir))

def main():
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    video = sys.argv[1]
    timeline = sys.argv[2] if len(sys.argv) > 2 else "reference-timeline.json"
    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        die("needs numpy and pillow (pip install numpy pillow)")

    FPS = 60
    frames = decode(video, FPS, "frames")
    if not frames: die("no frames decoded")
    print("decoded %d frames at %d fps" % (len(frames), FPS))

    arr = np.stack([np.asarray(Image.open(os.path.join("frames", f)),
                               dtype=np.float32) for f in frames])
    mean = arr.reshape(len(arr), -1).mean(axis=1)
    mean = (mean - mean.min()) / max(1e-6, (mean.max() - mean.min()))

    # sync flash = >=0.6 s bright then >=0.6 s dark
    hi, lo = mean > 0.75, mean < 0.15
    marks, i = [], 0
    need = int(0.6 * FPS)
    while i < len(mean) - 2 * need:
        if hi[i:i+need].all():
            j = i + need
            while j < len(mean) and hi[j]: j += 1
            if lo[j:j+need].all():
                k = j
                while k < len(mean) and lo[k]: k += 1
                marks.append(k)
                i = k
                continue
        i += 1
    print("found %d sync markers" % len(marks))

    labels = []
    if os.path.exists(timeline):
        labels = [e["label"] for e in json.load(open(timeline))]

    os.makedirs("analysis", exist_ok=True)
    print("\n%-38s %8s %8s %8s %9s" % ("segment", "period", "duty", "min/max", "px/s"))
    print("-" * 78)
    for n, s in enumerate(marks):
        e = marks[n+1] if n + 1 < len(marks) else len(mean)
        seg = mean[s:e]
        if len(seg) < FPS: continue
        body = seg[int(0.3*FPS):]
        if len(body) < FPS: continue
        # period by autocorrelation
        x = body - body.mean()
        ac = np.correlate(x, x, "full")[len(x)-1:]
        ac /= max(1e-9, ac[0])
        peak = None
        for k in range(int(0.05*FPS), min(len(ac)-1, int(6*FPS))):
            if ac[k] > 0.35 and ac[k] >= ac[k-1] and ac[k] >= ac[k+1]:
                peak = k; break
        period = peak / FPS if peak else float("nan")
        duty = float((body > (body.max()+body.min())/2).mean())
        # spatial: per-column profile, cross-correlate consecutive frames
        cols = arr[s:e].mean(axis=1)
        pxs = float("nan")
        if len(cols) > 4:
            shifts = []
            for t in range(1, min(len(cols)-1, 90)):
                a = cols[t] - cols[t].mean(); b = cols[t+1] - cols[t+1].mean()
                c = np.correlate(a, b, "full")
                shifts.append(int(np.argmax(c)) - (len(a) - 1))
            if shifts: pxs = float(np.median(shifts)) * FPS
        label = labels[n] if n < len(labels) else "segment %d" % n
        print("%-38s %8s %8.3f %4.2f/%.2f %9s" % (
            label[:38],
            "%.3fs" % period if period == period else "  n/a",
            duty, float(body.min()), float(body.max()),
            "%.1f" % pxs if pxs == pxs else "  n/a"))
        np.savetxt(os.path.join("analysis", "seg%02d.txt" % n), seg, fmt="%.5f")

    print("\nraw intensity traces in analysis/ , one file per segment.")
    print("Fit those to get the exact Breathing curve and speed mapping.")

if __name__ == "__main__":
    main()
