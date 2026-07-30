// WebAssembly entry point for the live browser demo. Wraps FastOrderBook in a
// self-contained market simulator so the *entire* hot loop (order generation
// and matching) runs in WASM; JavaScript only pulls a snapshot each frame and
// draws it. Built with Emscripten + embind (see web/build.sh).

#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "lob/fast_order_book.hpp"
#include "lob/itch/itch.hpp"
#include "lob/order_book.hpp"

using namespace lob;

namespace {

// Append an integer to a JSON string without pulling in a JSON library. The
// snapshot is tiny and built many times per second, so this stays allocation-lean.
void put_int(std::string& s, long long v) { s += std::to_string(v); }

}  // namespace

// A tiny synthetic exchange: a stream of passive and aggressive orders around a
// wandering mid-price, matched by the real engine. Produces a live, moving book
// and a trade tape, enough to show the matching engine working, with no data to
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
        const bool buy = (rng_() & 1) != 0;
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

// Replays a recorded NASDAQ TotalView-ITCH 5.0 stream for one symbol, rebuilding
// the real displayed book through the order book's non-matching maintenance API
// (the same path Milestone 2 validates against a Python reference). The input is
// a compact per-symbol slice carved by scripts/extract_symbol_itch.py, so the
// browser replays genuine market data without shipping a multi-GB session.
class ItchReplay {
public:
    // Copy an ITCH byte slice in from JS (a Uint8Array) and rewind to the start.
    void load(emscripten::val bytes) {
        buf_ = emscripten::convertJSArrayToNumberVector<std::uint8_t>(bytes);
        reset();
    }

    void reset() {
        book_ = OrderBook{};
        tape_.clear();
        pos_ = 0;
        applied_ = 0;
        execs_ = 0;
    }

    // Apply the next `n` messages; returns executions seen this call. When the
    // slice ends it loops: the book is rebuilt from the top so the demo runs on.
    int step(int n) {
        int e = 0;
        for (int i = 0; i < n; ++i) {
            if (pos_ + 2 > buf_.size()) {
                book_ = OrderBook{};  // wrap and replay from the start
                pos_ = 0;
            }
            if (pos_ + 2 > buf_.size()) break;  // buffer too small / empty
            const std::size_t len = itch::be16(&buf_[pos_]);
            if (len == 0 || pos_ + 2 + len > buf_.size()) {
                pos_ = buf_.size();  // truncated tail: stop this pass, wrap next call
                continue;
            }
            const std::uint8_t* p = &buf_[pos_ + 2];
            pos_ += 2 + len;
            e += apply_one(p);
        }
        return e;
    }

    std::string snapshot(int levels) {
        std::string s;
        s.reserve(2048);
        s += "{\"bids\":[";
        emit_map(s, book_.bids(), static_cast<std::size_t>(levels));
        s += "],\"asks\":[";
        emit_map(s, book_.asks(), static_cast<std::size_t>(levels));
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
        put_int(s, static_cast<long long>(applied_));
        s += ",\"trades\":";
        put_int(s, static_cast<long long>(execs_));
        s += ",\"resting\":";
        put_int(s, static_cast<long long>(book_.order_count()));
        s += ",\"bid\":";
        put_int(s, book_.best_bid() ? *book_.best_bid() : 0);
        s += ",\"ask\":";
        put_int(s, book_.best_ask() ? *book_.best_ask() : 0);
        s += ",\"progress\":";
        put_int(s, buf_.empty() ? 0 : static_cast<long long>(pos_ * 100 / buf_.size()));
        s += "}}";
        return s;
    }

private:
    // Apply one ITCH message to the book; return 1 if it was an execution.
    int apply_one(const std::uint8_t* p) {
        switch (static_cast<char>(p[0])) {
            case itch::AddOrder:
            case itch::AddOrderMPID: {
                const OrderId ref = itch::be64(p + 11);
                const Side side = itch::side_from_itch(p[19]);
                const Quantity shares = itch::be32(p + 20);
                const Price price = static_cast<Price>(itch::be32(p + 32));
                book_.add_resting(ref, side, price, shares);
                ++applied_;
                return 0;
            }
            case itch::OrderExecuted:
            case itch::OrderExecutedWithPrice: {
                const OrderId ref = itch::be64(p + 11);
                const Quantity shares = itch::be32(p + 19);
                // ITCH names only the resting order; read its price/side for the
                // tape, then reduce. The aggressor is the opposite side.
                if (auto maker = book_.find_order(ref)) {
                    record_exec(maker->price, shares, opposite(maker->side));
                    book_.reduce(ref, shares);
                    ++applied_;
                }
                return 1;
            }
            case itch::OrderCancel: {
                book_.reduce(itch::be64(p + 11), itch::be32(p + 19));
                ++applied_;
                return 0;
            }
            case itch::OrderDelete: {
                book_.remove(itch::be64(p + 11));
                ++applied_;
                return 0;
            }
            case itch::OrderReplace: {
                const OrderId orig = itch::be64(p + 11);
                const OrderId repl = itch::be64(p + 19);
                const Quantity shares = itch::be32(p + 27);
                const Price price = static_cast<Price>(itch::be32(p + 31));
                book_.replace(orig, repl, price, shares);
                ++applied_;
                return 0;
            }
            default:
                return 0;  // R and other messages don't move the displayed book
        }
    }

    void record_exec(Price price, Quantity shares, Side taker_side) {
        tape_.push_front(Trade{0, 0, price, shares, taker_side});
        if (tape_.size() > 40) tape_.pop_back();
        ++execs_;
    }

    // Emit up to `levels` (price, aggregate qty) pairs from a best-first side map.
    template <typename Map>
    void emit_map(std::string& s, const Map& side, std::size_t levels) {
        std::size_t emitted = 0;
        for (const auto& [price, level] : side) {
            if (emitted >= levels) break;
            Quantity total = 0;
            for (const Order& o : level) total += o.remaining;
            if (emitted) s += ',';
            s += '[';
            put_int(s, price);
            s += ',';
            put_int(s, static_cast<long long>(total));
            s += ']';
            ++emitted;
        }
    }

    OrderBook book_;
    std::vector<std::uint8_t> buf_;
    std::deque<Trade> tape_;
    std::size_t pos_ = 0;
    std::uint64_t applied_ = 0;
    std::uint64_t execs_ = 0;
};

EMSCRIPTEN_BINDINGS(lob_module) {
    emscripten::class_<MarketSim>("MarketSim")
        .constructor<unsigned, int, int, int>()
        .function("step", &MarketSim::step)
        .function("snapshot", &MarketSim::snapshot)
        .function("tickSize", &MarketSim::tick_size);

    emscripten::class_<ItchReplay>("ItchReplay")
        .constructor<>()
        .function("load", &ItchReplay::load)
        .function("step", &ItchReplay::step)
        .function("snapshot", &ItchReplay::snapshot)
        .function("reset", &ItchReplay::reset);
}
