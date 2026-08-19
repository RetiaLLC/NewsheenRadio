#!/usr/bin/env python3
"""Identify what firmware each puck is running, without assuming."""
import sys, time, serial
port = sys.argv[1]
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()
s.rts = True; time.sleep(0.2); s.rts = False      # reset to catch the banner
buf = b""; end = time.time() + 25
while time.time() < end:
    d = s.read(4096)
    if d:
        buf += d
        if b"Newsheen Radio" in buf: break
time.sleep(1)
s.write(b"\nstatus\n"); s.flush()
end = time.time() + 6
while time.time() < end:
    d = s.read(8192)
    if d: buf += d
t = buf.decode("utf-8", "replace")
print("=== %s ===" % port)
import re
for pat in (r"Newsheen Radio.*", r"^\s*(wifi|mac|chip|heap|psram|station|state)\s*:.*",
            r"MAC[: ].*", r"^\[wifi\].*", r"^\[boot\].*"):
    for m in re.finditer(pat, t, re.M):
        print("   " + m.group(0).strip()[:110])
if "Newsheen" not in t:
    print("   (no Newsheen banner) last bytes: %r" % t[-300:])
s.close()
