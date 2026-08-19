#!/usr/bin/env python3
"""Break a puck out of the auto-resume crash loop.

The device only lives ~6 s per cycle, so this opens the port the instant it
enumerates and hammers `stop`, then parks it on a plain-HTTP station so the
station saved in NVS is one that cannot crash on the next boot.
"""
import sys, time, glob, os, serial

MAC = sys.argv[1]
SAFE = sys.argv[2] if len(sys.argv) > 2 else "http://icecast.radiofrance.fr/fipjazz-hifi.aac"


def node():
    for l in glob.glob("/dev/serial/by-id/*"):
        if MAC.upper() in l.upper():
            return os.path.realpath(l)
    return None


deadline = time.time() + 180
while time.time() < deadline:
    p = node()
    if not p:
        time.sleep(0.2); continue
    try:
        s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0.1
        s.dtr = False; s.rts = False; s.open()
    except Exception:
        time.sleep(0.2); continue
    try:
        # Spam stop through the whole live window; one of these lands before
        # the decoder starts.
        for _ in range(60):
            s.write(b"\nstop\n"); s.flush()
            time.sleep(0.1)
        time.sleep(1)
        s.reset_input_buffer()
        s.write(b"\nstatus\n"); s.flush()
        time.sleep(2)
        t = s.read(8192).decode("utf-8", "replace")
        if "state" in t:
            state = [l.strip() for l in t.splitlines() if l.strip().startswith("state")]
            print("caught it:", state, flush=True)
            # Park on something safe so the next boot cannot loop.
            s.write(("tune " + SAFE + "\n").encode()); s.flush()
            time.sleep(12)
            s.reset_input_buffer()
            s.write(b"\nstatus\n"); s.flush()
            time.sleep(2)
            t2 = s.read(8192).decode("utf-8", "replace")
            for l in t2.splitlines():
                if l.strip().startswith(("state", "station", "wifi")):
                    print("   " + l.strip()[:100], flush=True)
            s.close()
            print("RESCUED", flush=True)
            sys.exit(0)
        s.close()
    except Exception:
        try: s.close()
        except Exception: pass
    time.sleep(0.2)
print("could not catch it", flush=True)
sys.exit(1)
