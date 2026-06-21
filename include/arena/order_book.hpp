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
    };
}