#include "lob/fast_order_book.hpp"

#include <algorithm>

namespace lob {

FastOrderBook::FastOrderBook(Price floor, std::size_t num_ticks)
    : floor_(floor), num_ticks_(num_ticks), ladder_(num_ticks) {
    pool_.reserve(1024);  // amortise the first burst of adds
}

std::uint32_t FastOrderBook::alloc_node() {
    if (free_head_ != kInvalid) {
        const std::uint32_t slot = free_head_;
        free_head_ = pool_[slot].next;
        return slot;
    }
    pool_.push_back(Node{});
    return static_cast<std::uint32_t>(pool_.size() - 1);
}

void FastOrderBook::free_node(std::uint32_t slot) {
    pool_[slot].next = free_head_;
    free_head_ = slot;
}

void FastOrderBook::link_back(std::int64_t tick, std::uint32_t slot) {
    Level& lvl = ladder_[static_cast<std::size_t>(tick)];
    Node& n = pool_[slot];
    n.next = kInvalid;
    n.prev = lvl.tail;
    if (lvl.tail != kInvalid) {
        pool_[lvl.tail].next = slot;
    } else {
        lvl.head = slot;  // first order at this level
    }
    lvl.tail = slot;
}

void FastOrderBook::unlink(std::uint32_t slot) {
    Node& n = pool_[slot];
    Level& lvl = ladder_[static_cast<std::size_t>(n.tick)];
    if (n.prev != kInvalid) {
        pool_[n.prev].next = n.next;
    } else {
        lvl.head = n.next;  // removed the head
    }
    if (n.next != kInvalid) {
        pool_[n.next].prev = n.prev;
    } else {
        lvl.tail = n.prev;  // removed the tail
    }
    lvl.total -= n.remaining;
}

void FastOrderBook::repair_best_bid() {
    std::int64_t t = best_bid_tick_;
    while (t >= 0 && ladder_[static_cast<std::size_t>(t)].head == kInvalid) --t;
    best_bid_tick_ = t;  // -1 when the bid side is now empty
}

void FastOrderBook::repair_best_ask() {
    std::int64_t t = best_ask_tick_;
    const auto n = static_cast<std::int64_t>(num_ticks_);
    while (t >= 0 && t < n && ladder_[static_cast<std::size_t>(t)].head == kInvalid) ++t;
    best_ask_tick_ = (t < n) ? t : -1;
}

void FastOrderBook::rest(OrderId id, Side side, std::int64_t tick, Quantity qty) {
    const std::uint32_t slot = alloc_node();
    Node& n = pool_[slot];
    n.id = id;
    n.remaining = qty;
    n.tick = tick;
    n.side = side;
    link_back(tick, slot);
    ladder_[static_cast<std::size_t>(tick)].total += qty;
    index_[id] = slot;

    if (side == Side::Buy) {
        if (best_bid_tick_ < 0 || tick > best_bid_tick_) best_bid_tick_ = tick;
    } else {
        if (best_ask_tick_ < 0 || tick < best_ask_tick_) best_ask_tick_ = tick;
    }
}

template <bool BuySide>
void FastOrderBook::match(Order& incoming, std::vector<Trade>& out) {
    // Buyers consume the ask ladder upward from the best ask; sellers consume the
    // bid ladder downward from the best bid. A limit order stops once the resting
    // price would no longer cross; a market order sweeps to the ladder edge.
    const bool is_market = incoming.type == OrderType::Market;

    while (incoming.remaining > 0) {
        std::int64_t& best = BuySide ? best_ask_tick_ : best_bid_tick_;
        if (best < 0) break;  // opposite side empty

        if (!is_market) {
            const std::int64_t limit_tick = to_tick(incoming.price);
            const bool crosses = BuySide ? (best <= limit_tick) : (best >= limit_tick);
            if (!crosses) break;
        }

        Level& lvl = ladder_[static_cast<std::size_t>(best)];
        const Price level_price = to_price(best);

        while (incoming.remaining > 0 && lvl.head != kInvalid) {
            const std::uint32_t head = lvl.head;
            Node& maker = pool_[head];
            const Quantity fill = std::min(incoming.remaining, maker.remaining);

            out.push_back(Trade{
                .maker_id = maker.id,
                .taker_id = incoming.id,
                .price = level_price,
                .quantity = fill,
                .taker_side = incoming.side,
            });

            incoming.remaining -= fill;
            maker.remaining -= fill;
            lvl.total -= fill;

            if (maker.remaining == 0) {
                index_.erase(maker.id);
                unlink(head);       // maker.remaining is 0, so total is unchanged here
                free_node(head);
            }
        }

        if (lvl.head == kInvalid) {
            // Best level fully consumed: advance the cached cursor to the next one.
            if constexpr (BuySide) {
                repair_best_ask();
            } else {
                repair_best_bid();
            }
        }
    }
}

std::vector<Trade> FastOrderBook::add_limit(OrderId id, Side side, Price price, Quantity quantity) {
    std::vector<Trade> trades;
    if (quantity == 0 || !in_range(price)) return trades;

    Order incoming{
        .id = id,
        .side = side,
        .type = OrderType::Limit,
        .price = price,
        .quantity = quantity,
        .remaining = quantity,
        .sequence = 0,
    };

    if (side == Side::Buy) {
        match<true>(incoming, trades);
    } else {
        match<false>(incoming, trades);
    }

    if (incoming.remaining > 0) {
        rest(id, side, to_tick(price), incoming.remaining);
    }
    return trades;
}

std::vector<Trade> FastOrderBook::add_market(OrderId id, Side side, Quantity quantity) {
    std::vector<Trade> trades;
    if (quantity == 0) return trades;

    Order incoming{
        .id = id,
        .side = side,
        .type = OrderType::Market,
        .price = 0,
        .quantity = quantity,
        .remaining = quantity,
        .sequence = 0,
    };

    if (side == Side::Buy) {
        match<true>(incoming, trades);
    } else {
        match<false>(incoming, trades);
    }
    // Unfilled market quantity is discarded (never rests), same as OrderBook.
    return trades;
}

bool FastOrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    const std::uint32_t slot = it->second;
    const Node& n = pool_[slot];
    const std::int64_t tick = n.tick;
    const Side side = n.side;

