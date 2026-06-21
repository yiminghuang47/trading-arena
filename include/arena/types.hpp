#pragma once
#include <cstdint>

namespace arena {
    using OrderId = uint64_t;
    using Price = int32_t;
    using Qty = uint32_t;
    using Seq = uint32_t;
    using ExSeq = uint64_t;
    using BotId = uint16_t;

    enum class Side : uint8_t {
        Bid = 0,
        Ask = 1
    };

    // Time-In-Force (how long an order should stay active)
    // GTC = Good-Til-Cancelled. Can't fill immediately -> rest in book
    // IOC = Immediate-Or-Cancel. Fill immediately or else cancel. 

    enum class Tif : uint8_t {
        GTC = 0,
        IOC = 1
    };

    // prices ranges in [0,1024)
    inline constexpr Price kPriceLevels = 1024;
    // Sentinel for no price
    inline constexpr Price kNoPrice = -1;

    constexpr Side opposite(Side s) noexcept{
        return s == Side::Bid ? Side::Ask : Side::Bid;
    }
}