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
| Milestone 1 | `OrderBook` (naive) | `std::map<Price, std::list<Order>>`: a red-black tree of price levels, each a heap-linked FIFO list |
| Milestone 3 | `FastOrderBook` (fast) | flat price-ladder array indexed by tick + an intrusive order pool (integer-linked FIFO, free-list allocation, 64-byte-aligned levels) |

`FastOrderBook` is proven byte-for-byte identical to the naive `OrderBook` by a
20,000-operation randomised differential test, so the speedup carries no
correctness cost. The naive book remains the oracle.

## Test machine

| | |
|---|---|
| CPU | Intel Core i7-1065G7 (Ice Lake, 4 cores / 8 threads, 1.30 GHz base) |
| Memory | 16 GB |
| Compiler | g++ 15.2.0, `-O3 -march=native` |
| OS | Windows 11 |

Absolute latency is hardware-bound, so these numbers mean little without the
machine attached. This is a 2019 ultrabook under Windows: I pin no cores,
isolate nothing, and leave turbo alone. A colocated server would post better
absolute numbers, though the gap between the two books should hold.

## Result

Best of 8 runs (2,000,000 ops, 1024-tick band). Taking the best of several runs
strips OS-scheduler preemption out of a micro-benchmark, so what remains is the
structural cost of the data structure. Absolute numbers vary by machine; the
per-percentile ordering does not.

```
  naive (map)     3.22 M ops/s   p50  538.2 ns   p99 2796.5 ns   p99.9 31396.9 ns
  fast (ladder)   4.02 M ops/s   p50  343.9 ns   p99 2317.7 ns   p99.9 20821.3 ns
```

The fast book wins every percentile. Median per-order latency drops from 538 ns
to 344 ns (~1.5×). Throughput improves too, from 1.2× on a quiet machine to 2.7×
under load, where the map's extra cache misses compound.

Regenerate the README chart from these numbers with
`python scripts/plot_benchmark.py`.

### Where the win comes from

- **Level lookup is an array index, not a tree descent.** The naive book pays an
  `O(log L)` red-black-tree walk, chasing pointers across cache lines, to find a
  price level. The ladder is a single offset into a contiguous array.
- **No per-order heap node.** Orders are dense slots in one pooled vector, linked
  by integer indices. Allocation is a free-list pop, and walking a level's FIFO
  queue stays in cache instead of chasing `std::list` nodes scattered across the
  heap.
- **Best bid and ask are cached ticks,** repaired by a short local scan only when
  the top level empties, so matching never recomputes a tree `begin()`.

Both books pay the same tail cost when the id→order hash map resizes and when an
aggressive order sweeps deep, so `p99.9` stays close; the ladder still leads.
