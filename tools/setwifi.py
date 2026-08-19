#!/usr/bin/env python3
"""Set Wi-Fi credentials on a puck and confirm it actually associates.

The CLI takes ssid|pass pipe-separated because SSIDs may contain spaces.
Reports the resulting IP and RSSI rather than assuming the write took.
"""
import sys, time, serial
port, ssid, pw = sys.argv[1], sys.argv[2], sys.argv[3]
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()
s.rts = True; time.sleep(0.2); s.rts = False
buf = b""; end = time.time() + 25
while time.time() < end:
    d = s.read(4096)
    if d:
        buf += d
        if b"Newsheen Radio" in buf: break
time.sleep(2); s.reset_input_buffer()

s.write(("wifi %s|%s\n" % (ssid, pw)).encode()); s.flush()
print("[%s] sent credentials for %s" % (port, ssid), flush=True)

acc = b""; end = time.time() + 60
ok = False
while time.time() < end:
    d = s.read(4096)
    if d:
        acc += d
        if b"connected, IP" in acc:
            ok = True
            break
        if b"join failed" in acc:
            break
time.sleep(1)
s.write(b"\nstatus\n"); s.flush()
end = time.time() + 6
while time.time() < end:
    d = s.read(8192)
    if d: acc += d
t = acc.decode("utf-8", "replace")
for line in t.splitlines():
    if line.strip().startswith(("wifi", "[wifi]", "state")):
        print("   " + line.strip()[:110])
print("[%s] %s" % (port, "ASSOCIATED" if ok else "DID NOT ASSOCIATE"))
s.close()
