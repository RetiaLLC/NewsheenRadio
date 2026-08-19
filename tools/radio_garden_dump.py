#!/usr/bin/env python3
"""Dump the radio.garden catalogue: every place, every station, every stream URL.

Pure standard library, resumable, threaded. Writes to ./radio_garden_out/.

    python3 radio_garden_dump.py --probe        # 10s: is the API reachable at all?
    python3 radio_garden_dump.py                # places + channels (fast)
    python3 radio_garden_dump.py --resolve      # ALSO resolve real upstream stream URLs

Run --probe first. radio.garden sits behind Cloudflare and returns HTTP 403 to
non-browser clients from some networks; if the probe fails there is nothing this
script can do about it and a full walk would just burn time producing 403s.

Only `resolved_stream_url` (from --resolve) is useful to an embedded player:
the radio.garden redirect URL goes back through the same gated API, so a device
gets 403 on it. The resolved upstream URLs are ordinary Icecast/Shoutcast
endpoints and play fine.
"""

import argparse
import csv
import json
import os
import queue
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request

BASE = "https://radio.garden/api/ara/content"
OUT = "radio_garden_out"
UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/125.0 Safari/537.36")
CTX = ssl.create_default_context()
_print_lock = threading.Lock()


def log(*a):
    with _print_lock:
        print(*a, file=sys.stderr, flush=True)


def fetch(path, timeout=25):
    req = urllib.request.Request(BASE + path, headers={
        "User-Agent": UA,
        "Accept": "application/json,text/plain,*/*",
        "Referer": "https://radio.garden/",
    })
    with urllib.request.urlopen(req, timeout=timeout, context=CTX) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def resolve_stream(channel_id, timeout=15):
    """Return the URL the channel's 302 points at, or '' if it can't be had."""
    url = f"{BASE}/listen/{channel_id}/channel.mp3"
    req = urllib.request.Request(url, headers={"User-Agent": UA,
                                               "Referer": "https://radio.garden/"})
    opener = urllib.request.build_opener(NoRedirect)
    try:
        with opener.open(req, timeout=timeout) as r:
            return r.headers.get("Location", "") or ""
    except urllib.error.HTTPError as e:
        if e.code in (301, 302, 303, 307, 308):
            return e.headers.get("Location", "") or ""
        return ""
    except Exception:
        return ""


