#pragma once

#include <cstdint>
#include <vector> 
#include <functional>
#include "arena/types.hpp"
#include "arena/protocol.hpp"

namespace arena{
    class MatchingEngine{
        public:
        using ReportSink = std::function<void(BotId owner, const wire::ExecReport&)>;
        explicit MatchingEngine(size_t max_orders){
            pool_.resize(max_orders);
            bid_levels_.resize(kPriceLevels);
            ask_levels_.resize(kPriceLevels);
            free_list_.reserve(max_orders);
            for (size_t i = max_orders; i > 0; i--){
                // push free slots into the free list
                // [0, max_orders-1]
                free_list_.push_back(static_cast<int32_t>(i-1));
            }
        
        }
        Price best_bid() const noexcept { return best_bid_; }
        Price best_ask() const noexcept { return best_ask_; }
        bool crossed() const noexcept { 
            if (best_bid_ == kNoPrice || best_ask_ == kNoPrice) return false;
            return best_bid_ >= best_ask_; 
        }
        void set_report_sink(ReportSink s){
            on_report_ = std::move(s);
        }
        
        void submit(BotId owner, const wire::InboundOrder& msg) {
            switch (msg.msg_type) {
                case wire::MsgType::New: handle_new(owner, msg); break;
                case wire::MsgType::Cancel: break;
                case wire::MsgType::Modify: break;
            }
        }

        private:
        struct Order {
            OrderId id;
            BotId owner;
            Side side;
            Price price;
            Qty qty;
            int32_t prev = -1; // pool indices
            int32_t next = -1;
        };
        struct Level {
            // fifo queue of resting orders at this price
            // for example, for a certain price level p
            // let i = the indices of orders with price p in the pool
            // then head = first element of i
            // tail = last element of i
            // total = total quantity at this price
            int32_t head = -1;
            int32_t tail = -1;
            Qty total = 0;
        };
        std::vector<Order> pool_;
        std::vector<int32_t> free_list_; // available slots
        std::vector<Level> bid_levels_;
        std::vector<Level> ask_levels_;
        Price best_bid_ = kNoPrice; 
        Price best_ask_ = kNoPrice;
        ExSeq ex_seq_ = 0;

        ReportSink on_report_;


        int32_t alloc_order(){
            if (free_list_.empty()){
                // full
                return -1;
            }
            else{
                auto back = free_list_.back();
                free_list_.pop_back();
                return back;
            }
        }
        void free_order(int32_t idx){
            free_list_.push_back(idx);
        }
        void push_back(Level& lvl, int32_t idx){
            // append order idx to the tail of lvl
            Order& o = pool_[idx];
            o.prev = lvl.tail;
            o.next = -1;
            if(lvl.tail == -1){
                // empty
                lvl.head = idx;
            }
            else{
                pool_[lvl.tail].next = idx;
            }
            lvl.tail = idx;
            lvl.total += o.qty;
        }

        void unlink(Level& lvl, int32_t idx){
            // remove idx from the lvl fifo (for canceling order)
            // O(1) cuz it's a doubly linked list
            Order& o = pool_[idx];
            if(o.prev != -1){
                pool_[o.prev].next = o.next;
            }
            else{
                // idx is head
                lvl.head = o.next;
            }
            if(o.next != -1){
                pool_[o.next].prev = o.prev;
            }
            else{
                // idx is tail
                lvl.tail = o.prev;
            }
            lvl.total -= o.qty;
        }

        void advance_best(Side side){
            // update best price
            if (side == Side::Bid){
                while(best_bid_ >= 0 && bid_levels_[best_bid_].total == 0){
                    best_bid_--;
                }
                if (best_bid_ < 0) best_bid_ = kNoPrice;
            }
            else{
                while(best_ask_ < kPriceLevels && ask_levels_[best_ask_].total == 0){
                    best_ask_++;
                }
                if (best_ask_ == kPriceLevels) best_ask_ = kNoPrice;
            }
        }
        void on_rest(Side side, Price px){
            // update best price
            if(side == Side::Bid && (best_bid_ == kNoPrice || px > best_bid_)){
                best_bid_ = px;
            }
            if(side == Side::Ask && (best_ask_ == kNoPrice || px < best_ask_)){
                best_ask_ = px;
            }
        }

        void emit(BotId owner, wire::ReportType type, 
                OrderId id, Side side, Price px, Qty qty) {
            if (on_report_){
                on_report_(owner, {type, ++ex_seq_, id, side, px, qty});
            }
        }

        void handle_new(BotId owner, const wire::InboundOrder& msg){
            const bool is_market = (msg.price == kNoPrice); // market order
            if( msg.qty ==0 || (!is_market && (msg.price < 0 || msg.price >= kPriceLevels))){
                emit(owner, wire::ReportType::Reject, msg.order_id, msg.side, msg.price, msg.qty);
                return;
            }
            // assume GTC for now
            int32_t idx = alloc_order();
            if(idx==-1){
                emit(owner, wire::ReportType::Reject, msg.order_id, msg.side, msg.price, msg.qty);
                return;
            }
            pool_[idx] = Order{msg.order_id, owner, msg.side, msg.price, msg.qty, -1, -1};
            Level& lvl = (msg.side == Side::Bid) ? bid_levels_[msg.price] : ask_levels_[msg.price];
            push_back(lvl, idx);
            on_rest(msg.side, msg.price);
            emit(owner, wire::ReportType::Ack, msg.order_id, msg.side, msg.price, msg.qty);
        }


    };
}