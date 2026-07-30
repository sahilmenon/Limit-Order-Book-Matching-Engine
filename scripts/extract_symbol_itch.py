#!/usr/bin/env python3
"""Carve a small, self-contained ITCH 5.0 substream for one symbol.

The browser demo can replay recorded NASDAQ data, but a full session is 3-5 GB.
This walks a (already downloaded) ITCH file once and keeps only the messages that
touch a single symbol: its Stock Directory (R), every Add (A/F) for its locate,
and every Execute/Cancel/Delete/Replace (E/C/X/D/U) that references an order we
kept. The result is a few hundred KB of valid, length-framed ITCH that rebuilds
that symbol's book on its own, small enough to commit and ship to the browser.

    python scripts/extract_symbol_itch.py --in data/itch_sample.bin \
        --out web/public/sample.itch.bin --max-bytes 500000

Symbol selection tries a list of liquid, recognisable tickers first, then falls
back to the most active symbol in the slice.
"""
import argparse
import struct
import sys
from pathlib import Path

PREFERRED = ["AAPL", "MSFT", "AMZN", "INTC", "CSCO", "QQQ", "SPY", "TSLA"]


def frames(data: bytes):
    """Yield (payload, whole_frame_bytes) from BinaryFILE framing."""
    pos, n = 0, len(data)
    while pos + 2 <= n:
        length = (data[pos] << 8) | data[pos + 1]
        if length == 0 or pos + 2 + length > n:
            break
        yield data[pos + 2 : pos + 2 + length], data[pos : pos + 2 + length]
        pos += 2 + length


def be(p: bytes, off: int, width: int) -> int:
    return int.from_bytes(p[off : off + width], "big")


def choose_symbol(data: bytes, want: str | None):
    """Return (locate, ticker). Honour --ticker, else prefer a known name, else
    the symbol with the most Add messages."""
    loc_to_sym: dict[int, str] = {}
    adds: dict[int, int] = {}
    for p, _ in frames(data):
        t = chr(p[0])
        if t == "R" and len(p) >= 19:
            loc_to_sym[be(p, 1, 2)] = p[11:19].rstrip(b" ").decode("ascii", "replace")
        elif t in ("A", "F") and len(p) >= 36:
            adds[be(p, 1, 2)] = adds.get(be(p, 1, 2), 0) + 1
    sym_to_loc = {s: l for l, s in loc_to_sym.items()}

    if want:
        if want not in sym_to_loc:
            sys.exit(f"error: {want} not found in slice")
        return sym_to_loc[want], want
    for cand in PREFERRED:
        if cand in sym_to_loc and adds.get(sym_to_loc[cand], 0) > 500:
            return sym_to_loc[cand], cand
    best = max(adds, key=adds.get)
    return best, loc_to_sym.get(best, "?")


def extract(data: bytes, locate: int, max_bytes: int) -> bytes:
    """Forward pass: emit the target's R + A/F and any E/C/X/D/U referencing a
    kept order. Tracks replacement refs so U chains stay self-contained."""
    kept_refs: set[int] = set()
    out = bytearray()
    for p, frame in frames(data):
        t = chr(p[0])
        emit = False
        if t == "R" and len(p) >= 19 and be(p, 1, 2) == locate:
            emit = True
        elif t in ("A", "F") and len(p) >= 36 and be(p, 1, 2) == locate:
            kept_refs.add(be(p, 11, 8))
            emit = True
        elif t in ("E", "C", "X", "D") and be(p, 11, 8) in kept_refs:
            emit = True
        elif t == "U" and be(p, 11, 8) in kept_refs:
            kept_refs.add(be(p, 19, 8))  # replacement ref stays in the set
            emit = True
        if emit:
            out += frame
            if len(out) >= max_bytes:
                break
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="inp", type=Path, default=Path("data/itch_sample.bin"))
    ap.add_argument("--out", type=Path, default=Path("web/public/sample.itch.bin"))
    ap.add_argument("--ticker", default=None)
    ap.add_argument("--max-bytes", type=int, default=500_000)
    args = ap.parse_args()

    if not args.inp.exists():
        sys.exit(f"error: {args.inp} not found — run scripts/fetch_itch.py first")

    data = args.inp.read_bytes()
    locate, ticker = choose_symbol(data, args.ticker)
    print(f"Symbol: {ticker} (stock_locate={locate})")

    out = extract(data, locate, args.max_bytes)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(out)
    print(f"Wrote {args.out} ({len(out) / 1024:.0f} KB, {ticker} replay stream)")

    # A tiny sidecar so the web app can label the replay without re-scanning.
    meta = args.out.with_suffix(".json")
    meta.write_text(f'{{"ticker":"{ticker}","locate":{locate},"bytes":{len(out)}}}\n')
    print(f"Wrote {meta}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
