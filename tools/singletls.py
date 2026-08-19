#!/usr/bin/env python3
"""One TLS tune from a quiet device -- is teardown of a prior stream required?

Boots, stops any auto-resumed stream, waits for things to settle, then tunes a
single HTTPS station and holds. If this survives, TLS setup alone is not enough
and the fault needs a preceding stream teardown.
"""
import sys, time, serial
URL = sys.argv[1] if len(sys.argv)>1 else "https://smoothjazz.cdnstream1.com/2585_64.aac"
HOLD = int(sys.argv[2]) if len(sys.argv)>2 else 90
s=serial.Serial(); s.port="/dev/ttyACM0"; s.baudrate=115200; s.timeout=0.2
s.dtr=False; s.rts=False; s.open()
s.rts=True; time.sleep(0.2); s.rts=False
end=time.time()+30; buf=b""
while time.time()<end:
    d=s.read(4096)
    if d:
        buf+=d
        if b"Newsheen Radio" in buf: break
# stop whatever auto-resume started, then let everything settle
s.write(b"stop\n"); s.flush()
t=time.time()+20
while time.time()<t:
    s.read(4096)
s.reset_input_buffer()
print("device quiet; tuning ONE TLS station: %s" % URL, flush=True)
s.write(("tune "+URL+"\n").encode()); s.flush()
acc=b""; t=time.time()+HOLD
while time.time()<t:
    d=s.read(4096)
    if d:
        acc+=d; low=acc.lower()
        if b"guru" in low or b"stack overflow" in low or b"rebooting" in low:
            print(acc.decode("utf-8","replace")[-900:])
            print("*** CRASHED on a single TLS tune ***", flush=True); s.close(); sys.exit(0)
print("*** SURVIVED %ds on a single TLS tune (no prior teardown) ***" % HOLD, flush=True)
s.close()
