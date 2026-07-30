#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/order.hpp"
#include "lob/trade.hpp"
#include "lob/types.hpp"

namespace lob {

// Cache-friendly limit order book with identical semantics to OrderBook, built
// for latency (Milestone 3). Two structural changes replace the naive
// std::map<Price, std::list<Order>>:
//
//   1. A flat price ladder. Price levels live in one contiguous std::vector
//      indexed by (price - floor), so locating a level is an array offset rather
//      than a red-black-tree descent. Best bid/ask are cached tick indices.
//
//   2. An intrusive order pool. Every order is a slot in one std::vector, linked
//      into its level by integer next/prev indices (not pointers, not list
//      nodes). Allocation is a free-list pop; there is no per-order heap churn,
//      and a whole level is a walk over pool slots that stay hot in cache.
//
// The ladder covers a fixed tick band [floor, floor + num_ticks); prices outside
// it are rejected. Real venues run bounded ladders for exactly this reason. The
// book is the drop-in fast path validated against OrderBook by a differential
// test, so the naive book remains the correctness oracle.
class FastOrderBook {
public:
    static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

    // Ladder spans [floor, floor + num_ticks) in integer ticks.
    FastOrderBook(Price floor, std::size_t num_ticks);

    // --- Order lifecycle (same contract as OrderBook) ----------------------
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity quantity);
    std::vector<Trade> add_market(OrderId id, Side side, Quantity quantity);
    bool cancel(OrderId id);
    std::vector<Trade> modify(OrderId id, Price new_price, Quantity new_quantity);

    // --- Read-only queries -------------------------------------------------
    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const;
    [[nodiscard]] bool contains(OrderId id) const { return index_.count(id) != 0; }
    [[nodiscard]] std::size_t order_count() const { return index_.size(); }
    [[nodiscard]] bool empty() const { return index_.empty(); }

    // True if `price` falls within the configured ladder band.
    [[nodiscard]] bool in_range(Price price) const {
        return price >= floor_ && price < floor_ + static_cast<Price>(num_ticks_);
    }

private:
    // One resting order, packed to stay dense in the pool. next/prev are pool
    // slot indices threading the FIFO queue at this order's price level.
    struct Node {
        OrderId id;
        Quantity remaining;
        std::int64_t tick;   // ladder index of this order's level
        std::uint32_t next;  // next order at the level (toward the tail), or kInvalid
        std::uint32_t prev;  // previous order (toward the head), or kInvalid
        Side side;
    };

    // A price level: intrusive FIFO list head/tail into the pool + aggregate qty
    // so quantity_at is O(1). Cache-line aligned so hot best-of-book levels don't
    // share a line with neighbours.
    struct alignas(64) Level {
        std::uint32_t head = kInvalid;
        std::uint32_t tail = kInvalid;
        Quantity total = 0;
    };

    [[nodiscard]] std::int64_t to_tick(Price price) const { return price - floor_; }
    [[nodiscard]] Price to_price(std::int64_t tick) const { return floor_ + tick; }

    std::uint32_t alloc_node();
    void free_node(std::uint32_t slot);
    void link_back(std::int64_t tick, std::uint32_t slot);   // append to level FIFO
    void unlink(std::uint32_t slot);                          // remove from its level

    // Advance the cached best-bid/ask cursor to the next non-empty level after
    // the current one emptied (bids scan down, asks scan up), or mark none.
    void repair_best_bid();
    void repair_best_ask();

    template <bool BuySide>
    void match(Order& incoming, std::vector<Trade>& out);

    void rest(OrderId id, Side side, std::int64_t tick, Quantity qty);

    Price floor_;
    std::size_t num_ticks_;
    std::vector<Level> ladder_;
    std::vector<Node> pool_;
    std::uint32_t free_head_ = kInvalid;
    std::unordered_map<OrderId, std::uint32_t> index_;  // id -> pool slot

    std::int64_t best_bid_tick_ = -1;                    // highest non-empty bid tick, or -1
    std::int64_t best_ask_tick_ = -1;                    // lowest  non-empty ask tick, or -1
};

}  // namespace lob
