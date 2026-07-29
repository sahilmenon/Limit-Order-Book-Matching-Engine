#pragma once

#include "lob/types.hpp"

namespace lob {

// A resting or incoming order. In this naive (Milestone 1) implementation the
// Order lives inside a std::list node at its price level; the latency rewrite
// (Milestone 3) will embed intrusive next/prev pointers here instead.
struct Order {
    OrderId id{};
    Side side{};
    OrderType type{};
    Price price{};          // limit price; ignored for Market orders
    Quantity quantity{};    // original quantity submitted
    Quantity remaining{};   // quantity still open (0 == fully filled)
    Sequence sequence{};    // arrival order, assigned when the order rests

    [[nodiscard]] bool filled() const noexcept { return remaining == 0; }
};

}  // namespace lob
