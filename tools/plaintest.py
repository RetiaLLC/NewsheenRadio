#!/usr/bin/env python3
"""Same reproducer, plain HTTP only -- isolates TLS teardown as the trigger."""
import sys, time, serial
PLAIN=["http://ice1.somafm.com/groovesalad-128-mp3",
       "http://icecast.radiofrance.fr/fipjazz-hifi.aac",
       "http://ice2.somafm.com/dronezone-128-mp3"]
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
acc=b""; crashed=False
for i in range(30):
    s.write(b"stop\n"); s.flush(); time.sleep(0.3)
    s.write(("tune "+PLAIN[i%3]+"\n").encode()); s.flush()
    t=time.time()+6
    while time.time()<t:
        d=s.read(4096)
        if d:
            acc+=d; low=acc.lower()
            if b"guru" in low or b"stack overflow" in low or b"rebooting" in low:
                print(acc.decode("utf-8","replace")[-1200:])
                print("*** CRASH on cycle %d (%s) ***"%(i+1,PLAIN[i%3]),flush=True)
                crashed=True; break
            if len(acc)>120000: acc=acc[-15000:]
    if crashed: break
    if (i+1)%5==0: print("  ...%d plain-HTTP switches ok"%(i+1),flush=True)
if not crashed: print("\nno crash in 30 plain-HTTP switches",flush=True)
s.close()
