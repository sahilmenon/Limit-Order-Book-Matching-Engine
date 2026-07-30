// Self-contained validation of the ITCH 5.0 reconstructor. Rather than depend on
// a multi-GB market-data download, these tests synthesise byte-accurate ITCH 5.0
// messages (field offsets straight from the Nasdaq TotalView-ITCH 5.0 spec) and
// replay them through the Reconstructor, asserting the rebuilt book. Because the
// bytes are built to the published spec, agreement here is real evidence the
// decoder handles the wire format correctly — the same decoder then drives the
// itch_validate tool against genuine feeds.

#include "lob/itch/itch.hpp"
#include "lob/itch/reconstructor.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace lob;
using namespace lob::itch;

namespace {

// Builds a BinaryFILE-framed ITCH stream: each appended message is prefixed with
// its 2-byte big-endian length, exactly as the parser (MessageStream) expects.
class StreamBuilder {
public:
    // Append one already-serialised message payload (payload[0] is the type char).
    void frame(const std::vector<std::uint8_t>& msg) {
        const std::size_t n = msg.size();
        buf_.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>(n & 0xFF));
        buf_.insert(buf_.end(), msg.begin(), msg.end());
    }

    // Append raw bytes verbatim (used to inject a deliberately truncated tail).
    void raw(const std::vector<std::uint8_t>& bytes) {
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] const std::uint8_t* data() const { return buf_.data(); }
    [[nodiscard]] std::size_t size() const { return buf_.size(); }

private:
    std::vector<std::uint8_t> buf_;
};

// --- Big-endian field writers (mirror the be16/be32/be64 readers) -----------

void put16(std::vector<std::uint8_t>& m, std::uint16_t v) {
    m.push_back(static_cast<std::uint8_t>(v >> 8));
    m.push_back(static_cast<std::uint8_t>(v));
}
void put32(std::vector<std::uint8_t>& m, std::uint32_t v) {
    m.push_back(static_cast<std::uint8_t>(v >> 24));
    m.push_back(static_cast<std::uint8_t>(v >> 16));
    m.push_back(static_cast<std::uint8_t>(v >> 8));
    m.push_back(static_cast<std::uint8_t>(v));
}
void put48(std::vector<std::uint8_t>& m, std::uint64_t v) {
    for (int shift = 40; shift >= 0; shift -= 8) m.push_back(static_cast<std::uint8_t>(v >> shift));
}
void put64(std::vector<std::uint8_t>& m, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) m.push_back(static_cast<std::uint8_t>(v >> shift));
}
// 8-char stock symbol, space-padded to width (ITCH pads alpha fields with spaces).
void put_stock(std::vector<std::uint8_t>& m, const std::string& sym) {
    for (std::size_t i = 0; i < 8; ++i)
        m.push_back(i < sym.size() ? static_cast<std::uint8_t>(sym[i]) : ' ');
}

// --- Message builders (offsets per Nasdaq TotalView-ITCH 5.0) ---------------

std::vector<std::uint8_t> stock_directory(std::uint16_t locate, const std::string& sym) {
    std::vector<std::uint8_t> m;
    m.push_back('R');            // 0: type
    put16(m, locate);           // 1: stock locate
    put16(m, 0);                // 3: tracking number
    put48(m, 0);                // 5: timestamp
    put_stock(m, sym);          // 11: stock (8)
    m.push_back('N');           // 19: market category
    m.push_back('N');           // 20: financial status
    put32(m, 100);              // 21: round lot size
    m.push_back('N');           // 25: round lots only
    // Remaining directory fields are irrelevant to reconstruction; pad to spec len.
    while (m.size() < 39) m.push_back(' ');
    return m;
}

std::vector<std::uint8_t> add_order(std::uint16_t locate, std::uint64_t ref, char side,
                                    std::uint32_t shares, std::uint32_t price) {
    std::vector<std::uint8_t> m;
    m.push_back('A');            // 0
    put16(m, locate);           // 1
    put16(m, 0);                // 3: tracking
    put48(m, 0);                // 5: timestamp
    put64(m, ref);              // 11: order ref
    m.push_back(static_cast<std::uint8_t>(side));  // 19: buy/sell
    put32(m, shares);           // 20: shares
    put_stock(m, "TEST");       // 24: stock (8)
    put32(m, price);            // 32: price
    return m;                   // len 36
}