    unlink(slot);
    free_node(slot);
    index_.erase(it);

    // If the emptied level was the top of its side, advance the cached cursor.
    if (ladder_[static_cast<std::size_t>(tick)].head == kInvalid) {
        if (side == Side::Buy && tick == best_bid_tick_) repair_best_bid();
        if (side == Side::Sell && tick == best_ask_tick_) repair_best_ask();
    }
    return true;
}

std::vector<Trade> FastOrderBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
    std::vector<Trade> trades;
    auto it = index_.find(id);
    if (it == index_.end()) return trades;  // unknown order: no-op

    if (new_quantity == 0) {
        cancel(id);
        return trades;
    }

    const std::uint32_t slot = it->second;
    Node& n = pool_[slot];
    const bool price_unchanged = in_range(new_price) && to_tick(new_price) == n.tick;
    const bool quantity_reduced = new_quantity <= n.remaining;

    if (price_unchanged && quantity_reduced) {
        // Same price, smaller quantity: keep time priority, edit in place.
        ladder_[static_cast<std::size_t>(n.tick)].total -= (n.remaining - new_quantity);
        n.remaining = new_quantity;
        return trades;
    }

    // Otherwise lose priority: cancel and resubmit (a reprice may now cross).
    const Side side = n.side;
    cancel(id);
    return add_limit(id, side, new_price, new_quantity);
}

std::optional<Price> FastOrderBook::best_bid() const {
    if (best_bid_tick_ < 0) return std::nullopt;
    return to_price(best_bid_tick_);
}

std::optional<Price> FastOrderBook::best_ask() const {
    if (best_ask_tick_ < 0) return std::nullopt;
    return to_price(best_ask_tick_);
}

std::vector<std::pair<Price, Quantity>> FastOrderBook::depth(Side side,
                                                            std::size_t max_levels) const {
    std::vector<std::pair<Price, Quantity>> out;
    out.reserve(max_levels);
    const auto n = static_cast<std::int64_t>(num_ticks_);
    if (side == Side::Buy) {
        for (std::int64_t t = best_bid_tick_; t >= 0 && out.size() < max_levels; --t) {
            if (ladder_[static_cast<std::size_t>(t)].head != kInvalid)
                out.emplace_back(to_price(t), ladder_[static_cast<std::size_t>(t)].total);
        }
    } else {
        for (std::int64_t t = best_ask_tick_; t >= 0 && t < n && out.size() < max_levels; ++t) {
            if (ladder_[static_cast<std::size_t>(t)].head != kInvalid)
                out.emplace_back(to_price(t), ladder_[static_cast<std::size_t>(t)].total);
        }
    }
    return out;
}

Quantity FastOrderBook::quantity_at(Side side, Price price) const {
    if (!in_range(price)) return 0;
    const Level& lvl = ladder_[static_cast<std::size_t>(to_tick(price))];
    if (lvl.head == kInvalid) return 0;
    // A level only ever holds one side at a time (the book never crosses); return
    // 0 if the caller asked about the side that isn't resting here.
    if (pool_[lvl.head].side != side) return 0;
    return lvl.total;
}

}  // namespace lob
