#!/usr/bin/env python3
"""Stream Mac audio to the Newsheen as an ordinary MP3 HTTP stream.

    python3 mac_cast.py --list                 # show capture devices
    python3 mac_cast.py --input :0             # capture a device, serve it
    python3 mac_cast.py --input song.mp3       # serve a file, paced live

Then on the puck:  tune http://<mac-ip>:8100/live      (or paste it in the web UI)

Needs NO firmware change: the puck already plays HTTP MP3, so the Mac just has
to look like a radio station. To capture *system* audio (everything the Mac
plays, not just a mic) install a loopback driver — BlackHole is free:
`brew install blackhole-2ch` — then create a Multi-Output Device in Audio MIDI
Setup containing both your speakers and BlackHole, select it as system output,
and point --input at the BlackHole device.

Why this exists rather than `ffmpeg -listen 1`: that serves exactly one
connection and then exits, so the first reconnect — and the puck reconnects
after any hiccup — gets a refused socket. Here each request spawns its own
encoder, so reconnects just work.

Latency is a few seconds: the puck prebuffers ~48 KB before it starts. Fine for
music, wrong for anything that has to stay in sync with video.
"""

import argparse
import os
import shutil
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FFMPEG = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
ARGS = None


def list_devices():
    p = subprocess.run([FFMPEG, "-hide_banner", "-f", "avfoundation",
                        "-list_devices", "true", "-i", ""],
                       capture_output=True, text=True)
    out = p.stderr
    show = False
    for line in out.splitlines():
        if "AVFoundation audio devices" in line:
            show = True
        elif "AVFoundation video devices" in line:
            show = False
        elif show and "]" in line:
            print("  " + line.split("] ", 1)[-1] if "] " in line else "  " + line)
    print("\nUse the bracketed index as --input :N  (e.g. --input :0)")


def encoder_cmd(inp, bitrate):
    live = inp.startswith(":")
    cmd = [FFMPEG, "-hide_banner", "-loglevel", "error"]
    if live:
        cmd += ["-f", "avfoundation", "-i", inp]
    else:
        cmd += ["-re", "-stream_loop", "-1", "-i", inp]
    cmd += ["-c:a", "libmp3lame", "-b:a", f"{bitrate}k", "-ar", "44100",
            "-ac", "2", "-f", "mp3", "-flush_packets", "1", "-"]
    return cmd


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"       # no chunking; the puck wants a raw body

    def log_message(self, *a):
        pass

    def do_GET(self):
        print(f"  client {self.client_address[0]} connected -> starting encoder",
              flush=True)
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("icy-name", "Mac Cast")
        self.send_header("Connection", "close")
        self.end_headers()
        proc = subprocess.Popen(encoder_cmd(ARGS.input, ARGS.bitrate),
                                stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL)
        try:
            while True:
                chunk = proc.stdout.read(4096)
                if not chunk:
                    break
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            print("  client disconnected", flush=True)
        finally:
            proc.kill()
            proc.wait()


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--input", default=":0")
    ap.add_argument("--port", type=int, default=8100)
    ap.add_argument("--bitrate", type=int, default=128)
    ARGS = ap.parse_args()

    if not os.path.exists(FFMPEG):
        sys.exit("ffmpeg not found")
    if ARGS.list:
        return list_devices()

    srv = ThreadingHTTPServer(("0.0.0.0", ARGS.port), Handler)
    print(f"serving {ARGS.input} at http://<this-mac>:{ARGS.port}/live "
          f"({ARGS.bitrate} kbps) — ctrl-C to stop", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
