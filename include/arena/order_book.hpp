#pragma once

#include <cstdint>
#include <vector> 
#include "arena/types.hpp"
#include "arena/protocol.hpp"

namespace arena{
    class MatchingEngine{
        public:
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
    };
}