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
    #pragma pack(pop)
    static_assert(sizeof(InboundOrder) == 23, "InboundOrder size");
    static_assert(sizeof(ExecReport) == 26, "ExecReport size");

}