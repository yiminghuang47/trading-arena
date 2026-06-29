#pragma once

#include <cstdint> 
#include "arena/types.hpp"

namespace arena::wire{
    enum class MsgType : uint8_t {
        New = 1,
        Cancel = 2,
        Modify = 3
    };
    enum class ReportType : uint8_t {
        Ack = 1,
        Fill = 2,
        Cancelled = 3,
        Reject = 4
    };
    #pragma pack(push, 1)
    struct InboundOrder{
        MsgType msg_type;
        Seq seq;
        OrderId order_id;
        Side side;
        Price price;
        Qty qty;
        Tif tif;
    };
    struct ExecReport{
        ReportType type;
        ExSeq exchange_seq;
        OrderId order_id;
        Side side;
        Price price;
        Qty qty;
    };
    
    struct MarketUpdate{
        ExSeq exchange_seq;
        Price best_bid;       // kNoPrice if no bids
        Qty   bid_qty;        // qty resting at best_bid (0 if none)
        Price best_ask;       // kNoPrice if no asks
        Qty   ask_qty;
        Price last_trade_px;  // kNoPrice if no trade this update
        Qty   last_trade_qty;
    };
    #pragma pack(pop)
    static_assert(sizeof(InboundOrder) == 23, "InboundOrder size");
    static_assert(sizeof(ExecReport) == 26, "ExecReport size");
    static_assert(sizeof(MarketUpdate) == 32, "MarketUpdate size");

}