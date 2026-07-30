#include "lob/order_book.hpp"

#include <algorithm>
#include <limits>

namespace lob {

namespace {

// Would an incoming order priced at `incoming_price` cross a resting level at
// `level_price`? For a buyer, yes when the ask is at or below the bid price;
// for a seller, yes when the bid is at or above the ask price. Market orders
// pass Price extremes so they always cross.
constexpr bool crosses(Side incoming_side, Price incoming_price, Price level_price) noexcept {
    return incoming_side == Side::Buy ? level_price <= incoming_price
                                      : level_price >= incoming_price;
}

}  // namespace

template <typename OppMap>
void OrderBook::match(OppMap& opp, Order& incoming, std::vector<Trade>& out) {
    // Walk the opposite book from its best price outward, filling the incoming
    // order against resting orders in strict price-then-time order.
    while (incoming.remaining > 0 && !opp.empty()) {
        auto level_it = opp.begin();          // best price on the opposite side
        const Price level_price = level_it->first;

        if (!crosses(incoming.side, incoming.price, level_price)) {
            break;  // best opposite price no longer crosses; nothing more to do
        }

        Level& resting = level_it->second;
        while (incoming.remaining > 0 && !resting.empty()) {
            Order& maker = resting.front();   // oldest order at this level (FIFO)
            const Quantity fill = std::min(incoming.remaining, maker.remaining);

            out.push_back(Trade{
                .maker_id = maker.id,
                .taker_id = incoming.id,
                .price = level_price,          // trade prints at the maker's price
                .quantity = fill,
                .taker_side = incoming.side,
            });

            incoming.remaining -= fill;
            maker.remaining -= fill;

            if (maker.filled()) {
                index_.erase(maker.id);
                resting.pop_front();
            }
        }

        if (resting.empty()) {
            opp.erase(level_it);  // level fully consumed; drop it
        }
    }
}

void OrderBook::rest(const Order& order) {
    if (order.side == Side::Buy) {
        Level& level = bids_[order.price];
        level.push_back(order);
        index_[order.id] = Location{order.side, order.price, std::prev(level.end())};
    } else {
        Level& level = asks_[order.price];
        level.push_back(order);
        index_[order.id] = Location{order.side, order.price, std::prev(level.end())};
    }
}

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity quantity) {
    std::vector<Trade> trades;
    if (quantity == 0) {
        return trades;
    }

    Order incoming{
        .id = id,
        .side = side,
        .type = OrderType::Limit,
        .price = price,
        .quantity = quantity,
        .remaining = quantity,
        .sequence = next_sequence_++,
    };

    if (side == Side::Buy) {
        match(asks_, incoming, trades);
    } else {
        match(bids_, incoming, trades);
    }

    if (incoming.remaining > 0) {
        rest(incoming);  // remainder joins the book at the back of its level
    }
    return trades;
}

std::vector<Trade> OrderBook::add_market(OrderId id, Side side, Quantity quantity) {
    std::vector<Trade> trades;
    if (quantity == 0) {
        return trades;
    }

    // A market order crosses at any price, so seed it with the most aggressive
    // possible limit; the crosses() check then never rejects a resting level.
    const Price sweep_price =
        side == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();

    Order incoming{
        .id = id,
        .side = side,
        .type = OrderType::Market,
        .price = sweep_price,
        .quantity = quantity,
        .remaining = quantity,
        .sequence = next_sequence_++,
    };

    if (side == Side::Buy) {
        match(asks_, incoming, trades);
    } else {
        match(bids_, incoming, trades);
    }
    // Any unfilled market quantity is discarded (never rests).
    return trades;
}

bool OrderBook::cancel(OrderId id) {
    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) {
        return false;
    }

    const Location loc = idx_it->second;
    index_.erase(idx_it);

    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) {
            bids_.erase(level_it);
        }
    } else {
        auto level_it = asks_.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) {
            asks_.erase(level_it);
        }
    }
    return true;
}

std::vector<Trade> OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
    std::vector<Trade> trades;

    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) {
        return trades;  // unknown order: no-op
    }

    const Location loc = idx_it->second;
    Order& existing = *loc.it;

    if (new_quantity == 0) {
        cancel(id);
        return trades;
    }

    // Fast path: same price and a quantity reduction keeps time priority, so we
    // edit the resting order in place without touching its position in the level.
    const bool price_unchanged = (new_price == loc.price);
    const bool quantity_reduced = (new_quantity <= existing.remaining);
    if (price_unchanged && quantity_reduced) {
        existing.remaining = new_quantity;
        existing.quantity = new_quantity;
        return trades;
    }

    // Otherwise the modify loses priority: cancel and resubmit as a fresh limit,
    // which re-runs matching (a price change may now cross the spread).
    const Side side = existing.side;
    cancel(id);
    return add_limit(id, side, new_price, new_quantity);
}

void OrderBook::add_resting(OrderId id, Side side, Price price, Quantity quantity) {
    if (quantity == 0) {
        return;
    }
    Order order{
        .id = id,
        .side = side,
        .type = OrderType::Limit,
        .price = price,
        .quantity = quantity,
        .remaining = quantity,
        .sequence = next_sequence_++,
    };
    rest(order);
}

bool OrderBook::reduce(OrderId id, Quantity qty) {
    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) {
        return false;
    }
    Order& order = *idx_it->second.it;
    if (qty >= order.remaining) {
        cancel(id);  // fully consumed
    } else {
        order.remaining -= qty;
    }
    return true;
}

void OrderBook::replace(OrderId old_id, OrderId new_id, Price price, Quantity quantity) {
    auto idx_it = index_.find(old_id);
    if (idx_it == index_.end()) {
        return;  // unknown original: nothing to replace
    }
    const Side side = idx_it->second.it->side;
    cancel(old_id);
    add_resting(new_id, side, price, quantity);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    auto sum_level = [](const Level& level) {
        Quantity total = 0;
        for (const Order& o : level) {
            total += o.remaining;
        }
        return total;
    };

    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : sum_level(it->second);
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : sum_level(it->second);
}

// Explicit instantiations keep the templated match() out of the header while
// still linking for both concrete book-side map types.
template void OrderBook::match<OrderBook::AskMap>(AskMap&, Order&, std::vector<Trade>&);
template void OrderBook::match<OrderBook::BidMap>(BidMap&, Order&, std::vector<Trade>&);

}  // namespace lob