def probe():
    try:
        d = fetch("/places", timeout=20)
        n = len(d.get("data", {}).get("list", []))
        print(f"OK — /places answered with {n} places. Safe to run the full dump.")
        return 0
    except urllib.error.HTTPError as e:
        print(f"BLOCKED — /places returned HTTP {e.code}.")
        print("radio.garden is refusing non-browser clients from this network.")
        print("Nothing to do from here; the catalogue cannot be fetched this way.")
        return 1
    except Exception as e:
        print(f"FAILED — {e}")
        return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", action="store_true", help="check reachability and exit")
    ap.add_argument("--resolve", action="store_true", help="resolve upstream stream URLs")
    ap.add_argument("--workers", type=int, default=12)
    ap.add_argument("--sleep", type=float, default=0.0, help="pause between requests")
    args = ap.parse_args()

    if args.probe:
        return probe()

    os.makedirs(OUT, exist_ok=True)

    # ---- places ------------------------------------------------------------
    places_path = os.path.join(OUT, "places.json")
    if os.path.exists(places_path):
        places = json.load(open(places_path))
        log(f"places: {len(places)} (cached)")
    else:
        d = fetch("/places")
        places = d.get("data", {}).get("list", [])
        json.dump(places, open(places_path, "w"))
        log(f"places: {len(places)}")

    # ---- channels, resumable ----------------------------------------------
    chan_path = os.path.join(OUT, "channels.jsonl")
    done = set()
    if os.path.exists(chan_path):
        for line in open(chan_path):
            try:
                done.add(json.loads(line)["place_id"])
            except Exception:
                pass
        log(f"resuming: {len(done)} places already walked")

    todo = [p for p in places if p.get("id") not in done]
    q = queue.Queue()
    for p in todo:
        q.put(p)
    out_lock = threading.Lock()
    chan_f = open(chan_path, "a")
    counter = [0]

    def worker():
        while True:
            try:
                p = q.get_nowait()
            except queue.Empty:
                return
            pid = p.get("id")
            try:
                d = fetch(f"/page/{pid}/channels")
                for block in d.get("data", {}).get("content", []):
                    for item in block.get("items", []) or []:
                        pg = item.get("page") or {}
                        url = pg.get("url") or ""
                        cid = url.rsplit("/", 1)[-1]
                        if not cid:
                            continue
                        row = {
                            "channel_id": cid,
                            "title": pg.get("title", ""),
                            "place": p.get("title", ""),
                            "country": p.get("country", ""),
                            "lat": (p.get("geo") or [None, None])[1],
                            "lon": (p.get("geo") or [None, None])[0],
                            "place_id": pid,
                        }
                        with out_lock:
                            chan_f.write(json.dumps(row) + "\n")
            except Exception as e:
                log(f"  place {pid}: {e}")
            finally:
                with out_lock:
                    counter[0] += 1
                    if counter[0] % 200 == 0:
                        chan_f.flush()
                        log(f"  {counter[0]}/{len(todo)} places")
                if args.sleep:
                    time.sleep(args.sleep)
                q.task_done()

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(args.workers)]
    [t.start() for t in threads]
    [t.join() for t in threads]
    chan_f.close()

    rows = {}
    for line in open(chan_path):
        try:
            r = json.loads(line)
            rows[r["channel_id"]] = r
        except Exception:
            pass
    log(f"channels: {len(rows)}")

    # ---- resolve upstream URLs (the only ones a device can play) ------------
    if args.resolve:
        res_path = os.path.join(OUT, "resolved.jsonl")
        resolved = {}
        if os.path.exists(res_path):
            for line in open(res_path):
                try:
                    r = json.loads(line)
                    resolved[r["channel_id"]] = r["url"]
                except Exception:
                    pass
            log(f"resuming: {len(resolved)} already resolved")

        pending = [c for c in rows if c not in resolved]
        rq = queue.Queue()
        for c in pending:
            rq.put(c)
        res_f = open(res_path, "a")
        rc = [0]

        def rworker():
            while True:
                try:
                    cid = rq.get_nowait()
                except queue.Empty:
                    return
                u = resolve_stream(cid)
                with out_lock:
                    res_f.write(json.dumps({"channel_id": cid, "url": u}) + "\n")
                    resolved[cid] = u
                    rc[0] += 1
                    if rc[0] % 250 == 0:
                        res_f.flush()
                        log(f"  resolved {rc[0]}/{len(pending)}")
                if args.sleep:
                    time.sleep(args.sleep)
                rq.task_done()

        threads = [threading.Thread(target=rworker, daemon=True) for _ in range(args.workers)]
        [t.start() for t in threads]
        [t.join() for t in threads]
        res_f.close()
        for cid, u in resolved.items():
            if cid in rows:
                rows[cid]["resolved_stream_url"] = u

    # ---- outputs -----------------------------------------------------------
    cols = ["channel_id", "title", "place", "country", "lat", "lon",
            "radio_garden_page", "radio_garden_stream_url", "resolved_stream_url"]
    with open(os.path.join(OUT, "channels.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows.values():
            r["radio_garden_page"] = f"https://radio.garden/listen/x/{r['channel_id']}"
            r["radio_garden_stream_url"] = f"{BASE}/listen/{r['channel_id']}/channel.mp3"
            r.setdefault("resolved_stream_url", "")
            w.writerow(r)
    json.dump(list(rows.values()), open(os.path.join(OUT, "channels.json"), "w"))

    with open(os.path.join(OUT, "streams.m3u"), "w") as f:
        f.write("#EXTM3U\n")
        for r in rows.values():
            url = r.get("resolved_stream_url") or r["radio_garden_stream_url"]
            f.write(f"#EXTINF:-1,{r['title']} ({r['place']})\n{url}\n")

    n_res = sum(1 for r in rows.values() if r.get("resolved_stream_url"))
    print(f"\nDone. {len(rows)} stations -> {OUT}/channels.csv")
    if args.resolve:
        print(f"      {n_res} with a resolved upstream stream URL")
    return 0


if __name__ == "__main__":
    sys.exit(main())
