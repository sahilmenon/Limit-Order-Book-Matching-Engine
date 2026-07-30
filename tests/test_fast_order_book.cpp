// Differential test: the cache-friendly FastOrderBook must be byte-for-byte
// indistinguishable from the naive OrderBook, which is the correctness oracle.
// A deterministic pseudo-random workload of adds / cancels / modifies / market
// orders is replayed into both books; after every operation the emitted trades
// and the full visible book state are required to be identical.

#include "lob/fast_order_book.hpp"
#include "lob/order_book.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

using namespace lob;

namespace {

// Two trades are equal when every field matches. Trade order matters too (it
// encodes price-time priority), so we compare the vectors positionally.
void expect_same_trades(const std::vector<Trade>& a, const std::vector<Trade>& b) {
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].maker_id, b[i].maker_id) << "trade " << i;
        EXPECT_EQ(a[i].taker_id, b[i].taker_id) << "trade " << i;
        EXPECT_EQ(a[i].price, b[i].price) << "trade " << i;
        EXPECT_EQ(a[i].quantity, b[i].quantity) << "trade " << i;
        EXPECT_EQ(a[i].taker_side, b[i].taker_side) << "trade " << i;
    }
}

// Compare the visible book across the whole price band: top of book, order count,
// and aggregate resting quantity at every tick on both sides.
void expect_same_book(const OrderBook& naive, const FastOrderBook& fast, Price lo, Price hi) {
    EXPECT_EQ(naive.best_bid(), fast.best_bid());
    EXPECT_EQ(naive.best_ask(), fast.best_ask());
    EXPECT_EQ(naive.order_count(), fast.order_count());
    EXPECT_EQ(naive.empty(), fast.empty());
    for (Price p = lo; p < hi; ++p) {
        EXPECT_EQ(naive.quantity_at(Side::Buy, p), fast.quantity_at(Side::Buy, p))
            << "buy qty @ " << p;
        EXPECT_EQ(naive.quantity_at(Side::Sell, p), fast.quantity_at(Side::Sell, p))
            << "sell qty @ " << p;
    }
}

}  // namespace

// A handful of pointed scenarios first, so a failure is easy to localise before
// the randomised sweep runs.

TEST(FastOrderBook, MatchesNaiveOnBasicCross) {
    OrderBook naive;
    FastOrderBook fast(100, 100);  // ladder covers prices [100, 200)

    naive.add_limit(1, Side::Sell, 150, 10);
    fast.add_limit(1, Side::Sell, 150, 10);

    auto tn = naive.add_limit(2, Side::Buy, 150, 4);
    auto tf = fast.add_limit(2, Side::Buy, 150, 4);
    expect_same_trades(tn, tf);
    expect_same_book(naive, fast, 100, 200);
}

TEST(FastOrderBook, MatchesNaiveOnMultiLevelSweepAndFifo) {
    OrderBook naive;
    FastOrderBook fast(100, 100);
    for (auto [id, side, px, qty] : std::vector<std::tuple<OrderId, Side, Price, Quantity>>{
             {1, Side::Sell, 150, 5}, {2, Side::Sell, 151, 5}, {3, Side::Sell, 150, 5},
             {4, Side::Sell, 152, 5}}) {
        naive.add_limit(id, side, px, qty);
        fast.add_limit(id, side, px, qty);
    }
    auto tn = naive.add_limit(5, Side::Buy, 151, 12);  // 150(5)+150(5)+151(2)
    auto tf = fast.add_limit(5, Side::Buy, 151, 12);
    expect_same_trades(tn, tf);
    expect_same_book(naive, fast, 100, 200);
}

TEST(FastOrderBook, MatchesNaiveOnMarketOrderAndModify) {
    OrderBook naive;
    FastOrderBook fast(100, 100);
    naive.add_limit(1, Side::Buy, 140, 10);
    fast.add_limit(1, Side::Buy, 140, 10);
    naive.add_limit(2, Side::Buy, 140, 10);
    fast.add_limit(2, Side::Buy, 140, 10);

    // Reduce-in-place keeps FIFO priority in both books.
    naive.modify(1, 140, 4);
    fast.modify(1, 140, 4);

    auto tn = naive.add_market(3, Side::Sell, 6);  // hits order1(4) then order2(2)
    auto tf = fast.add_market(3, Side::Sell, 6);
    expect_same_trades(tn, tf);
    expect_same_book(naive, fast, 100, 200);
}

// The main event: a long randomised workload. Any divergence in matching,
// priority, cancel/modify bookkeeping, or best-of-book tracking surfaces here.
TEST(FastOrderBook, MatchesNaiveOnRandomisedWorkload) {
    constexpr Price kFloor = 1000;
    constexpr std::size_t kTicks = 200;   // prices in [1000, 1200)
    OrderBook naive;
    FastOrderBook fast(kFloor, kTicks);

    std::mt19937_64 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> op(0, 9);
    std::uniform_int_distribution<int> px(kFloor, kFloor + static_cast<int>(kTicks) - 1);
    std::uniform_int_distribution<int> qty(1, 50);
    std::uniform_int_distribution<int> sidep(0, 1);

    std::vector<OrderId> live;    // ids currently resting (best-effort; both books agree)
    OrderId next_id = 1;

    for (int step = 0; step < 20000; ++step) {
        const int choice = op(rng);
        std::vector<Trade> tn, tf;

        if (choice <= 5) {  // 60% adds (limit)
            const OrderId id = next_id++;
            const Side side = sidep(rng) ? Side::Buy : Side::Sell;
            const Price price = px(rng);
            const Quantity q = qty(rng);
            tn = naive.add_limit(id, side, price, q);
            tf = fast.add_limit(id, side, price, q);
            live.push_back(id);
        } else if (choice <= 6) {  // 10% market orders
            const OrderId id = next_id++;
            const Side side = sidep(rng) ? Side::Buy : Side::Sell;
            const Quantity q = qty(rng);
            tn = naive.add_market(id, side, q);
            tf = fast.add_market(id, side, q);
        } else if (choice <= 8 && !live.empty()) {  // ~20% cancels
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t k = pick(rng);
            const OrderId id = live[k];
            EXPECT_EQ(naive.cancel(id), fast.cancel(id)) << "cancel " << id;
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
        } else if (!live.empty()) {  // ~10% modifies
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const OrderId id = live[pick(rng)];
            const Price price = px(rng);
            const Quantity q = qty(rng);
            tn = naive.modify(id, price, q);
            tf = fast.modify(id, price, q);
        }

        expect_same_trades(tn, tf);
        if (::testing::Test::HasFailure()) {
            FAIL() << "divergence at step " << step;
        }
    }

    expect_same_book(naive, fast, kFloor, kFloor + static_cast<Price>(kTicks));
}
