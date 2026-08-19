#!/usr/bin/env python3
"""How deep into the TLS path do we have to go before it dies?

  rung 1  handshake only            -- https URL that 404s
  rung 2  handshake + headers       -- https .ogg, codec refused before decode
  rung 3  handshake + prebuffer + decoder start -- the known killer
  rung 4  plain-HTTP full path      -- control

Each rung runs from a fresh boot so one crash cannot poison the next.
"""
import sys, time
sys.path.insert(0, "/home/pi/nsr")
from dut_io import open_dut, cmd, read_until

target = sys.argv[1]
RUNGS = [
    ("1 handshake only (404)",      "https://smoothjazz.cdnstream1.com/definitely-not-a-stream-404"),
    ("2 headers, codec refused",    "https://radio.chinesemusicworld.com/chinesemusic.ogg"),
    ("3 full path (known killer)",  "https://smoothjazz.cdnstream1.com/2585_64.aac"),
    ("4 plain HTTP full path",      "http://icecast.radiofrance.fr/fipjazz-hifi.aac"),
]
for label, url in RUNGS:
    try:
        s = open_dut(target)
    except Exception as e:
        print("%-32s OPEN FAILED %s" % (label, e), flush=True); continue
    if not s.booted:
        print("%-32s NO BANNER" % label, flush=True); s.close(); continue
    cmd(s, "stop", 2); time.sleep(5)
    s.reset_input_buffer()
    s.write(("tune " + url + "\n").encode()); s.flush()
    txt, crashed = read_until(s, 45)
    tail = [l.strip() for l in txt.splitlines()
            if l.strip().startswith(("[stream]", "[audio]", "[icy]"))][:4]
    print("%-32s %-6s | %s" % (label, "CRASH" if crashed else "ok", " / ".join(tail)[:120]), flush=True)
    try: s.close()
    except Exception: pass
    time.sleep(3)
