#!/usr/bin/env python3
"""Cross-validate the C++ engine against the Python reference on a real feed.

Runs both reconstructions over the same ITCH sample and diffs their depth
snapshots line for line. The two implementations share no code and are written
in different languages, so a byte-identical snapshot is strong evidence the C++
matching-engine maintenance API rebuilds a real market's book correctly.

    python validate_itch.py --bin data/itch_sample.bin \
        --cpp build/itch_validate[.exe] [--ticker AAPL]

Exit status is non-zero if the snapshots differ, so this doubles as a CI gate.
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REFERENCE = HERE / "reference_reconstruct.py"


def run(cmd: list[str]) -> None:
    print("$", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def load_snapshot(path: Path) -> list[str]:
    return [ln.rstrip("\n") for ln in path.read_text().splitlines() if ln.strip()]


def find_cpp_binary(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.exists():
            return p
        sys.exit(f"error: --cpp binary not found: {p}")
    for cand in ("build/itch_validate.exe", "build/itch_validate",
                 "build/apps/itch_validate.exe", "build/apps/itch_validate"):
        p = HERE.parent / cand
        if p.exists():
            return p
    sys.exit("error: could not locate itch_validate; pass --cpp <path>")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", type=Path, default=Path("data/itch_sample.bin"))
    ap.add_argument("--cpp", default=None, help="path to the itch_validate binary")
    ap.add_argument("--ticker", default=None,
                    help="symbol to reconstruct (default: most active)")
    args = ap.parse_args()

    if not args.bin.exists():
        sys.exit(f"error: {args.bin} not found — run scripts/fetch_itch.py first")

    cpp = find_cpp_binary(args.cpp)
    cpp_csv = args.bin.with_suffix(".cpp.csv")
    py_csv = args.bin.with_suffix(".py.csv")

    cpp_cmd = [str(cpp), str(args.bin)]
    py_cmd = [sys.executable, str(REFERENCE), str(args.bin)]
    if args.ticker:
        cpp_cmd.append(args.ticker)
        py_cmd.append(args.ticker)
    cpp_cmd += ["--snapshot", str(cpp_csv)]
    py_cmd += ["--snapshot", str(py_csv)]

    print("=== C++ reconstruction ===")
    run(cpp_cmd)
    print("\n=== Python reference reconstruction ===")
    run(py_cmd)

    cpp_lines = load_snapshot(cpp_csv)
    py_lines = load_snapshot(py_csv)

    print("\n=== Diff ===")
    if cpp_lines == py_lines:
        print(f"MATCH: {len(cpp_lines)} price levels identical in both snapshots.")
        return 0

    print(f"MISMATCH: C++ has {len(cpp_lines)} levels, Python {len(py_lines)}.")
    cpp_set, py_set = set(cpp_lines), set(py_lines)
    only_cpp = [l for l in cpp_lines if l not in py_set]
    only_py = [l for l in py_lines if l not in cpp_set]
    for l in only_cpp[:20]:
        print(f"  only C++: {l}")
    for l in only_py[:20]:
        print(f"  only PY : {l}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
