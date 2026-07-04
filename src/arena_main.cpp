// Wires the matching engine, accounting and reference bots together, runs a
// fixed-horizon game on a hidden terminal value V, settles P&L and prints a
// leaderboard. Pass --cases to run the engine demo/smoke cases instead.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

#include "arena/order_book.hpp"
#include "arena/reference_bots.hpp"
#include "arena/wal.hpp"

using namespace arena;

namespace {

struct BotResult {
    std::string  name;
    std::int32_t position;
    std::int64_t cash;
    std::int64_t pnl;
};

struct GameResult {
    Price                  value = 0;
    std::uint64_t          volume = 0;    // total lots traded
    std::uint64_t          messages = 0;  // outbound exchange_seq reached
    std::vector<BotResult> bots;
};

class Arena final : public OrderEntry {
    public:
    Arena(std::uint64_t seed, int rounds) : engine_(1 << 16), rounds_(rounds) {
        std::mt19937_64 master(seed);

        // Hidden terminal value, centered in the tick range.
        std::uniform_int_distribution<Price> v_dist(450, 550);
        value_ = v_dist(master);

        // Each bot gets a noisy, independent estimate of V.
        std::uniform_int_distribution<Price> noise(-8, 8);
        auto est = [&] { return clamp_px(value_ + noise(master)); };

        add<NoiseBot>(master(), est());
        add<NoiseBot>(master(), est());
        add<NaiveMM>(est());
        add<InventoryMM>(est());
        add<SniperBot>(est());

        cash_.assign(bots_.size(), 0);
        pos_.assign(bots_.size(), 0);

        engine_.set_report_sink([this](BotId owner, const wire::ExecReport& r) {
            digest_.update(r);
            on_report(owner, r);
        });
        engine_.set_market_sink([this](const wire::MarketUpdate& md) {
            digest_.update(md);
            for (auto& b : bots_) b->on_market_data(md);
        });
    }

    // OrderEntry: route a bot's message into the engine, logging it first if
    // recording (write-ahead: the log gets the event before the engine does).
    void send(BotId owner, const wire::InboundOrder& msg) override {
        if (wal_) wal_->append(owner, msg);
        engine_.submit(owner, msg);
    }

    // Replay path: feed one recorded event straight to the engine (bypass bots).
    void feed(const WalRecord& rec) { engine_.submit(rec.owner, rec.msg); }

    void set_wal(WalWriter* w) { wal_ = w; }
    std::uint16_t num_bots() const { return static_cast<std::uint16_t>(bots_.size()); }
    std::uint64_t digest() const { return digest_.h; }

    GameResult build_result() const {
        GameResult res;
        res.value = value_;
        res.volume = buy_volume_;  // == sell_volume_ by conservation
        res.messages = engine_.exchange_seq();
        for (std::size_t i = 0; i < bots_.size(); ++i) {
            // Settle open inventory at the revealed terminal value V.
            const std::int64_t pnl =
                cash_[i] + static_cast<std::int64_t>(pos_[i]) * value_;
            res.bots.push_back({bots_[i]->name(), pos_[i], cash_[i], pnl});
        }
        return res;
    }

    GameResult run() {
        for (int r = 0; r < rounds_; ++r)
            for (auto& b : bots_) b->act();
        return build_result();
    }

    Price value() const { return value_; }
    std::uint64_t buy_volume() const { return buy_volume_; }
    std::uint64_t sell_volume() const { return sell_volume_; }
    bool crossed() const { return engine_.crossed(); }

    private:
    template <class T, class... Args>
    void add(Args&&... args) {
        const BotId id = static_cast<BotId>(bots_.size());
        bots_.push_back(std::make_unique<T>(id, this, std::forward<Args>(args)...));
    }

    void on_report(BotId owner, const wire::ExecReport& r) {
        if (r.type == wire::ReportType::Fill) {
            const std::int64_t notional = static_cast<std::int64_t>(r.price) * r.qty;
            if (r.side == Side::Bid) {
                cash_[owner] -= notional;
                pos_[owner]  += static_cast<std::int32_t>(r.qty);
                buy_volume_  += r.qty;
            } else {
                cash_[owner] += notional;
                pos_[owner]  -= static_cast<std::int32_t>(r.qty);
                sell_volume_ += r.qty;
            }
        }
        bots_[owner]->deliver_execution(r);
    }

