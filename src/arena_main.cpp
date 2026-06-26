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

    return 0;
}