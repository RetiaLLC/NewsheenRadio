#!/usr/bin/env python3
"""Per-station peak stack depth, with a reboot between each.

usStackHighWaterMark is a min-ever figure, so attribution requires starting
from a fresh boot for every station rather than reading a cumulative low.
"""
import re, sys, time, serial

STATIONS = [
    ("idle-no-playback",    None),
    ("plainHTTP-mp3",  "http://ice1.somafm.com/groovesalad-128-mp3"),
    ("plainHTTP-aac",  "http://icecast.radiofrance.fr/fipjazz-hifi.aac"),
    ("TLS-aac",        "https://smoothjazz.cdnstream1.com/2585_64.aac"),
]
WATCH = ("audio", "netfill", "loopTask", "tiT", "leds")

def boot(s):
    s.rts = True; time.sleep(0.2); s.rts = False
    end = time.time()+30; buf=b""
    while time.time()<end:
        d=s.read(4096)
        if d:
            buf+=d
            if b"Newsheen Radio" in buf: time.sleep(3); return True
    return False

def cmd(s, c, wait=2.0):
    s.write((c+"\n").encode()); s.flush()
    out=b""; t=time.time()+wait
    while time.time()<t:
        d=s.read(8192)
        if d: out+=d
    return out.decode("utf-8","replace")

def lows(s, acc):
    t = cmd(s, "tasks", 2.5)
    for line in t.splitlines():
        m = re.match(r"\s*(\S+)\s+prio=(\d+)\s+core=(-?\d+)\s+state=(\d+)\s+stack_free=(\d+)", line)
        if m:
            name, free = m.group(1), int(m.group(5))
            if free < acc.get(name, 10**9): acc[name] = free
    return acc

s = serial.Serial(); s.port="/dev/ttyACM0"; s.baudrate=115200; s.timeout=0.2
s.dtr=False; s.rts=False; s.open()

results = {}
for label, url in STATIONS:
    if not boot(s):
        print("%-18s BOOT FAILED" % label); continue
    s.reset_input_buffer()
    cmd(s, "stop", 1.5)
    if url:
        cmd(s, "tune "+url, 1.0)
        time.sleep(12)                 # through connect + prebuffer + decoder start
    acc = {}
    for _ in range(10):
        lows(s, acc); time.sleep(1.5)
    results[label] = acc
    got = " ".join("%s=%d" % (k, acc[k]) for k in WATCH if k in acc)
    print("%-18s %s" % (label, got), flush=True)

print("\n=== MIN FREE STACK BY WORKLOAD (audio task built at 32768) ===")
print("%-18s %8s %8s %8s" % ("workload", "audio", "netfill", "tiT"))
for label, acc in results.items():
    print("%-18s %8s %8s %8s" % (label,
          acc.get("audio","-"), acc.get("netfill","-"), acc.get("tiT","-")))
s.close()
