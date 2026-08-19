#!/usr/bin/env python3
"""Turn the radio.garden dump into an on-device catalogue.

    python3 build_catalogue.py [--max-per-place 6] [--out ../data]

Reads radio_garden_out/{channels,resolved}.jsonl and writes:

  stations.tsv   name \t place \t country \t lat \t lon \t url      (one per line)
  countries.idx  country \t byte_offset \t count                     (seek index)

Why TSV and not JSON: the device scans this file directly off LittleFS, and a
line-oriented format lets it stream a match out without parsing the whole thing
or holding it in RAM. The country index turns "browse Brazil" into a seek plus a
short read instead of a 3 MB scan.

Filtering matches what the firmware can actually play:
  - must have resolved to a real upstream URL (the radio.garden redirect URL is
    useless to the device — it goes back through an API that 403s without a
    browser Referer)
  - http/https only
  - no .m3u8 (HLS is not supported)
  - no .ogg/.opus/.flac/.wav suffix (no Vorbis decoder; the others are refused)
"""

import argparse
import json
import os
import re
import sys
import collections

IN = "radio_garden_out"
BAD_SUFFIX = re.compile(r"\.(m3u8|ogg|opus|flac|wav)(\?|$)", re.I)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-per-place", type=int, default=0,
                    help="cap stations per place (0 = keep all)")
    ap.add_argument("--out", default="../data")
    args = ap.parse_args()

    chan_p = os.path.join(IN, "channels.jsonl")
    res_p = os.path.join(IN, "resolved.jsonl")
    if not os.path.exists(chan_p):
        sys.exit(f"missing {chan_p} — run radio_garden_dump.py first")

    resolved = {}
    if os.path.exists(res_p):
        for line in open(res_p):
            try:
                r = json.loads(line)
                if r.get("url"):
                    resolved[r["channel_id"]] = r["url"]
            except Exception:
                pass

    rows, seen = {}, set()
    for line in open(chan_p):
        try:
            r = json.loads(line)
        except Exception:
            continue
        rows[r["channel_id"]] = r

    stats = collections.Counter()
    out = []
    per_place = collections.Counter()
    for cid, r in rows.items():
        stats["total"] += 1
        url = resolved.get(cid, "")
        if not url:
            stats["unresolved"] += 1
            continue
        if not url.startswith(("http://", "https://")):
            stats["bad_scheme"] += 1
            continue
        if ".m3u8" in url.lower():
            stats["hls"] += 1
            continue
        if BAD_SUFFIX.search(url):
            stats["undecodable"] += 1
            continue
        key = url.split("?")[0]
        if key in seen:
            stats["dup_url"] += 1
            continue
        seen.add(key)
        place = (r.get("place") or "").strip()
        if args.max_per_place:
            if per_place[place] >= args.max_per_place:
                stats["place_capped"] += 1
                continue
            per_place[place] += 1
        name = (r.get("title") or "").replace("\t", " ").strip()
        if not name:
            continue
        # Strip control characters and bound the row: the device parses this with
        # a fixed line buffer, and an over-long row truncates into invalid JSON.
        name = re.sub(r"[\x00-\x1f\x7f]", " ", name)[:90].strip()
        place = re.sub(r"[\x00-\x1f\x7f]", " ", place)[:60].strip()
        if len(name) + len(place) + len(url) > 380:
            stats["overlong"] += 1
            continue
        out.append((
            name,
            place.replace("\t", " "),
            (r.get("country") or "").replace("\t", " ").strip(),
            r.get("lat"), r.get("lon"), url,
        ))
        stats["kept"] += 1

    # country then place then name: makes the index contiguous and browsing sane
    out.sort(key=lambda t: (t[2].lower(), t[1].lower(), t[0].lower()))

    os.makedirs(args.out, exist_ok=True)
    tsv_p = os.path.join(args.out, "stations.tsv")
    idx_p = os.path.join(args.out, "countries.idx")
    offsets = {}
    with open(tsv_p, "w", encoding="utf-8") as f:
        for name, place, country, lat, lon, url in out:
            if country not in offsets:
                offsets[country] = [f.tell(), 0]
            offsets[country][1] += 1
            la = f"{lat:.3f}" if isinstance(lat, (int, float)) else ""
            lo = f"{lon:.3f}" if isinstance(lon, (int, float)) else ""
            f.write(f"{name}\t{place}\t{country}\t{la}\t{lo}\t{url}\n")
    with open(idx_p, "w", encoding="utf-8") as f:
        for c, (off, n) in sorted(offsets.items(), key=lambda kv: kv[0].lower()):
            f.write(f"{c}\t{off}\t{n}\n")

    size = os.path.getsize(tsv_p)
    print(f"kept {stats['kept']} of {stats['total']} stations "
          f"({100*stats['kept']/max(1,stats['total']):.1f}%)")
    for k in ("unresolved", "hls", "undecodable", "bad_scheme", "dup_url", "place_capped", "overlong"):
        if stats[k]:
            print(f"  dropped {k:14} {stats[k]}")
    print(f"countries: {len(offsets)}")
    print(f"{tsv_p}  {size/1048576:.2f} MB")
    print(f"{idx_p}  {os.path.getsize(idx_p)} B")
    if size > 11 * 1048576:
        print("WARNING: larger than the LittleFS partition; use --max-per-place")


if __name__ == "__main__":
    main()
