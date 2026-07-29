#pragma once

#include "lob/types.hpp"

namespace lob {

// A single execution produced by matching. Trades always print at the resting
// (maker) order's price, which is standard price-time-priority behaviour: the
// order that was on the book first sets the price, the incoming (taker) order
// crosses the spread to it.
struct Trade {
    OrderId maker_id{};   // resting order that was matched against
    OrderId taker_id{};   // incoming order that initiated the match
    Price price{};        // execution price (the maker's limit price)
    Quantity quantity{};  // quantity exchanged in this execution
    Side taker_side{};    // side of the aggressor
};

}  // namespace lob