std::vector<std::uint8_t> order_executed(std::uint64_t ref, std::uint32_t shares) {
    std::vector<std::uint8_t> m;
    m.push_back('E');
    put16(m, 0);                // locate (unused by E path)
    put16(m, 0);                // tracking
    put48(m, 0);                // timestamp
    put64(m, ref);              // 11
    put32(m, shares);           // 19: executed shares
    put64(m, 0);                // 23: match number
    return m;                   // len 31
}

std::vector<std::uint8_t> order_cancel(std::uint64_t ref, std::uint32_t shares) {
    std::vector<std::uint8_t> m;
    m.push_back('X');
    put16(m, 0);
    put16(m, 0);
    put48(m, 0);
    put64(m, ref);              // 11
    put32(m, shares);           // 19: cancelled shares
    return m;                   // len 23
}

std::vector<std::uint8_t> order_delete(std::uint64_t ref) {
    std::vector<std::uint8_t> m;
    m.push_back('D');
    put16(m, 0);
    put16(m, 0);
    put48(m, 0);
    put64(m, ref);              // 11
    return m;                   // len 19
}

std::vector<std::uint8_t> order_replace(std::uint64_t orig, std::uint64_t repl,
                                        std::uint32_t shares, std::uint32_t price) {
    std::vector<std::uint8_t> m;
    m.push_back('U');
    put16(m, 0);
    put16(m, 0);
    put48(m, 0);
    put64(m, orig);             // 11: original ref
    put64(m, repl);             // 19: new ref
    put32(m, shares);           // 27: shares
    put32(m, price);            // 31: price
    return m;                   // len 35
}

// Replay every framed message in `sb` through a reconstructor for `locate`.
Reconstructor replay(const StreamBuilder& sb, std::uint16_t locate) {
    Reconstructor recon(locate);
    MessageStream stream(sb.data(), sb.size());
    std::size_t len = 0;
    for (const std::uint8_t* p = stream.next(len); p; p = stream.next(len)) recon.apply(p, len);
    return recon;
}

constexpr std::uint16_t kLocate = 42;

}  // namespace

TEST(ItchReconstructor, AddsBuildTopOfBook) {
    StreamBuilder sb;
    sb.frame(stock_directory(kLocate, "TEST"));
    sb.frame(add_order(kLocate, 1, 'B', 100, 9900));
    sb.frame(add_order(kLocate, 2, 'B', 200, 9800));
    sb.frame(add_order(kLocate, 3, 'S', 150, 10100));

    Reconstructor recon = replay(sb, kLocate);
    const OrderBook& book = recon.book();

    ASSERT_TRUE(book.best_bid().has_value());
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(*book.best_bid(), 9900);
    EXPECT_EQ(*book.best_ask(), 10100);
    EXPECT_EQ(book.quantity_at(Side::Buy, 9900), 100u);
    EXPECT_EQ(book.quantity_at(Side::Buy, 9800), 200u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 10100), 150u);
    EXPECT_EQ(book.order_count(), 3u);
}

TEST(ItchReconstructor, ExecutePartiallyThenFullyRemovesOrder) {
    StreamBuilder sb;
    sb.frame(add_order(kLocate, 1, 'S', 500, 10000));
    sb.frame(order_executed(1, 200));  // 300 left
    Reconstructor r1 = replay(sb, kLocate);
    EXPECT_EQ(r1.book().quantity_at(Side::Sell, 10000), 300u);
    EXPECT_TRUE(r1.book().contains(1));

    sb.frame(order_executed(1, 300));  // fully executed -> gone
    Reconstructor r2 = replay(sb, kLocate);
    EXPECT_FALSE(r2.book().contains(1));
    EXPECT_TRUE(r2.book().empty());
}

