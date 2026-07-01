// Bot SDK

#pragma once

#include "arena/protocol.hpp"
#include "arena/types.hpp"

namespace arena{
   
    class OrderEntry{
        public:
        virtual ~OrderEntry() = default;
        virtual void send(BotId owner, const wire::InboundOrder& msg) = 0;
    };

    class Bot{
        public:
        Bot(BotId id, OrderEntry* oe) : id_(id), oe_(oe) {}
        virtual ~Bot() = default;

        virtual const char* name() const = 0;

        // Engine callbacks. 
        virtual void on_market_data(const wire::MarketUpdate&) {}
        virtual void on_execution(const wire::ExecReport&) {}

        // Called once per round to let the bot quote / take.
        virtual void act() {}

        BotId id() const noexcept { return id_; }
        std::int32_t position() const noexcept { return pos_; }

        // The arena calls this to maintain position 
        void deliver_execution(const wire::ExecReport& r) {
            if (r.type == wire::ReportType::Fill) {
                pos_ += (r.side == Side::Bid)
                        ? static_cast<std::int32_t>(r.qty)
                        : -static_cast<std::int32_t>(r.qty);
            }
            on_execution(r);
        }

        protected:
        // Send a NEW limit/market order
        // returns the order_id used
        OrderId submit(Side side, Price price, Qty qty, Tif tif = Tif::GTC) {
            OrderId oid = next_order_id();
            oe_->send(id_, {wire::MsgType::New, ++seq_, oid, side, price, qty, tif});
            return oid;
        }

        void cancel(OrderId oid) {
            oe_->send(id_, {wire::MsgType::Cancel, ++seq_, oid, Side::Bid, 0, 0, Tif::GTC});
        }

        private:
        OrderId next_order_id() {
            // order_id (64 bits) = bot id (32 bits) + per-bot counter (32 bits)
            return (static_cast<OrderId>(id_) << 32) | (++order_counter_);
        }

        BotId         id_;
        OrderEntry*   oe_;
        Seq           seq_ = 0;
        std::uint32_t order_counter_ = 0;
        std::int32_t  pos_ = 0;
    };
}  
