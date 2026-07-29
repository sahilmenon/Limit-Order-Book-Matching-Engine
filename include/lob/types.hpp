#pragma once

#include <cstdint>

namespace lob {

// Prices are integer ticks (e.g. cents), never floating point. Exchanges quote
// on a discrete tick grid; using an integer type makes price comparison exact
// and equality well-defined, which matters for price-time priority.
using Price = std::int64_t;

// Quantities and identifiers. Unsigned because they are never negative; a fill
// can reduce a quantity to zero but never below it.
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;

// Monotonic arrival sequence. Within a price level, the order with the smaller
// sequence has time priority (arrived first).
using Sequence = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class OrderType : std::uint8_t {
    Limit,   // rests on the book at a stated price if not fully matched
    Market,  // matches at any price; never rests, remainder is discarded
};

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace lob
