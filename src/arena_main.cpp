// Wires the matching engine, accounting and reference bots together, runs a
// fixed-horizon game on a hidden terminal value V, settles P&L and prints a
// leaderboard. Pass --cases to run the engine demo/smoke cases instead.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

#include "arena/order_book.hpp"
#include "arena/reference_bots.hpp"

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

        cash_.assign(bots_.size(), 0);
        pos_.assign(bots_.size(), 0);

        engine_.set_report_sink([this](BotId owner, const wire::ExecReport& r) {
            on_report(owner, r);
        });
        engine_.set_market_sink([this](const wire::MarketUpdate& md) {
            for (auto& b : bots_) b->on_market_data(md);
        });
    }

    void send(BotId owner, const wire::InboundOrder& msg) override {
        engine_.submit(owner, msg);
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

    MatchingEngine engine_;
    int rounds_;
    Price value_ = 0;
    std::vector<std::unique_ptr<Bot>> bots_;
    std::vector<std::int64_t> cash_;
    std::vector<std::int32_t> pos_;
    std::uint64_t buy_volume_ = 0;
    std::uint64_t sell_volume_ = 0;
};

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

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 42;
    int rounds = 3000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cases") { demo_cases(); return 0; }
        else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
    }

    Arena a(seed, rounds);
    GameResult res = a.run();

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
    std::printf("------------------------------------------------------------\n");
    print_leaderboard(res);
    return 0;
}