TEST(ItchReconstructor, CancelReducesAndDeleteRemoves) {
    StreamBuilder sb;
    sb.frame(add_order(kLocate, 1, 'B', 100, 9900));
    sb.frame(add_order(kLocate, 2, 'B', 100, 9900));
    sb.frame(order_cancel(1, 40));   // order 1 -> 60
    sb.frame(order_delete(2));       // order 2 gone
    Reconstructor recon = replay(sb, kLocate);

    EXPECT_EQ(recon.book().quantity_at(Side::Buy, 9900), 60u);
    EXPECT_TRUE(recon.book().contains(1));
    EXPECT_FALSE(recon.book().contains(2));
}

TEST(ItchReconstructor, ReplaceMovesOrderToNewPriceAndRef) {
    StreamBuilder sb;
    sb.frame(add_order(kLocate, 1, 'B', 100, 9900));
    sb.frame(order_replace(1, 2, 80, 9950));  // reprice up, new ref, new qty
    Reconstructor recon = replay(sb, kLocate);

    EXPECT_FALSE(recon.book().contains(1));
    EXPECT_TRUE(recon.book().contains(2));
    EXPECT_EQ(recon.book().quantity_at(Side::Buy, 9900), 0u);
    EXPECT_EQ(recon.book().quantity_at(Side::Buy, 9950), 80u);
    EXPECT_EQ(*recon.book().best_bid(), 9950);
}

TEST(ItchReconstructor, IgnoresMessagesForOtherSymbols) {
    StreamBuilder sb;
    sb.frame(add_order(kLocate, 1, 'B', 100, 9900));       // ours
    sb.frame(add_order(kLocate + 1, 2, 'B', 999, 9900));   // different symbol
    Reconstructor recon = replay(sb, kLocate);

    // Only our add affects the book; the foreign add is counted but not applied.
    EXPECT_EQ(recon.book().order_count(), 1u);
    EXPECT_EQ(recon.book().quantity_at(Side::Buy, 9900), 100u);
    EXPECT_FALSE(recon.book().contains(2));
    EXPECT_EQ(recon.stats().adds, 2u);
    EXPECT_EQ(recon.stats().applied, 1u);
}

TEST(ItchReconstructor, StatsCountEveryMessageType) {
    StreamBuilder sb;
    sb.frame(add_order(kLocate, 1, 'B', 100, 9900));
    sb.frame(add_order(kLocate, 2, 'S', 100, 10100));
    sb.frame(order_executed(1, 50));
    sb.frame(order_cancel(2, 50));
    sb.frame(order_delete(1));
    sb.frame(order_replace(2, 3, 50, 10050));
    Reconstructor recon = replay(sb, kLocate);

    const auto& s = recon.stats();
    EXPECT_EQ(s.adds, 2u);
    EXPECT_EQ(s.executes, 1u);
    EXPECT_EQ(s.cancels, 1u);
    EXPECT_EQ(s.deletes, 1u);
    EXPECT_EQ(s.replaces, 1u);
    EXPECT_EQ(s.total, 6u);
}

TEST(ItchReconstructor, TruncatedTailFrameStopsCleanly) {
    StreamBuilder sb;
    sb.frame(add_order(kLocate, 1, 'B', 100, 9900));
    // A length prefix promising 36 bytes but only 4 present: a range-truncated
    // download. The parser must stop, not read past the buffer.
    sb.raw({0x00, 0x24, 'A', 0x00, 0x00, 0x01});
    Reconstructor recon = replay(sb, kLocate);

    EXPECT_EQ(recon.book().order_count(), 1u);  // only the complete first message applied
    EXPECT_EQ(recon.stats().total, 1u);
}

TEST(ItchReconstructor, ScanSymbolDirectoryResolvesTickerToLocate) {
    StreamBuilder sb;
    sb.frame(stock_directory(7, "AAPL"));
    sb.frame(stock_directory(9, "MSFT"));
    sb.frame(add_order(7, 1, 'B', 100, 9900));

    auto dir = scan_symbol_directory(sb.data(), sb.size());
    ASSERT_EQ(dir.count("AAPL"), 1u);
    ASSERT_EQ(dir.count("MSFT"), 1u);
    EXPECT_EQ(dir["AAPL"], 7u);
    EXPECT_EQ(dir["MSFT"], 9u);
}
