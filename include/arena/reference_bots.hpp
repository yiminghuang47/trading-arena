// Reference demo strategies
//
//   NoiseBot : orders at random prices

#pragma once

#include <algorithm>
#include <random>

#include "arena/bot.hpp"

namespace arena{
    inline constexpr std::int32_t kPosLimit = 50;  // +/- inventory cap

    inline Price clamp_px(Price p) {
        return std::clamp<Price>(p, 1, kPriceLevels - 2);
    }

    class NoiseBot final : public Bot{
        public:
        NoiseBot(BotId id, OrderEntry* oe, std::uint64_t seed, Price est)
            : Bot(id, oe), rng_(seed), est_(est) {}

        const char* name() const override { return "noise"; }
        void on_market_data(const wire::MarketUpdate& md) override { md_ = md; }

        void act() override {
            std::uniform_int_distribution<int> coin(0, 1);
            std::uniform_int_distribution<int> off(-6, 6);
            std::uniform_int_distribution<Qty> qd(1, 4);
            // Pick a side, but respect the +/- position limit so inventory
            // mean-reverts around flat instead of random-walking away (otherwise
            // terminal P&L is pure luck and drowns out the skill bots).
            Side side;
            if (position() >= kPosLimit)       side = Side::Ask;
            else if (position() <= -kPosLimit) side = Side::Bid;
            else                               side = coin(rng_) ? Side::Bid : Side::Ask;
            const Price px = clamp_px(est_ + off(rng_));
            // Half the time cross the spread (IOC) to generate trades, else rest.
            const Tif tif = coin(rng_) ? Tif::IOC : Tif::GTC;
            submit(side, px, qd(rng_), tif);
        }

        private:
        std::mt19937_64 rng_;
        Price est_;
        wire::MarketUpdate md_{};
    };
}  
