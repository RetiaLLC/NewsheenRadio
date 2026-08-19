#!/usr/bin/env python3
"""Reproduce the TLS station-change memory corruption.

Switches between three HTTPS stations on a short dwell and watches for a panic.
The device fails within 2 to 10 switches. tools/plaintest.py is the control:
the same test over plain HTTP survives 30 switches, which is what isolates the
fault to TLS session setup rather than to streaming or decoding.

Usage: racetest.py [dwell_seconds] [cycles]
"""
import re, sys, time, serial

TLS = ["https://smoothjazz.cdnstream1.com/2585_64.aac",
       "https://cast2.midiazdx.com.br:7260/stream",
       "https://ice1.somafm.com/groovesalad-128-mp3"]
DWELL = float(sys.argv[1]) if len(sys.argv)>1 else 6.0
CYCLES = int(sys.argv[2]) if len(sys.argv)>2 else 40

s=serial.Serial(); s.port="/dev/ttyACM0"; s.baudrate=115200; s.timeout=0.2
s.dtr=False; s.rts=False; s.open()
s.rts=True; time.sleep(0.2); s.rts=False
end=time.time()+30; buf=b""
while time.time()<end:
    d=s.read(4096)
    if d:
        buf+=d
        if b"Newsheen Radio" in buf: time.sleep(3); break
s.reset_input_buffer()

def send(c):
    s.write((c+"\n").encode()); s.flush()

acc=b""; crashed=False
print("switching every %.1fs, %d cycles" % (DWELL, CYCLES), flush=True)
for i in range(CYCLES):
    url = TLS[i % len(TLS)]
    send("stop"); time.sleep(0.3)          # deliberately short gap: stop -> tune
    send("tune "+url)
    t=time.time()+DWELL
    while time.time()<t:
        d=s.read(4096)
        if d:
            acc+=d
            low=acc.lower()
            if b"stack overflow" in low or b"guru" in low or b"backtrace" in low or b"rebooting" in low:
                time.sleep(3)
                x=s.read(65536)
                if x: acc+=x
                print(acc.decode("utf-8","replace")[-2500:])
                print("\n*** CRASH on cycle %d (%s) ***" % (i+1, url), flush=True)
                crashed=True; break
            if len(acc)>120000: acc=acc[-15000:]
    if crashed: break
    if (i+1) % 5 == 0: print("  ...%d switches ok" % (i+1), flush=True)

if not crashed:
    print("\nno crash in %d switches at 32K" % CYCLES, flush=True)
s.close()
