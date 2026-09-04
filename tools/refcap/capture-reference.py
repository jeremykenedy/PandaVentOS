#!/usr/bin/env python3
"""
Drive a STOCK Panda Vent through a scripted lighting sequence so its real
animations can be measured from video, then put every setting back exactly as
it was.

This does not flash anything. It sends the same WebSocket messages the factory
web app sends when you move a slider.

    python3 capture-reference.py <ip>          # run the sequence
    python3 capture-reference.py <ip> restore  # put settings back (also runs
                                               # automatically at the end and
                                               # on Ctrl-C)

Point a camera at the vent, start recording, then run it. Each segment is
introduced by a sync flash (1 s full white, 1 s off) so the video can be
aligned automatically afterwards.
"""
import sys, json, time, socket, base64, os, struct, signal

PORT = 80
HOLD = 8.0          # seconds per effect segment
SYNC_ON = 1.0
SYNC_OFF = 1.0

# ---------------------------------------------------------------- websocket

def connect(host):
    s = socket.create_connection((host, PORT), timeout=8)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (host, key)).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    return s, buf.split(b"\r\n\r\n", 1)[1]

def frame(payload):
    m = os.urandom(4)
    p = bytes(b ^ m[i % 4] for i, b in enumerate(payload))
    n = len(payload)
    if n < 126:     h = struct.pack("!BB", 0x81, 0x80 | n)
    elif n < 65536: h = struct.pack("!BBH", 0x81, 0x80 | 126, n)
    else:           h = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
    return h + m + p

def read_docs(s, buf, seconds):
    out, end = [], time.time() + seconds
    s.settimeout(0.8)
    while time.time() < end:
        while len(buf) >= 2:
            b0, b1 = buf[0], buf[1]
            ln = b1 & 0x7F; off = 2
            if ln == 126:
                if len(buf) < 4: break
                ln = struct.unpack("!H", buf[2:4])[0]; off = 4
            elif ln == 127:
                if len(buf) < 10: break
                ln = struct.unpack("!Q", buf[2:10])[0]; off = 10
            if len(buf) < off + ln: break
            payload, buf = buf[off:off+ln], buf[off+ln:]
            if (b0 & 0x0F) == 1:
                try: out.append(json.loads(payload.decode()))
                except Exception: pass
        try: buf += s.recv(8192)
        except socket.timeout: continue
        except Exception: break
    return out, buf

class Vent:
    def __init__(self, host):
        self.host = host
        self.sock, self.buf = connect(host)
        docs, self.buf = read_docs(self.sock, self.buf, 3.0)
        self.state = {}
        for d in docs:
            for k, v in d.items():
                if isinstance(v, dict) and isinstance(self.state.get(k), dict):
                    self.state[k].update(v)
                else:
                    self.state[k] = v
        if "rgb_mode" not in self.state:
            raise SystemExit("no state received from %s" % host)

    def send(self, obj):
        self.sock.sendall(frame(json.dumps(obj).encode()))
        time.sleep(0.30)

    def close(self):
        try: self.sock.close()
        except Exception: pass

# ---------------------------------------------------------------- sequence

EFFECTS = ["Static", "Breathing", "Strobing", "Wave",
           "Marquee", "Color_Cycle", "Rainbow"]

def simple(effect, bg=None, speed=None, rgb=None):
    m = {"effect": effect}
    if bg is not None:    m["bg"] = bg
    if speed is not None: m["speed"] = speed
    if rgb is not None:   m["rgb"] = rgb
    return {"rgb_mode": {"simple_mode": m}}

def sync_flash(v):
    """Unmistakable marker: full white, then dark."""
    v.send({"rgb_switch": {"total_switch": 1}})
    v.send(simple(0, bg=100, speed=50, rgb="FFFFFF"))
    time.sleep(SYNC_ON)
    v.send({"rgb_switch": {"total_switch": 0}})
    time.sleep(SYNC_OFF)
    v.send({"rgb_switch": {"total_switch": 1}})

def segment(v, label, msgs, hold=HOLD):
    sync_flash(v)
    t0 = time.time()
    for m in msgs:
        v.send(m)
    print("  %7.1fs  %s" % (t0 - START, label), flush=True)
    LOG.append({"t": round(t0 - START, 2), "label": label})
    time.sleep(hold)

