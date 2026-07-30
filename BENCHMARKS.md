# Benchmarks

Reproducible throughput and latency comparison of the two book implementations.
Both replay the **same** pre-generated workload, so the data structure is the
only variable.

## Running

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bench [num_ops] [num_ticks]      # defaults: 2,000,000 ops, 1024 ticks
```

The harness is built with `-O3 -march=native`. Latency is timed per operation
with the CPU timestamp counter (`rdtsc`), calibrated to nanoseconds against
`steady_clock`; percentiles are computed exactly (all samples sorted, not
bucketed). Throughput is whole-workload wall-clock.

## Workload

A synthetic but realistic mix over a bounded price band:

| operation      | share | notes                                   |
|----------------|-------|-----------------------------------------|
| add limit      | 65%   | random side/price; most rest, some cross|
| market order   | 10%   | always marketable, generates trades     |
| cancel         | 15%   | targets a live resting order            |
| modify         | 10%   | reprice/resize a live resting order     |

## What's being compared

| build | book | structure |
|-------|------|-----------|
| Milestone 1 | `OrderBook` (naive) | `std::map<Price, std::list<Order>>` — a red-black tree of price levels, each a heap-linked FIFO list |
| Milestone 3 | `FastOrderBook` (fast) | flat price-ladder array indexed by tick + an intrusive order pool (integer-linked FIFO, free-list allocation, 64-byte-aligned levels) |

`FastOrderBook` is proven byte-for-byte identical to the naive `OrderBook` by a
20,000-operation randomised differential test, so the speedup is free of
correctness cost — the naive book remains the oracle.

## Result

Representative run (2,000,000 ops, 1024-tick band; absolute numbers vary by
machine, the **ratio** is the point):

```
  naive (map)     0.72 M ops/s   p50  716.5 ns   p99 3866.2 ns   p99.9 43430.1 ns
  fast (ladder)   1.93 M ops/s   p50  478.8 ns   p99 3297.3 ns   p99.9 28817.4 ns

  speedup: 2.67x throughput
```

### Where the win comes from

- **Level lookup is an array index, not a tree descent.** The naive book pays an
  `O(log L)` red-black-tree walk (and pointer chasing across cache lines) to find
  a price level; the ladder is a single offset into a contiguous array.
- **No per-order heap node.** Orders are dense slots in one pooled vector, linked
  by integer indices. Allocation is a free-list pop, and walking a level's FIFO
  stays in cache instead of chasing `std::list` nodes scattered across the heap.
- **Best bid/ask are cached ticks,** repaired by a short local scan only when the
  top level empties — no tree `begin()` recomputation per match.

The tail (`p99.9`) is dominated by the id→order hash map resizing and the
occasional deep sweep; both books pay it, and the ladder still comes out ahead.
