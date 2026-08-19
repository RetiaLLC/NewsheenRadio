#!/usr/bin/env python3
"""Phase 0: tunes-to-failure over N trials, resilient to USB re-enumeration.

Native-USB pucks re-enumerate whenever they reset, so the device node moves and
any RTS pulse kills the fd we are holding. Trials therefore never reset in
hardware: they open by MAC, verify the CLI answers, quiesce with `stop`, tune,
and watch. After a crash the device reboots itself and comes back on a new node.

Usage: phase0.py <mac|port> <trials> [url] [watch_seconds]
"""
import sys, time, re, statistics
sys.path.insert(0, "/home/pi/nsr")
from dut_io import open_dut, cmd, read_until, resolve

target = sys.argv[1]
trials = int(sys.argv[2])
url = sys.argv[3] if len(sys.argv) > 3 else "https://smoothjazz.cdnstream1.com/2585_64.aac"
watch = int(sys.argv[4]) if len(sys.argv) > 4 else 60


def connect(total=120):
    """Open and confirm the CLI answers.

    Tries a bare `status` first (the device may already be up and streaming),
    and only then waits for a boot banner. Never resets in hardware: on
    native USB that re-enumerates the puck and invalidates the fd.
    """
    end = time.time() + total
    while time.time() < end:
        try:
            s = open_dut(target, reset=False, wait_banner=False)
        except Exception:
            time.sleep(2); continue
        try:
            t = cmd(s, "status", 4)
            if "state" in t or "wifi " in t:
                return s, t
            # Not up yet - sit through a boot and try once more.
            buf = b""
            tend = time.time() + 25
            while time.time() < tend:
                d = s.read(4096)
                if d:
                    buf += d
                    if b"Newsheen Radio" in buf:
                        break
            time.sleep(1)
            s.reset_input_buffer()
            t = cmd(s, "status", 4)
            if "state" in t or "wifi " in t:
                return s, t
        except Exception:
            pass
        try: s.close()
        except Exception: pass
        time.sleep(2)
    return None, ""


results = []
for i in range(trials):
    s, st = connect()
    if not s:
        print("trial %2d  NO CLI (device not answering)" % (i + 1), flush=True)
        time.sleep(10); continue

    m = re.search(r"rssi=(-?\d+)", st)
    rssi = int(m.group(1)) if m else None

    cmd(s, "stop", 2)
    time.sleep(6)
    try:
        s.reset_input_buffer()
    except Exception:
        pass

    t0 = time.time()
    try:
        s.write(("tune " + url + "\n").encode()); s.flush()
        txt, crashed = read_until(s, watch)
    except Exception as e:
        # The port vanishing mid-watch is itself the crash signature here.
        txt, crashed = str(e), True
    dt = time.time() - t0

    sig = ""
    for pat in (r"stack overflow in task \S+", r"Guru Meditation Error[^\n]*",
                r"Debug exception reason[^\n]*"):
        mm = re.search(pat, txt)
        if mm:
            sig = mm.group(0).strip()[:68]; break
    results.append((crashed, dt, rssi, sig))
    print("trial %2d  %-5s  %5.1fs  rssi=%s  %s"
          % (i + 1, "CRASH" if crashed else "ok", dt, rssi, sig), flush=True)
    try: s.close()
    except Exception: pass
    time.sleep(12 if crashed else 3)      # let it reboot and re-enumerate

n = len(results)
c = [r for r in results if r[0]]
print("\n=== %s : %d/%d trials crashed ===" % (target, len(c), n), flush=True)
if c:
    ts = sorted(r[1] for r in c)
    print("time-to-crash: median %.1fs  min %.1fs  max %.1fs"
          % (statistics.median(ts), ts[0], ts[-1]), flush=True)
rs = [r[2] for r in results if r[2] is not None]
if rs:
    print("rssi: min %d  max %d  mean %.1f" % (min(rs), max(rs), sum(rs) / len(rs)), flush=True)
