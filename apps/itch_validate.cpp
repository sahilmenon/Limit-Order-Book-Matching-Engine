// Reconstructs a single symbol's book from a NASDAQ ITCH 5.0 file and writes a
// depth snapshot. The snapshot is cross-checked against an independent Python
// reference reconstructor (scripts/reference_reconstruct.py) to prove the C++
// engine rebuilds a real market's book correctly.
//
//   Usage: itch_validate <itch_file.bin> [TICKER] [--snapshot out.csv]
//   If TICKER is omitted, the most active symbol (most Add messages) is chosen.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/itch/itch.hpp"
#include "lob/itch/reconstructor.hpp"

namespace {

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "error: cannot open %s\n", path.c_str());
        std::exit(2);
    }
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lob;

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch_file.bin> [TICKER] [--snapshot out.csv]\n", argv[0]);
        return 2;
    }

    std::string path = argv[1];
    std::string ticker;
    std::string snapshot_path;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--snapshot" && i + 1 < argc) {
            snapshot_path = argv[++i];
        } else if (arg.rfind("--", 0) != 0) {
            ticker = arg;
        }
    }

    const std::vector<std::uint8_t> data = read_file(path);
    std::printf("Loaded %s (%.1f MB)\n", path.c_str(),
                static_cast<double>(data.size()) / (1024 * 1024));

    // First pass: directory + per-locate Add counts, so we can auto-select the
    // most active symbol and resolve a requested ticker to its locate.
    std::unordered_map<std::uint16_t, std::string> locate_to_ticker;
    std::unordered_map<std::uint16_t, std::uint64_t> adds_per_locate;
    {
        itch::MessageStream stream(data.data(), data.size());
        std::size_t len = 0;
        for (const std::uint8_t* p = stream.next(len); p; p = stream.next(len)) {
            const char t = static_cast<char>(p[0]);
            if (t == itch::StockDirectory && len >= 19) {
                locate_to_ticker.emplace(itch::be16(p + 1), std::string(itch::alpha(p + 11, 8)));
            } else if ((t == itch::AddOrder || t == itch::AddOrderMPID) && len >= 36) {
                ++adds_per_locate[itch::be16(p + 1)];
            }
        }
    }

    std::uint16_t target_locate = 0;
    if (!ticker.empty()) {
        for (const auto& [loc, sym] : locate_to_ticker) {
            if (sym == ticker) {
                target_locate = loc;
                break;
            }
        }
        if (target_locate == 0) {
            std::fprintf(stderr, "error: ticker %s not found in stock directory\n", ticker.c_str());
            return 2;
        }
    } else {
        std::uint64_t best = 0;
        for (const auto& [loc, n] : adds_per_locate) {
            // Most active wins; ties broken by smallest locate so the choice is
            // deterministic and matches the Python reference exactly.
            if (n > best || (n == best && (target_locate == 0 || loc < target_locate))) {
                best = n;
                target_locate = loc;
            }
        }
        ticker = locate_to_ticker.count(target_locate) ? locate_to_ticker[target_locate] : "?";
    }
    std::printf("Target symbol: %s (stock_locate=%u)\n", ticker.c_str(), target_locate);

    // Second pass: reconstruct the target symbol's book.
    itch::Reconstructor recon(target_locate);
    {
        itch::MessageStream stream(data.data(), data.size());
        std::size_t len = 0;
        for (const std::uint8_t* p = stream.next(len); p; p = stream.next(len)) {
            recon.apply(p, len);
        }
    }

    const auto& s = recon.stats();
    std::printf("\nMessages: %llu total | adds %llu, exec %llu, cancel %llu, delete %llu, replace %llu\n",
                (unsigned long long)s.total, (unsigned long long)s.adds,
                (unsigned long long)s.executes, (unsigned long long)s.cancels,
                (unsigned long long)s.deletes, (unsigned long long)s.replaces);
    std::printf("Applied to %s book: %llu | resting orders: %zu\n", ticker.c_str(),
                (unsigned long long)s.applied, recon.book().order_count());

    const auto& book = recon.book();
    if (book.best_bid() && book.best_ask()) {
        std::printf("Top of book: bid %lld x%llu | ask %lld x%llu | spread %lld\n",
                    (long long)*book.best_bid(),
                    (unsigned long long)book.quantity_at(Side::Buy, *book.best_bid()),
                    (long long)*book.best_ask(),
                    (unsigned long long)book.quantity_at(Side::Sell, *book.best_ask()),
                    (long long)(*book.best_ask() - *book.best_bid()));
    }

    if (!snapshot_path.empty()) {
        std::ofstream out(snapshot_path, std::ios::binary);
        // Bids high->low, then asks low->high. Aggregate quantity per price level.
        for (const auto& [price, level] : book.bids()) {
            Quantity q = 0;
            for (const auto& o : level) q += o.remaining;
            out << "B," << price << ',' << q << '\n';
        }
        for (const auto& [price, level] : book.asks()) {
            Quantity q = 0;
            for (const auto& o : level) q += o.remaining;
            out << "S," << price << ',' << q << '\n';
        }
        std::printf("Wrote snapshot: %s\n", snapshot_path.c_str());
    }
    return 0;
}
