// Throughput + latency benchmark comparing the naive OrderBook (std::map +
// std::list) against the cache-friendly FastOrderBook (flat price ladder +
// intrusive order pool). Both replay the *same* pre-generated workload, so the
// only variable is the data structure.
//
//   Usage: bench [num_ops] [num_ticks]
//
// Reports operations/second and per-operation latency percentiles (p50/p99/
// p99.9), timed with the CPU timestamp counter and calibrated to nanoseconds.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "lob/bench/histogram.hpp"
#include "lob/fast_order_book.hpp"
#include "lob/order_book.hpp"

namespace {

using namespace lob;

enum class Kind : std::uint8_t { AddLimit, AddMarket, Cancel, Modify };

struct Op {
    Kind kind;
    OrderId id;
    Side side;
    Price price;
    Quantity qty;
};

// Build a realistic-ish mix: mostly resting limit adds, a steady stream of
// cancels and modifies against live orders, and occasional marketable orders
// that actually cross and generate trades. Prices sit in [floor, floor+ticks).
std::vector<Op> make_workload(std::size_t n, Price floor, std::size_t ticks) {
    std::vector<Op> ops;
    ops.reserve(n);
    std::mt19937_64 rng(0xABCDEF01u);
    std::uniform_int_distribution<int> pick(0, 99);
    std::uniform_int_distribution<int> pxd(static_cast<int>(floor),
                                           static_cast<int>(floor) + static_cast<int>(ticks) - 1);
    std::uniform_int_distribution<int> qtyd(1, 100);

    std::vector<OrderId> live;
    live.reserve(n);
    OrderId next_id = 1;

    for (std::size_t i = 0; i < n; ++i) {
        const int r = pick(rng);
        if (r < 65) {  // 65% resting limit adds
            const OrderId id = next_id++;
            ops.push_back({Kind::AddLimit, id,
                           (rng() & 1) ? Side::Buy : Side::Sell,
                           pxd(rng), static_cast<Quantity>(qtyd(rng))});
            live.push_back(id);
        } else if (r < 75) {  // 10% marketable orders
            ops.push_back({Kind::AddMarket, next_id++,
                           (rng() & 1) ? Side::Buy : Side::Sell, 0,
                           static_cast<Quantity>(qtyd(rng))});
        } else if (r < 90 && !live.empty()) {  // 15% cancels
            const std::size_t k = rng() % live.size();
            ops.push_back({Kind::Cancel, live[k], Side::Buy, 0, 0});
            live[k] = live.back();
            live.pop_back();
        } else if (!live.empty()) {  // 10% modifies
            const std::size_t k = rng() % live.size();
            ops.push_back({Kind::Modify, live[k], Side::Buy,
                           pxd(rng), static_cast<Quantity>(qtyd(rng))});
        } else {
            const OrderId id = next_id++;
            ops.push_back({Kind::AddLimit, id, Side::Buy, pxd(rng),
                           static_cast<Quantity>(qtyd(rng))});
            live.push_back(id);
        }
    }
    return ops;
}

// Apply one op; returns trade count so the timed loop can't be optimised away.
template <typename Book>
inline std::size_t apply(Book& book, const Op& op) {
    switch (op.kind) {
        case Kind::AddLimit:  return book.add_limit(op.id, op.side, op.price, op.qty).size();
        case Kind::AddMarket: return book.add_market(op.id, op.side, op.qty).size();
        case Kind::Cancel:    return book.cancel(op.id) ? 1 : 0;
        case Kind::Modify:    return book.modify(op.id, op.price, op.qty).size();
    }
    return 0;
}

// Whole-workload wall-clock throughput (ops/sec).
template <typename Book>
double measure_throughput(Book&& book, const std::vector<Op>& ops, std::size_t& sink) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    std::size_t acc = 0;
    for (const Op& op : ops) acc += apply(book, op);
    const auto t1 = clock::now();
    sink += acc;
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<double>(ops.size()) / secs;
}

// Per-operation latency, sampled with rdtsc into a histogram.
template <typename Book>
void measure_latency(Book&& book, const std::vector<Op>& ops, bench::Histogram& h,
                     std::size_t& sink) {
    h.reserve(ops.size());
    std::size_t acc = 0;
    for (const Op& op : ops) {
        const std::uint64_t a = bench::now_ticks();
        acc += apply(book, op);
        const std::uint64_t b = bench::now_ticks();
        h.add(b - a);
    }
    sink += acc;
}

void report(const char* name, double ops_per_sec, bench::Histogram& h, double ns_per_tick) {
    std::printf("  %-14s  %10.2f M ops/s   p50 %7.1f ns   p99 %8.1f ns   p99.9 %8.1f ns\n",
                name, ops_per_sec / 1e6, h.percentile_ns(0.50, ns_per_tick),
                h.percentile_ns(0.99, ns_per_tick), h.percentile_ns(0.999, ns_per_tick));
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t num_ops = (argc > 1) ? std::stoul(argv[1]) : 2'000'000;
    std::size_t num_ticks = (argc > 2) ? std::stoul(argv[2]) : 1024;
    constexpr Price kFloor = 100'000;

    std::printf("Order book benchmark\n");
    std::printf("  workload: %zu ops over a %zu-tick price band\n\n", num_ops, num_ticks);

    const std::vector<Op> ops = make_workload(num_ops, kFloor, num_ticks);
    const double ns_per_tick = bench::calibrate_ns_per_tick();

    std::size_t sink = 0;

    // Throughput (fresh book per run).
    const double naive_tp = measure_throughput(OrderBook{}, ops, sink);
    const double fast_tp = measure_throughput(FastOrderBook{kFloor, num_ticks}, ops, sink);

    // Latency (fresh book per run).
    bench::Histogram naive_h, fast_h;
    measure_latency(OrderBook{}, ops, naive_h, sink);
    measure_latency(FastOrderBook{kFloor, num_ticks}, ops, fast_h, sink);

    std::printf("Throughput + latency (lower ns is better):\n");
    report("naive (map)", naive_tp, naive_h, ns_per_tick);
    report("fast (ladder)", fast_tp, fast_h, ns_per_tick);
    std::printf("\n  speedup: %.2fx throughput\n", fast_tp / naive_tp);

    if (sink == 0xDEADBEEF) std::printf("");  // keep `sink` observably live
    return 0;
}
