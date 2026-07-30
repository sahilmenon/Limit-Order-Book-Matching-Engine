#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include "lob/types.hpp"

// Minimal decoder for NASDAQ TotalView-ITCH 5.0 in the public "BinaryFILE"
// framing: the stream is a sequence of [2-byte big-endian length][payload],
// where payload[0] is the message-type character. All multi-byte integer fields
// are big-endian. Prices are 4-byte unsigned fixed-point with 4 implied decimal
// places (i.e. price/10000 dollars); we keep the raw integer as our tick price.
//
// Reference: Nasdaq TotalView-ITCH 5.0 specification.
namespace lob::itch {

// --- Big-endian field readers ----------------------------------------------
// `p` points at the first byte of the field; width is fixed by the ITCH spec.

inline std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0]) << 8 | p[1];
}

inline std::uint32_t be32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

// 6-byte big-endian timestamp (nanoseconds since midnight), widened to 64 bits.
inline std::uint64_t be48(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
    return v;
}

inline std::uint64_t be64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// Fixed-width, space-padded alphanumeric field (e.g. an 8-char ticker), trimmed
// of trailing spaces.
inline std::string_view alpha(const std::uint8_t* p, std::size_t width) noexcept {
    std::size_t end = width;
    while (end > 0 && p[end - 1] == ' ') --end;
    return std::string_view(reinterpret_cast<const char*>(p), end);
}

// --- Message types we act on ------------------------------------------------
enum : char {
    SystemEvent = 'S',
    StockDirectory = 'R',
    AddOrder = 'A',
    AddOrderMPID = 'F',
    OrderExecuted = 'E',
    OrderExecutedWithPrice = 'C',
    OrderCancel = 'X',
    OrderDelete = 'D',
    OrderReplace = 'U',
};

inline Side side_from_itch(std::uint8_t buy_sell) noexcept {
    return buy_sell == 'B' ? Side::Buy : Side::Sell;
}

// --- Message stream ---------------------------------------------------------
// Walks a contiguous buffer of BinaryFILE-framed messages. A trailing partial
// message (from a range-truncated download) is detected and stops iteration
// cleanly rather than reading out of bounds.
class MessageStream {
public:
    MessageStream(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    // Returns a pointer to the next message payload and sets `len`, or nullptr
    // when the stream is exhausted (or only a partial frame remains).
    const std::uint8_t* next(std::size_t& len) noexcept {
        if (pos_ + 2 > size_) return nullptr;                  // no room for length prefix
        const std::size_t msg_len = be16(data_ + pos_);
        if (msg_len == 0 || pos_ + 2 + msg_len > size_) return nullptr;  // truncated tail
        const std::uint8_t* payload = data_ + pos_ + 2;
        pos_ += 2 + msg_len;
        len = msg_len;
        return payload;
    }

    [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_{0};
};

}  // namespace lob::itch
