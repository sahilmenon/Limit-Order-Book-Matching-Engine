#!/usr/bin/env python3
"""Fetch a prefix of a real NASDAQ TotalView-ITCH 5.0 file for validation.

The published daily files are 3-5 GB gzipped (tens of GB raw) — far more than a
correctness check needs. This downloads only the first N megabytes via an HTTP
Range request, then streams them through gunzip, stopping cleanly when the
compressed prefix runs out. The result is a truncated-but-valid ITCH byte stream
whose final framed message may be partial; both reconstructors detect that tail
and stop, so a prefix is a perfectly good test input.

    python fetch_itch.py --date 12302019 --mb 32 --out data/itch_sample.bin

Files live under data/ (git-ignored). Run scripts/validate_itch.py afterwards to
diff the C++ and Python reconstructions.
"""
import argparse
import sys
import zlib
from pathlib import Path
from urllib.request import Request, urlopen

BASE = "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/"


def fetch_prefix(date: str, mb: int, out: Path) -> None:
    url = f"{BASE}{date}.NASDAQ_ITCH50.gz"
    nbytes = mb * 1024 * 1024
    req = Request(url, headers={"User-Agent": "lob-itch-validator/1.0",
                                "Range": f"bytes=0-{nbytes - 1}"})
    print(f"Downloading first {mb} MB of {date}.NASDAQ_ITCH50.gz ...", flush=True)

    out.parent.mkdir(parents=True, exist_ok=True)
    # gzip has a 2-byte magic + header; wbits=31 tells zlib to expect that header.
    dec = zlib.decompressobj(wbits=31)
    raw_total = 0
    with urlopen(req, timeout=120) as resp, open(out, "wb") as fh:
        if resp.status not in (200, 206):
            sys.exit(f"error: unexpected HTTP status {resp.status}")
        while True:
            chunk = resp.read(1 << 20)
            if not chunk:
                break
            try:
                raw = dec.decompress(chunk)
            except zlib.error as e:
                # Truncated deflate stream at our cut point — keep what we have.
                print(f"  (stream cut mid-block: {e}); using decoded prefix", flush=True)
                break
            fh.write(raw)
            raw_total += len(raw)

    print(f"Wrote {out} ({raw_total / 1048576:.1f} MB raw ITCH)", flush=True)
    if raw_total == 0:
        sys.exit("error: decoded 0 bytes — download or gunzip failed")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--date", default="12302019",
                    help="ITCH file date stamp MMDDYYYY (default: 12302019)")
    ap.add_argument("--mb", type=int, default=32,
                    help="compressed megabytes to download (default: 32)")
    ap.add_argument("--out", type=Path, default=Path("data/itch_sample.bin"))
    args = ap.parse_args()
    fetch_prefix(args.date, args.mb, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
