#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/order.hpp"
#include "lob/trade.hpp"
#include "lob/types.hpp"

namespace lob {

// Single-symbol limit order book with price-time (FIFO) priority.
//
// Milestone 1: correctness first. Price levels are held in ordered std::maps and
// each level is a std::list of orders in arrival order. This is deliberately the
// "naive" structure named in the build spec; it is easy to reason about and
// serves as the correctness reference that the later cache-friendly rewrite
// (intrusive lists + flat price arrays) must match exactly.
class OrderBook {
public:
    // A resting order at a price level. Orders within a level are stored in a
    // std::list, so their relative position encodes time priority (front = oldest).
    using Level = std::list<Order>;

    // Bids are ordered highest-price-first, asks lowest-price-first, so that
    // begin() of each map is always the best (most aggressive) price.
    using BidMap = std::map<Price, Level, std::greater<Price>>;
    using AskMap = std::map<Price, Level, std::less<Price>>;

    OrderBook() = default;

    // --- Order lifecycle ---------------------------------------------------

    // Submit a limit order. Matches against the opposite side up to `price`,
    // then rests any remainder on the book. Returns the trades generated.
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity quantity);

    // Submit a market order. Matches against the opposite side at any price
    // until filled or the book is exhausted; never rests. Returns the trades.
    std::vector<Trade> add_market(OrderId id, Side side, Quantity quantity);

    // Cancel a resting order by id. Returns true if the order was found and removed.
    bool cancel(OrderId id);

    // Modify a resting order.
    //  - Reducing quantity keeps time priority (edit in place).
    //  - Increasing quantity or changing price loses priority: the order is
    //    cancelled and re-inserted at the back of the (possibly new) level,
    //    which may cross and generate trades.
    // Returns the trades generated (empty unless a price change crosses).
    std::vector<Trade> modify(OrderId id, Price new_price, Quantity new_quantity);

    // --- Read-only queries -------------------------------------------------

    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;

    // Aggregate open quantity resting at a given price on a given side.
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const;

    [[nodiscard]] bool contains(OrderId id) const { return index_.count(id) != 0; }
    [[nodiscard]] std::size_t order_count() const { return index_.size(); }
    [[nodiscard]] bool empty() const { return index_.empty(); }

    const BidMap& bids() const { return bids_; }
    const AskMap& asks() const { return asks_; }

private:
    // Where a resting order lives, so cancel/modify are O(1) average.
    struct Location {
        Side side;
        Price price;
        Level::iterator it;  // stable: std::list iterators survive other edits
    };

    // Core matching routine, templated over the opposite-side map type so the
    // same logic serves both a buy (matching asks) and a sell (matching bids).
    template <typename OppMap>
    void match(OppMap& opp, Order& incoming, std::vector<Trade>& out);

    // Rest the remaining quantity of `order` on its own side of the book.
    void rest(const Order& order);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Location> index_;
    Sequence next_sequence_{0};
};

}  // namespace lob