    MatchingEngine engine_;
    int rounds_;
    Price value_ = 0;
    std::vector<std::unique_ptr<Bot>> bots_;
    std::vector<std::int64_t> cash_;
    std::vector<std::int32_t> pos_;
    std::uint64_t buy_volume_ = 0;
    std::uint64_t sell_volume_ = 0;
    WalWriter* wal_ = nullptr;
    Digest     digest_;
};

GameResult play(std::uint64_t seed, int rounds) {
    Arena a(seed, rounds);
    return a.run();
}

// Run many games on consecutive seeds; print each bot's mean P&L, stddev, and a
// Sharpe-like mean/stddev ratio 
void run_aggregate(std::uint64_t seed, int rounds, const GameResult& sample) {
    const int n_games = 200;
    const std::size_t nb = sample.bots.size();
    std::vector<double> sum(nb, 0.0), sumsq(nb, 0.0);
    for (int g = 0; g < n_games; ++g) {
        GameResult gr = play(seed + static_cast<std::uint64_t>(g), rounds);
        for (std::size_t i = 0; i < nb; ++i) {
            const double p = static_cast<double>(gr.bots[i].pnl);
            sum[i] += p;
            sumsq[i] += p * p;
        }
    }
    std::vector<int> agg(nb);
    for (std::size_t i = 0; i < nb; ++i) agg[i] = static_cast<int>(i);
    auto mean = [&](int i) { return sum[i] / n_games; };
    std::sort(agg.begin(), agg.end(), [&](int x, int y) { return mean(x) > mean(y); });

    std::printf("\nAggregate over %d games (seeds %llu..%llu)\n", n_games,
                static_cast<unsigned long long>(seed),
                static_cast<unsigned long long>(seed + n_games - 1));
    std::printf("------------------------------------------------------------\n");
    std::printf("%-4s %-10s %12s %12s %8s\n", "rank", "bot", "mean_pnl", "stddev", "sharpe");
    int rank = 1;
    for (int i : agg) {
        const double m = mean(i);
        const double var = sumsq[i] / n_games - m * m;
        const double sd = var > 0 ? std::sqrt(var) : 0.0;
        const double sharpe = sd > 0 ? m / sd : 0.0;
        std::printf("%-4d %-10s %12.1f %12.1f %8.2f\n", rank++,
                    sample.bots[i].name.c_str(), m, sd, sharpe);
    }
}

void print_leaderboard(const GameResult& res);  // defined below

// Replay a recorded game from its WAL and verify byte-identical output.
int replay(const std::string& path) {
    WalReader wal(path);
    Arena a(wal.header.seed, 0);  // rebuild bots/V from the seed; don't run them
    for (const auto& rec : wal.records) a.feed(rec);
    GameResult res = a.build_result();
    const bool digest_ok = a.digest() == wal.header.out_digest;
    const bool value_ok = a.value() == wal.header.value;

    std::printf("Replay of %s\n", path.c_str());
    std::printf("------------------------------------------------------------\n");
    std::printf("seed .................... %llu\n",
                static_cast<unsigned long long>(wal.header.seed));
    std::printf("records replayed ........ %llu\n",
                static_cast<unsigned long long>(wal.header.record_count));
    std::printf("Hidden value V .......... %d  (matches log: %s)\n", res.value,
                value_ok ? "ok" : "FAIL");
    std::printf("Outbound digest ......... 0x%016llx (expected 0x%016llx)\n",
                static_cast<unsigned long long>(a.digest()),
                static_cast<unsigned long long>(wal.header.out_digest));
    std::printf("Replay byte-identical ... %s\n", digest_ok ? "ok" : "FAIL");
    std::printf("------------------------------------------------------------\n");
    print_leaderboard(res);
    return (digest_ok && value_ok) ? 0 : 1;
}

// Print the per-bot leaderboard, sorted by P&L descending.
void print_leaderboard(const GameResult& res) {
    std::vector<int> order(res.bots.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int x, int y) {
        return res.bots[x].pnl > res.bots[y].pnl;
    });
    std::printf("%-4s %-10s %10s %12s %12s\n", "rank", "bot", "position", "cash", "pnl");
    int rank = 1;
    for (int i : order) {
        const auto& b = res.bots[i];
        std::printf("%-4d %-10s %10d %12lld %12lld\n", rank++, b.name.c_str(),
                    b.position, static_cast<long long>(b.cash),
                    static_cast<long long>(b.pnl));
    }
}

// ---------------------------------------------------------------------------
// Engine demo/smoke cases (run with --cases).
MatchingEngine make_engine() {
    MatchingEngine eng(1 << 16);
    eng.set_report_sink([](BotId owner, const wire::ExecReport& r) {
        static const char* kType[] = {"?", "Ack", "Fill", "Cancelled", "Reject"};
        printf("  report: owner=%u %-9s seq=%llu id=%llu side=%d px=%d qty=%u\n",
               owner, kType[static_cast<int>(r.type)],
               static_cast<unsigned long long>(r.exchange_seq),
               static_cast<unsigned long long>(r.order_id),
               static_cast<int>(r.side), r.price, r.qty);
    });
    return eng;
}

