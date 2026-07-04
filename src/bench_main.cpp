// Matching-engine microbenchmark: throughput + per-order latency.
//
// Pre-generates a realistic message mix (new limit/IOC orders around a mid,
// plus cancels of recently-rested orders), then measures:
//   * throughput -- total orders / wall time for the whole batch,
//   * latency    -- per-submit timing, reported as a percentile histogram.
// Messages are built up front so RNG cost is excluded from the timed sections.
// Sinks are left unset so we measure the engine core, not report formatting.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "arena/order_book.hpp"

using namespace arena;

int main(int argc, char** argv) {
    int n = 2'000'000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--n" && i + 1 < argc) n = std::atoi(argv[++i]);
    }
    if (n <= 0) { std::fprintf(stderr, "bench: --n must be positive\n"); return 2; }

    // --- pre-generate the message stream -------------------------------------
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<Price> pd(400, 600);   // prices around a mid
    std::uniform_int_distribution<Qty>   qd(1, 10);
    std::uniform_int_distribution<int>   sd(0, 1);
    std::uniform_int_distribution<int>   op(0, 9);

    std::vector<wire::InboundOrder> msgs;
    msgs.reserve(n);
    std::vector<OrderId> recent;     // ring of recently rested ids to cancel
    recent.reserve(4096);
    std::size_t ri = 0;
    OrderId next_id = 1;

    for (int i = 0; i < n; ++i) {
        if (op(rng) < 3 && !recent.empty()) {           // ~30% cancels
            const OrderId id = recent[rng() % recent.size()];
            msgs.push_back({wire::MsgType::Cancel, 0, id, Side::Bid, 0, 0, Tif::GTC});
        } else {                                        // ~70% new orders
            const Side side = sd(rng) ? Side::Ask : Side::Bid;
            const Tif  tif  = (op(rng) < 2) ? Tif::IOC : Tif::GTC;  // ~20% IOC (trade)
            const OrderId id = next_id++;
            msgs.push_back({wire::MsgType::New, 0, id, side, pd(rng), qd(rng), tif});
            if (recent.size() < 4096) recent.push_back(id);
            else recent[ri++ & 4095] = id;
        }
    }

    using clock = std::chrono::steady_clock;

    // --- throughput ----------------------------------------------------------
    {
        MatchingEngine eng(1 << 20);
        const auto t0 = clock::now();
        for (const auto& m : msgs) eng.submit(1, m);
        const auto t1 = clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        std::printf("Throughput\n");
        std::printf("  orders ......... %d\n", n);
        std::printf("  wall ........... %.3f s\n", sec);
        std::printf("  rate ........... %.2f M orders/sec\n", n / sec / 1e6);
        std::printf("  per order ...... %.1f ns\n", sec / n * 1e9);
        std::printf("  resting at end . %zu\n", eng.resting_orders());
    }

    // --- latency (fresh engine, time each submit) ----------------------------
    {
        MatchingEngine eng(1 << 20);
        std::vector<std::uint32_t> lat;
        lat.reserve(n);
        for (const auto& m : msgs) {
            const auto a = clock::now();
            eng.submit(1, m);
            const auto b = clock::now();
            lat.push_back(static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
        }
        std::sort(lat.begin(), lat.end());
        auto pct = [&](double p) {
            std::size_t idx = static_cast<std::size_t>(p / 100.0 * (lat.size() - 1));
            return lat[idx];
        };
        std::printf("\nLatency (ns/order)\n");
        std::printf("  p50 ............ %u\n", pct(50));
        std::printf("  p90 ............ %u\n", pct(90));
        std::printf("  p99 ............ %u\n", pct(99));
        std::printf("  p99.9 .......... %u\n", pct(99.9));
        std::printf("  max ............ %u\n", lat.back());
    }
    return 0;
}
