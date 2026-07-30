#include "lob/itch/reconstructor.hpp"

namespace lob::itch {

// Field offsets follow the Nasdaq TotalView-ITCH 5.0 spec. Offset 0 is the
// message-type character; stock_locate is always at bytes 1-2.

void Reconstructor::apply(const std::uint8_t* p, std::size_t len) {
    ++stats_.total;
    const char type = static_cast<char>(p[0]);

    switch (type) {
        case StockDirectory: {  // R: build the locate -> ticker directory
            if (len < 19) return;
            const std::uint16_t locate = be16(p + 1);
            directory_.emplace(locate, std::string(alpha(p + 11, 8)));
            return;
        }

        case AddOrder:       // A
        case AddOrderMPID: { // F (identical layout for the first 36 bytes)
            ++stats_.adds;
            if (be16(p + 1) != target_locate_) return;
            const OrderId ref = be64(p + 11);
            const Side side = side_from_itch(p[19]);
            const Quantity shares = be32(p + 20);
            const Price price = static_cast<Price>(be32(p + 32));
            book_.add_resting(ref, side, price, shares);
            ++stats_.applied;
            return;
        }

        case OrderExecuted: {  // E: reduce by executed shares
            ++stats_.executes;
            const OrderId ref = be64(p + 11);
            const Quantity shares = be32(p + 19);
            if (book_.reduce(ref, shares)) ++stats_.applied;
            return;
        }

        case OrderExecutedWithPrice: {  // C: reduce by executed shares (price aside)
            ++stats_.executes;
            const OrderId ref = be64(p + 11);
            const Quantity shares = be32(p + 19);
            if (book_.reduce(ref, shares)) ++stats_.applied;
            return;
        }

        case OrderCancel: {  // X: reduce by cancelled shares
            ++stats_.cancels;
            const OrderId ref = be64(p + 11);
            const Quantity shares = be32(p + 19);
            if (book_.reduce(ref, shares)) ++stats_.applied;
            return;
        }

        case OrderDelete: {  // D: remove the order
            ++stats_.deletes;
            const OrderId ref = be64(p + 11);
            if (book_.remove(ref)) ++stats_.applied;
            return;
        }

        case OrderReplace: {  // U: delete original, add replacement
            ++stats_.replaces;
            const OrderId orig = be64(p + 11);
            const OrderId repl = be64(p + 19);
            const Quantity shares = be32(p + 27);
            const Price price = static_cast<Price>(be32(p + 31));
            if (book_.contains(orig)) {
                book_.replace(orig, repl, price, shares);
                ++stats_.applied;
            }
            return;
        }

        default:
            return;  // messages that do not affect the displayed book
    }
}

std::unordered_map<std::string, std::uint16_t> scan_symbol_directory(const std::uint8_t* data,
                                                                     std::size_t size) {
    std::unordered_map<std::string, std::uint16_t> out;
    MessageStream stream(data, size);
    std::size_t len = 0;
    for (const std::uint8_t* p = stream.next(len); p != nullptr; p = stream.next(len)) {
        if (static_cast<char>(p[0]) == StockDirectory && len >= 19) {
            out.emplace(std::string(alpha(p + 11, 8)), be16(p + 1));
        }
    }
    return out;
}

}  // namespace lob::itch
