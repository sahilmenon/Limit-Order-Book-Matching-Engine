#!/usr/bin/env python3
"""Independent reference reconstruction of a NASDAQ ITCH 5.0 order book.

This is deliberately a separate, straightforward implementation (different
language, no shared code with the C++ engine) so that agreement between the two
depth snapshots is real evidence of correctness, not a shared bug. It mirrors
the C++ validator's framing, symbol selection and event logic exactly.

    python reference_reconstruct.py <itch_file.bin> [TICKER] [--snapshot out.csv]
"""
import argparse
import struct
import sys
from collections import defaultdict


def frames(data: bytes):
    """Yield message payloads from BinaryFILE framing; stop on a partial tail."""
    pos, n = 0, len(data)
    while pos + 2 <= n:
        length = (data[pos] << 8) | data[pos + 1]
        if length == 0 or pos + 2 + length > n:
            break  # truncated final frame (range-limited download)
        yield data[pos + 2 : pos + 2 + length]
        pos += 2 + length


def be(payload: bytes, off: int, width: int) -> int:
    return int.from_bytes(payload[off : off + width], "big")


def select_symbol(data: bytes, ticker: str | None):
    """Return (target_locate, ticker). Auto-selects the most active symbol,
    tie-broken by smallest locate — identical rule to the C++ validator."""
    locate_to_ticker: dict[int, str] = {}
    adds = defaultdict(int)
    for p in frames(data):
        t = chr(p[0])
        if t == "R" and len(p) >= 19:
            locate_to_ticker[be(p, 1, 2)] = p[11:19].rstrip(b" ").decode("ascii", "replace")
        elif t in ("A", "F") and len(p) >= 36:
            adds[be(p, 1, 2)] += 1

    if ticker:
        for loc, sym in locate_to_ticker.items():
            if sym == ticker:
                return loc, ticker
        sys.exit(f"error: ticker {ticker} not found")

    best_loc, best_n = 0, -1
    for loc, n in adds.items():
        if n > best_n or (n == best_n and (best_loc == 0 or loc < best_loc)):
            best_loc, best_n = loc, n
    return best_loc, locate_to_ticker.get(best_loc, "?")


def reconstruct(data: bytes, target_locate: int):
    """Rebuild the displayed book. orders: ref -> [side, price, remaining]."""
    orders: dict[int, list] = {}
    stats = defaultdict(int)

    for p in frames(data):
        t = chr(p[0])
        stats["total"] += 1

        if t in ("A", "F"):
            stats["adds"] += 1
            if be(p, 1, 2) != target_locate:
                continue
            ref = be(p, 11, 8)
            side = "B" if p[19:20] == b"B" else "S"
            shares = be(p, 20, 4)
            price = be(p, 32, 4)
            orders[ref] = [side, price, shares]

        elif t in ("E", "C", "X"):  # execute / execute-with-price / cancel: reduce
            stats["reduce"] += 1
            ref = be(p, 11, 8)
            shares = be(p, 19, 4)
            o = orders.get(ref)
            if o is not None:
                o[2] -= shares
                if o[2] <= 0:
                    del orders[ref]

        elif t == "D":  # delete
            stats["deletes"] += 1
            orders.pop(be(p, 11, 8), None)

        elif t == "U":  # replace: delete original, add replacement
            stats["replaces"] += 1
            orig = be(p, 11, 8)
            o = orders.pop(orig, None)
            if o is not None:
                repl = be(p, 19, 8)
                shares = be(p, 27, 4)
                price = be(p, 31, 4)
                orders[repl] = [o[0], price, shares]

    # Aggregate remaining quantity per (side, price).
    bids: dict[int, int] = defaultdict(int)
    asks: dict[int, int] = defaultdict(int)
    for side, price, qty in orders.values():
        (bids if side == "B" else asks)[price] += qty
    return bids, asks, stats


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("itch_file")
    ap.add_argument("ticker", nargs="?", default=None)
    ap.add_argument("--snapshot")
    args = ap.parse_args()

    with open(args.itch_file, "rb") as fh:
        data = fh.read()
    print(f"Loaded {args.itch_file} ({len(data) / 1048576:.1f} MB)")

    target_locate, ticker = select_symbol(data, args.ticker)
    print(f"Target symbol: {ticker} (stock_locate={target_locate})")

    bids, asks, stats = reconstruct(data, target_locate)
    print(
        f"Messages: {stats['total']} total | adds {stats['adds']}, "
        f"reduce {stats['reduce']}, delete {stats['deletes']}, replace {stats['replaces']}"
    )
    print(f"Resting price levels: {len(bids)} bid / {len(asks)} ask")
    if bids and asks:
        bb, ba = max(bids), min(asks)
        print(f"Top of book: bid {bb} x{bids[bb]} | ask {ba} x{asks[ba]} | spread {ba - bb}")

    if args.snapshot:
        with open(args.snapshot, "w", newline="\n") as out:
            for price in sorted(bids, reverse=True):
                out.write(f"B,{price},{bids[price]}\n")
            for price in sorted(asks):
                out.write(f"S,{price},{asks[price]}\n")
        print(f"Wrote snapshot: {args.snapshot}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
