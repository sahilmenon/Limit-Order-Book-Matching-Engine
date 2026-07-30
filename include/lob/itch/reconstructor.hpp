#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "lob/itch/itch.hpp"
#include "lob/order_book.hpp"

namespace lob::itch {

// Rebuilds the displayed order book for a single symbol from an ITCH 5.0 stream.
//
// ITCH states executions, cancels, deletes and replaces explicitly, so this is
// pure event application onto the book's maintenance API, never matching. Only
// messages for the target stock locate affect the book; order-reference messages
// (E/C/X/D/U) carry no symbol, so we simply apply them and rely on the fact that
// order references are unique per day: a reference we never added is not ours.
class Reconstructor {
public:
    struct Stats {
        std::uint64_t total = 0;
        std::uint64_t adds = 0;
        std::uint64_t executes = 0;
        std::uint64_t cancels = 0;
        std::uint64_t deletes = 0;
        std::uint64_t replaces = 0;
        std::uint64_t applied = 0;  // messages that touched the target book
    };

    // `target_locate` selects the symbol (its ITCH stock-locate code).
    explicit Reconstructor(std::uint16_t target_locate) : target_locate_(target_locate) {}

    // Apply one framed message payload (payload[0] is the type character).
    void apply(const std::uint8_t* p, std::size_t len);

    [[nodiscard]] const OrderBook& book() const { return book_; }
    [[nodiscard]] const Stats& stats() const { return stats_; }

    // Directory of stock-locate -> ticker, populated from Stock Directory (R)
    // messages; useful for resolving a ticker to its locate before replay.
    [[nodiscard]] const std::unordered_map<std::uint16_t, std::string>& directory() const {
        return directory_;
    }

private:
    std::uint16_t target_locate_;
    OrderBook book_;
    Stats stats_;
    std::unordered_map<std::uint16_t, std::string> directory_;
};

// Scans only Stock Directory (R) messages to build a ticker -> locate map,
// without reconstructing any book. Used to resolve a requested ticker.
std::unordered_map<std::string, std::uint16_t> scan_symbol_directory(const std::uint8_t* data,
                                                                     std::size_t size);

}  // namespace lob::itch