def run_sequence(v):
    # Deterministic starting point: lights on, no gating, forward direction,
    # Simple mode so one effect at a time is on screen.
    v.send({"rgb_switch": {"total_switch": 1}})
    v.send({"rgb_switch": {"follow_printer": 0}})
    v.send({"rgb_switch": {"follow_vent": 0}})
    v.send({"rgb_switch": {"reverse_light": 0}})
    v.send({"rgb_switch": {"warning_overide": 0}})
    v.send({"rgb_switch": {"current_light_mode": 0}})
    time.sleep(1.0)

    # 1. every effect at a fixed, mid setting
    for i, name in enumerate(EFFECTS):
        segment(v, "effect %d %s  bg=100 speed=50" % (i, name),
                [simple(i, bg=100, speed=50, rgb="FFFFFF")])

    # 2. speed mapping, on the two effects where motion is easiest to time
    for i in (1, 3, 4):
        for sp in (0, 25, 50, 75, 100):
            segment(v, "effect %d %s  speed=%d" % (i, EFFECTS[i], sp),
                    [simple(i, bg=100, speed=sp, rgb="FFFFFF")], hold=6.0)

    # 3. brightness curve, on Static so nothing else moves
    for bg in (0, 5, 10, 25, 50, 75, 100):
        segment(v, "effect 0 Static  bg=%d" % bg,
                [simple(0, bg=bg, speed=50, rgb="FFFFFF")], hold=4.0)

    # 4. colour fidelity at full brightness
    for col in ("FF0000", "00FF00", "0000FF", "FF8000", "FFFFFF"):
        segment(v, "effect 0 Static  rgb=%s" % col,
                [simple(0, bg=100, speed=50, rgb=col)], hold=4.0)

    # 5. reverse direction, to see which way the pattern travels
    segment(v, "effect 4 Marquee  reverse=1",
            [{"rgb_switch": {"reverse_light": 1}},
             simple(4, bg=100, speed=50, rgb="FFFFFF")], hold=8.0)
    v.send({"rgb_switch": {"reverse_light": 0}})

def restore(v, saved):
    """Replay a saved capture. Ordered so per-effect params land before the
    active selection, exactly like the known-good restore path."""
    r = saved["rgb_mode"]
    msgs = []
    for e in r["effects"]:
        msgs.append(simple(e["id"], bg=e["brightness"], speed=e["speed"],
                           rgb=e["color"]))
    for st in r["h2d_mode"]["device_states"]:
        for e in st["effects"]:
            msgs.append({"rgb_mode": {"h2d_mode": {
                "mode": st["device_state_id"], "effect": e["effect_id"],
                "bg": e["brightness"], "speed": e["speed"], "rgb": e["color"]}}})
        msgs.append({"rgb_mode": {"h2d_mode": {
            "mode": st["device_state_id"],
            "effect": st["active_effect_id"]}}})
    for lvl in ("safe", "warn"):
        w = r["warning_hot_mode"][lvl]
        for p in w["params"]:
            msgs.append({"rgb_mode": {"warning_hot_mode": {
                lvl: {"effect": p["index"], "bg": p["bg"], "speed": p["speed"]}}}})
        msgs.append({"rgb_mode": {"warning_hot_mode": {
            lvl: {"effect": w["current_effect"]}}}})
    msgs.append(simple(r["current_simple_effect"]))
    for k, key in (("is_follow_printer", "follow_printer"),
                   ("is_follow_vent", "follow_vent"),
                   ("is_reverse", "reverse_light"),
                   ("warning_sw", "warning_overide"),
                   ("light_on_off", "total_switch")):
        msgs.append({"rgb_switch": {key: 1 if r[k] else 0}})
    msgs.append({"rgb_switch": {"current_light_mode": r["rgb_light_mode"]}})
    for m in msgs:
        v.send(m)
    return len(msgs)

# ---------------------------------------------------------------- main

START = time.time()
LOG = []
BACKUP = "reference-BEFORE.json"

def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    host = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "run"

    v = Vent(host)

    # `restore` is the command this tool tells you to run when a capture went
    # wrong. It used to read the device's CURRENT state, write that over
    # reference-BEFORE.json, and then "restore" it -- so the one command a
    # user runs after an aborted run destroyed the only record of their
    # original lighting and put the half-finished sequence back instead.
    # Restore reads the snapshot off disk and writes nothing.
    if mode == "restore":
        if not os.path.exists(BACKUP):
            raise SystemExit(
                "no %s here, so there is nothing to restore from.\n"
                "It is written at the start of a run; run this from the same\n"
                "directory as that run." % BACKUP)
        with open(BACKUP) as f:
            saved = json.load(f)
        n = restore(v, saved)
        print("replayed %d messages from %s" % (n, BACKUP))
        v.close(); return

    saved = json.loads(json.dumps(v.state))
    # And a second run does not overwrite the first run's snapshot.
    if os.path.exists(BACKUP):
        keep = BACKUP + time.strftime(".%Y%m%d-%H%M%S")
        os.rename(BACKUP, keep)
        print("kept the previous snapshot as %s" % keep)
    with open(BACKUP, "w") as f:
        json.dump(saved, f, indent=1)
    print("settings backed up to %s" % BACKUP)

    def put_back(*_):
        print("\nrestoring settings...")
        try:
            n = restore(v, saved)
            print("restored (%d messages)" % n)
        except Exception as e:
            print("RESTORE FAILED: %s" % e)
            print("run:  python3 capture-reference.py %s restore" % host)
        with open("reference-timeline.json", "w") as f:
            json.dump(LOG, f, indent=1)
        print("timeline written to reference-timeline.json")
        v.close()
        sys.exit(0)

    signal.signal(signal.SIGINT, put_back)

    print("\nSTART RECORDING NOW. Sequence takes about 3 minutes.")
    print("Each segment starts with a 1 s white flash then 1 s dark.\n")
    time.sleep(5)
    globals()["START"] = time.time()
    try:
        run_sequence(v)
    finally:
        put_back()

if __name__ == "__main__":
    main()
