// WebAssembly entry point for the live browser demo. Wraps FastOrderBook in a
// self-contained market simulator so the *entire* hot loop — order generation
// and matching — runs in WASM; JavaScript only pulls a snapshot each frame and
// draws it. Built with Emscripten + embind (see web/build.sh).

#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include <emscripten/bind.h>

#include "lob/fast_order_book.hpp"

using namespace lob;

namespace {

// Append an integer to a JSON string without pulling in a JSON library — the
// snapshot is tiny and built many times per second, so this stays allocation-lean.
void put_int(std::string& s, long long v) { s += std::to_string(v); }

}  // namespace

// A tiny synthetic exchange: a stream of passive and aggressive orders around a
// wandering mid-price, matched by the real engine. Produces a live, moving book
// and a trade tape — enough to show the matching engine working, with no data to
// download.
class MarketSim {
public:
    MarketSim(unsigned seed, int floor, int ticks, int qty_max)
        : floor_(floor),
          ticks_(static_cast<std::size_t>(ticks)),
          qty_max_(qty_max),
          book_(floor, static_cast<std::size_t>(ticks)),
          rng_(seed),
          mid_(floor + ticks / 2) {
        seed_liquidity();
    }

    // Advance the simulation by `n` orders. Returns trades produced this call.
    int step(int n) {
        int produced = 0;
        for (int i = 0; i < n; ++i) produced += one_order();
        return produced;
    }

    // Depth ladder + recent tape + counters, as a compact JSON string for JS.
    std::string snapshot(int levels) {
        const auto bids = book_.depth(Side::Buy, static_cast<std::size_t>(levels));
        const auto asks = book_.depth(Side::Sell, static_cast<std::size_t>(levels));

        std::string s;
        s.reserve(2048);
        s += "{\"bids\":[";
        emit_levels(s, bids);
        s += "],\"asks\":[";
        emit_levels(s, asks);
        s += "],\"tape\":[";
        for (std::size_t i = 0; i < tape_.size(); ++i) {
            const Trade& t = tape_[i];
            if (i) s += ',';
            s += '[';
            put_int(s, t.price);
            s += ',';
            put_int(s, static_cast<long long>(t.quantity));
            s += ',';
            put_int(s, t.taker_side == Side::Buy ? 1 : 0);
            s += ']';
        }
        s += "],\"stats\":{\"ops\":";
        put_int(s, static_cast<long long>(total_ops_));
        s += ",\"trades\":";
        put_int(s, static_cast<long long>(total_trades_));
        s += ",\"resting\":";
        put_int(s, static_cast<long long>(book_.order_count()));
        s += ",\"bid\":";
        put_int(s, book_.best_bid() ? *book_.best_bid() : 0);
        s += ",\"ask\":";
        put_int(s, book_.best_ask() ? *book_.best_ask() : 0);
        s += "}}";
        return s;
    }

    // Price scaling for display: ticks are integer, the UI shows dollars.
    double tick_size() const { return 0.01; }

private:
    void emit_levels(std::string& s, const std::vector<std::pair<Price, Quantity>>& v) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) s += ',';
            s += '[';
            put_int(s, v[i].first);
            s += ',';
            put_int(s, static_cast<long long>(v[i].second));
            s += ']';
        }
    }

    int clamp_price(long long p) const {
        if (p < floor_) return floor_;
        const int hi = floor_ + static_cast<int>(ticks_) - 1;
        return p > hi ? hi : static_cast<int>(p);
    }

    Quantity rand_qty() {
        std::uniform_int_distribution<int> d(1, qty_max_);
        return static_cast<Quantity>(d(rng_));
    }

    // Lay down a band of resting liquidity on both sides so the book opens full.
    void seed_liquidity() {
        for (int off = 1; off <= 20; ++off) {
            book_.add_limit(next_id_++, Side::Buy, clamp_price(mid_ - off), rand_qty());
            live_.push_back(next_id_ - 1);
            book_.add_limit(next_id_++, Side::Sell, clamp_price(mid_ + off), rand_qty());
            live_.push_back(next_id_ - 1);
        }
    }

    // One order of synthetic flow: mostly passive quotes that rest, a steady
    // fraction of aggressive orders that cross and trade, plus random cancels.
    int one_order() {
        ++total_ops_;
        std::uniform_int_distribution<int> roll(0, 99);
        const int r = roll(rng_);
        const bool buy = (rng_ & 1) != 0;
        const Side side = buy ? Side::Buy : Side::Sell;

        // Track the mid from the live book so quotes cluster around the market.
        if (book_.best_bid() && book_.best_ask()) {
            mid_ = static_cast<int>((*book_.best_bid() + *book_.best_ask()) / 2);
        }

        if (r < 25) {  // aggressive: cross the spread and print trades
            const OrderId id = next_id_++;
            std::uniform_int_distribution<int> depth(0, 3);
            const int px = buy ? clamp_price((book_.best_ask() ? *book_.best_ask() : mid_) + depth(rng_))
                               : clamp_price((book_.best_bid() ? *book_.best_bid() : mid_) - depth(rng_));
            auto trades = book_.add_limit(id, side, px, rand_qty());
            record_trades(trades);
            if (book_.contains(id)) live_.push_back(id);  // any remainder rested
            return static_cast<int>(trades.size());
        }
        if (r < 80) {  // passive: join the book a few ticks off the touch
            const OrderId id = next_id_++;
            std::uniform_int_distribution<int> off(1, 8);
            const int px = buy ? clamp_price(mid_ - off(rng_)) : clamp_price(mid_ + off(rng_));
            auto trades = book_.add_limit(id, side, px, rand_qty());
            record_trades(trades);
            if (book_.contains(id)) live_.push_back(id);
            return static_cast<int>(trades.size());
        }
        // cancel a random resting order (compacting the live list lazily)
        while (!live_.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live_.size() - 1);
            const std::size_t k = pick(rng_);
            const OrderId id = live_[k];
            live_[k] = live_.back();
            live_.pop_back();
            if (book_.cancel(id)) break;  // skip ids already filled/cancelled
        }
        return 0;
    }

    void record_trades(const std::vector<Trade>& trades) {
        total_trades_ += trades.size();
        for (const Trade& t : trades) {
            tape_.push_front(t);
            if (tape_.size() > 40) tape_.pop_back();
        }
    }

    int floor_;
    std::size_t ticks_;
    int qty_max_;
    FastOrderBook book_;
    std::mt19937_64 rng_;
    int mid_;
    OrderId next_id_ = 1;
    std::vector<OrderId> live_;
    std::deque<Trade> tape_;
    std::uint64_t total_ops_ = 0;
    std::uint64_t total_trades_ = 0;
};

EMSCRIPTEN_BINDINGS(lob_module) {
    emscripten::class_<MarketSim>("MarketSim")
        .constructor<unsigned, int, int, int>()
        .function("step", &MarketSim::step)
        .function("snapshot", &MarketSim::snapshot)
        .function("tickSize", &MarketSim::tick_size);
}