void demo_cases() {
    printf("== case 1: market buy, thin book ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(8, {wire::MsgType::New, 1, 200, Side::Ask, 505, 3, Tif::GTC});
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, kNoPrice, 5, Tif::GTC});
        printf("  best_bid=%d best_ask=%d\n", eng.best_bid(), eng.best_ask());
    }
    printf("== case 2: IOC buy, partial fill ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(8, {wire::MsgType::New, 1, 200, Side::Ask, 505, 2, Tif::GTC});
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, 505, 5, Tif::IOC});
        printf("  best_bid=%d best_ask=%d\n", eng.best_bid(), eng.best_ask());
    }
    printf("== case 3: IOC buy, no fill ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, 400, 4, Tif::IOC});
        printf("  best_bid=%d best_ask=%d\n", eng.best_bid(), eng.best_ask());
    }
    printf("== case 4: cancel best bid ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, 500, 5, Tif::GTC});
        eng.submit(7, {wire::MsgType::New, 2, 101, Side::Bid, 499, 4, Tif::GTC});
        printf("  before cancel: best_bid=%d resting=%zu\n", eng.best_bid(), eng.resting_orders());
        eng.submit(7, {wire::MsgType::Cancel, 3, 100, Side::Bid, 0, 0, Tif::GTC});
        printf("  after cancel:  best_bid=%d resting=%zu\n", eng.best_bid(), eng.resting_orders());
    }
    printf("== case 5: cancel unknown id ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(7, {wire::MsgType::Cancel, 1, 999, Side::Bid, 0, 0, Tif::GTC});
    }
    printf("== case 6: modify reprice ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, 500, 5, Tif::GTC});
        printf("  before: best_bid=%d resting=%zu\n", eng.best_bid(), eng.resting_orders());
        eng.submit(7, {wire::MsgType::Modify, 2, 100, Side::Bid, 502, 5, Tif::GTC});
        printf("  after:  best_bid=%d resting=%zu\n", eng.best_bid(), eng.resting_orders());
    }
    printf("== case 7: modify that crosses ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(8, {wire::MsgType::New, 1, 200, Side::Ask, 505, 5, Tif::GTC});
        eng.submit(7, {wire::MsgType::New, 2, 100, Side::Bid, 500, 5, Tif::GTC});
        eng.submit(7, {wire::MsgType::Modify, 3, 100, Side::Bid, 505, 5, Tif::GTC});
        printf("  best_bid=%d best_ask=%d resting=%zu\n",
               eng.best_bid(), eng.best_ask(), eng.resting_orders());
    }
    printf("== case 8: market data ==\n");
    {
        MatchingEngine eng(1 << 16);
        eng.set_market_sink([](const wire::MarketUpdate& md) {
            printf("  md: seq=%llu bid=%d(%u) ask=%d(%u) last=%d(%u)\n",
                   static_cast<unsigned long long>(md.exchange_seq),
                   md.best_bid, md.bid_qty, md.best_ask, md.ask_qty,
                   md.last_trade_px, md.last_trade_qty);
        });
        eng.submit(1, {wire::MsgType::New, 1, 100, Side::Bid, 500, 5, Tif::GTC});
        eng.submit(2, {wire::MsgType::New, 1, 200, Side::Ask, 505, 4, Tif::GTC});
        eng.submit(3, {wire::MsgType::New, 2, 201, Side::Ask, 500, 3, Tif::GTC});
    }
}

}  

int main(int argc, char** argv) {
    std::uint64_t seed = 42;
    int rounds = 3000;
    bool quiet = false;
    std::string wal_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cases") { demo_cases(); return 0; }
        else if (a == "--quiet") quiet = true;
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
        else if (a == "--wal" && i + 1 < argc) wal_path = argv[++i];
        else if (a == "--replay" && i + 1 < argc) return replay(argv[++i]);
    }

    Arena a(seed, rounds);
    std::optional<WalWriter> wal;
    if (!wal_path.empty()) {
        wal.emplace(wal_path, seed, a.value(), a.num_bots());
        a.set_wal(&*wal);
    }
    GameResult res = a.run();
    if (wal) wal->finish(a.digest());

    std::printf("Bot-vs-Bot Trading Arena  (seed=%llu  rounds=%d)\n",
                static_cast<unsigned long long>(seed), rounds);
    std::printf("------------------------------------------------------------\n");
    std::printf("Hidden value V .......... %d  (revealed at settlement)\n", res.value);
    std::printf("Lots traded ............. %llu\n",
                static_cast<unsigned long long>(res.volume));
    std::printf("Outbound messages ....... %llu\n",
                static_cast<unsigned long long>(res.messages));
    std::printf("Invariant: book not crossed ... %s\n", a.crossed() ? "FAIL" : "ok");
    std::printf("Invariant: qty conserved ...... %s\n",
                a.buy_volume() == a.sell_volume() ? "ok" : "FAIL");
    std::printf("Outbound digest ......... 0x%016llx\n",
                static_cast<unsigned long long>(a.digest()));
    if (wal)
        std::printf("WAL written ............. %s (%llu records)\n", wal_path.c_str(),
                    static_cast<unsigned long long>(wal->record_count()));
    std::printf("------------------------------------------------------------\n");
    print_leaderboard(res);

    if (!quiet) run_aggregate(seed, rounds, res);
    return 0;
}
