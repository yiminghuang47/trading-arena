#include <cstdio>
#include "arena/types.hpp"
#include "arena/protocol.hpp"
#include "arena/order_book.hpp"

using namespace arena;

// Build an engine whose report sink prints every outbound report.
static MatchingEngine make_engine() {
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

int main(){
    // Case 1: market buy into a thin book -> fills what's there, cancels the rest.
    printf("== case 1: market buy, thin book ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(8, {wire::MsgType::New, 1, 200, Side::Ask, 505, 3, Tif::GTC});
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, kNoPrice, 5, Tif::GTC}); // market
        printf("  best_bid=%d best_ask=%d\n", eng.best_bid(), eng.best_ask());
    }

    // Case 2: IOC buy that partially fills -> fill 2, cancel remaining 3, nothing rests.
    printf("== case 2: IOC buy, partial fill ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(8, {wire::MsgType::New, 1, 200, Side::Ask, 505, 2, Tif::GTC});
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, 505, 5, Tif::IOC});
        printf("  best_bid=%d best_ask=%d\n", eng.best_bid(), eng.best_ask());
    }

    // Case 3: IOC buy that can't fill (price too low) -> cancelled immediately, no fills.
    printf("== case 3: IOC buy, no fill ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, 400, 4, Tif::IOC});
        printf("  best_bid=%d best_ask=%d\n", eng.best_bid(), eng.best_ask());
    }

    // Case 4: cancel a resting order; best price should walk to the next level.
    printf("== case 4: cancel best bid ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, 500, 5, Tif::GTC});
        eng.submit(7, {wire::MsgType::New, 2, 101, Side::Bid, 499, 4, Tif::GTC});
        printf("  before cancel: best_bid=%d resting=%zu\n", eng.best_bid(), eng.resting_orders());
        // cancel order 100 (the 500 bid) -> best_bid should drop to 499
        eng.submit(7, {wire::MsgType::Cancel, 3, 100, Side::Bid, 0, 0, Tif::GTC});
        printf("  after cancel:  best_bid=%d resting=%zu\n", eng.best_bid(), eng.resting_orders());
    }

    // Case 5: cancel an unknown order id -> Reject.
    printf("== case 5: cancel unknown id ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(7, {wire::MsgType::Cancel, 1, 999, Side::Bid, 0, 0, Tif::GTC});
    }

    // Case 6: modify a resting bid to a new price (cancel-replace).
    printf("== case 6: modify reprice ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, 500, 5, Tif::GTC});
        printf("  before: best_bid=%d resting=%zu\n", eng.best_bid(), eng.resting_orders());
        eng.submit(7, {wire::MsgType::Modify, 2, 100, Side::Bid, 502, 5, Tif::GTC});
        printf("  after:  best_bid=%d resting=%zu\n", eng.best_bid(), eng.resting_orders());
    }

    // Case 8: market data broadcast — top-of-book after each message, last trade on a fill.
    printf("== case 8: market data ==\n");
    {
        MatchingEngine eng(1 << 16);
        eng.set_market_sink([](const wire::MarketUpdate& md) {
            printf("  md: seq=%llu bid=%d(%u) ask=%d(%u) last=%d(%u)\n",
                   static_cast<unsigned long long>(md.exchange_seq),
                   md.best_bid, md.bid_qty, md.best_ask, md.ask_qty,
                   md.last_trade_px, md.last_trade_qty);
        });
        eng.submit(1, {wire::MsgType::New, 1, 100, Side::Bid, 500, 5, Tif::GTC}); // md: bid 500
        eng.submit(2, {wire::MsgType::New, 1, 200, Side::Ask, 505, 4, Tif::GTC}); // md: 500/505
        eng.submit(3, {wire::MsgType::New, 2, 201, Side::Ask, 500, 3, Tif::GTC}); // trade @ 500
    }

    // Case 7: modify that crosses the book -> the replacement fills instead of resting.
    printf("== case 7: modify that crosses ==\n");
    {
        MatchingEngine eng = make_engine();
        eng.submit(8, {wire::MsgType::New, 1, 200, Side::Ask, 505, 5, Tif::GTC}); // resting ask
        eng.submit(7, {wire::MsgType::New, 2, 100, Side::Bid, 500, 5, Tif::GTC}); // resting bid
        // reprice the bid up to 505 -> it now crosses the ask and trades
        eng.submit(7, {wire::MsgType::Modify, 3, 100, Side::Bid, 505, 5, Tif::GTC});
        printf("  best_bid=%d best_ask=%d resting=%zu\n",
               eng.best_bid(), eng.best_ask(), eng.resting_orders());
    }

    return 0;
}