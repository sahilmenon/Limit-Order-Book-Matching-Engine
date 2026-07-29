#include "lob/order_book.hpp"

#include <gtest/gtest.h>

using namespace lob;

namespace {

// Convenience: total quantity across all trades.
Quantity total_qty(const std::vector<Trade>& trades) {
    Quantity q = 0;
    for (const auto& t : trades) q += t.quantity;
    return q;
}

}  // namespace

// --- Resting & book queries -------------------------------------------------

TEST(OrderBook, RestingLimitUpdatesTopOfBook) {
    OrderBook book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());

    EXPECT_TRUE(book.add_limit(1, Side::Buy, 100, 10).empty());
    EXPECT_TRUE(book.add_limit(2, Side::Sell, 105, 7).empty());

    ASSERT_TRUE(book.best_bid().has_value());
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(*book.best_bid(), 100);
    EXPECT_EQ(*book.best_ask(), 105);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 10u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 105), 7u);
    EXPECT_EQ(book.order_count(), 2u);
}

TEST(OrderBook, BestBidIsHighestBestAskIsLowest) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 98, 1);
    book.add_limit(2, Side::Buy, 101, 1);
    book.add_limit(3, Side::Buy, 99, 1);
    book.add_limit(4, Side::Sell, 110, 1);
    book.add_limit(5, Side::Sell, 106, 1);
    book.add_limit(6, Side::Sell, 108, 1);

    EXPECT_EQ(*book.best_bid(), 101);
    EXPECT_EQ(*book.best_ask(), 106);
}

// --- Matching: crossing, partials, price improvement ------------------------

TEST(OrderBook, NonCrossingLimitJustRests) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);
    auto trades = book.add_limit(2, Side::Sell, 105, 5);  // ask above bid: no cross
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.order_count(), 2u);
}

TEST(OrderBook, FullMatchRemovesBothOrders) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);
    auto trades = book.add_limit(2, Side::Buy, 100, 10);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_EQ(trades[0].taker_id, 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(trades[0].taker_side, Side::Buy);
    EXPECT_TRUE(book.empty());
}

TEST(OrderBook, TakerLargerThanRestingLeavesRemainderOnBook) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 4);
    auto trades = book.add_limit(2, Side::Buy, 100, 10);  // buys 4, rests 6

    EXPECT_EQ(total_qty(trades), 4u);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 100);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 6u);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, RestingLargerThanTakerIsPartiallyFilled) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);
    auto trades = book.add_limit(2, Side::Buy, 100, 4);  // takes 4 of the 10

    EXPECT_EQ(total_qty(trades), 4u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 6u);
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBook, TradePrintsAtMakerPrice) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);        // maker sets the price
    auto trades = book.add_limit(2, Side::Buy, 103, 10);  // buyer willing to pay more

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100);  // executes at resting price, not 103
}

TEST(OrderBook, AggressiveOrderSweepsMultiplePriceLevels) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 101, 5);
    book.add_limit(3, Side::Sell, 102, 5);

    auto trades = book.add_limit(4, Side::Buy, 101, 8);  // sweeps 100 then part of 101

    EXPECT_EQ(total_qty(trades), 8u);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_EQ(trades[1].price, 101);
    EXPECT_EQ(trades[1].quantity, 3u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 101), 2u);  // 2 left at 101
    EXPECT_EQ(*book.best_ask(), 101);                  // 102 untouched behind it
}

// --- Price-time priority ----------------------------------------------------

TEST(OrderBook, TimePriorityIsFifoWithinPriceLevel) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);  // arrived first
    book.add_limit(2, Side::Buy, 100, 5);  // arrived second

    auto trades = book.add_limit(3, Side::Sell, 100, 5);  // should hit order 1
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 1u);
    EXPECT_TRUE(book.contains(2));
    EXPECT_FALSE(book.contains(1));
}

TEST(OrderBook, BetterPricedRestingOrderMatchesFirst) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 99, 5);
    book.add_limit(2, Side::Buy, 100, 5);  // better bid, must trade first

    auto trades = book.add_limit(3, Side::Sell, 99, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 2u);
    EXPECT_EQ(trades[0].price, 100);
}

// --- Market orders ----------------------------------------------------------

TEST(OrderBook, MarketOrderMatchesBestAvailableAndDiscardsRemainder) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 3);
    book.add_limit(2, Side::Sell, 101, 3);

    auto trades = book.add_market(3, Side::Buy, 10);  // only 6 available
    EXPECT_EQ(total_qty(trades), 6u);
    EXPECT_TRUE(book.empty());                        // remainder (4) discarded, nothing rests
}

TEST(OrderBook, MarketOrderOnEmptyBookProducesNoTrades) {
    OrderBook book;
    auto trades = book.add_market(1, Side::Buy, 10);
    EXPECT_TRUE(trades.empty());
    EXPECT_TRUE(book.empty());
}

// --- Cancel -----------------------------------------------------------------

TEST(OrderBook, CancelRemovesRestingOrder) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);
    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_TRUE(book.empty());
}

TEST(OrderBook, CancelUnknownOrderReturnsFalse) {
    OrderBook book;
    EXPECT_FALSE(book.cancel(42));
}

TEST(OrderBook, CancelPreservesOtherOrdersAtSameLevel) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(2, Side::Buy, 100, 5);
    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 5u);

    auto trades = book.add_limit(3, Side::Sell, 100, 5);  // must hit order 2
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 2u);
}

// --- Modify -----------------------------------------------------------------

TEST(OrderBook, ModifyReduceQuantityKeepsTimePriority) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);  // first in line
    book.add_limit(2, Side::Buy, 100, 10);

    auto trades = book.modify(1, 100, 4);  // same price, smaller qty -> keeps priority
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 14u);

    auto hit = book.add_limit(3, Side::Sell, 100, 4);  // should still hit order 1 first
    ASSERT_EQ(hit.size(), 1u);
    EXPECT_EQ(hit[0].maker_id, 1u);
    EXPECT_EQ(hit[0].quantity, 4u);
}

TEST(OrderBook, ModifyIncreaseQuantityLosesTimePriority) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);
    book.add_limit(2, Side::Buy, 100, 10);

    book.modify(1, 100, 20);  // increase -> goes to back of the queue

    auto trades = book.add_limit(3, Side::Sell, 100, 10);  // now hits order 2 first
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].maker_id, 2u);
}

TEST(OrderBook, ModifyPriceCanCrossAndTrade) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 105, 10);
    book.add_limit(2, Side::Buy, 100, 10);   // resting bid below the ask

    auto trades = book.modify(2, 105, 10);   // reprice bid up to 105 -> crosses
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 105);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_TRUE(book.empty());
}

TEST(OrderBook, ModifyToZeroQuantityCancels) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);
    auto trades = book.modify(1, 100, 0);
    EXPECT_TRUE(trades.empty());
    EXPECT_FALSE(book.contains(1));
    EXPECT_TRUE(book.empty());
}

TEST(OrderBook, ModifyUnknownOrderIsNoOp) {
    OrderBook book;
    auto trades = book.modify(99, 100, 5);
    EXPECT_TRUE(trades.empty());
}
