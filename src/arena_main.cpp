#include <cstdio>
#include "arena/types.hpp"
#include "arena/protocol.hpp"
#include "arena/order_book.hpp"

int main(){
    using namespace arena;

    MatchingEngine eng(1 << 16);

    // Report sink: print every outbound report the engine emits.
    eng.set_report_sink([](BotId owner, const wire::ExecReport& r) {
        printf("report: owner=%u type=%d seq=%llu id=%llu side=%d px=%d qty=%u\n",
               owner, static_cast<int>(r.type),
               static_cast<unsigned long long>(r.exchange_seq),
               static_cast<unsigned long long>(r.order_id),
               static_cast<int>(r.side), r.price, r.qty);
    });

    eng.submit(7, {wire::MsgType::New, 1, 100, Side::Bid, 500, 5, Tif::GTC});
    eng.submit(7, {wire::MsgType::New, 2, 101, Side::Bid, 499, 4, Tif::GTC});
    eng.submit(8, {wire::MsgType::New, 1, 200, Side::Ask, 500, 7, Tif::GTC});
    printf("best_bid=%d best_ask=%d\n", eng.best_bid(), eng.best_ask());

    return 0;
}