// Reference demo strategies
//
//   NoiseBot : orders at random prices

#pragma once

#include <algorithm>
#include <optional>
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

    // Quotes a fixed spread around its value signal; no inventory management
    // beyond the hard position cap. A "textbook but naive" market maker.
    class NaiveMM final : public Bot{
        public:
        NaiveMM(BotId id, OrderEntry* oe, Price est, Price half_spread = 2, Qty size = 3)
            : Bot(id, oe), est_(est), half_(half_spread), size_(size) {}

        const char* name() const override { return "naive_mm"; }

        void act() override {
            if (bid_id_) cancel(*bid_id_);
            if (ask_id_) cancel(*ask_id_);
            // Fixed spread around the value signal; suppress the side that would
            // push us past the position limit (else inventory runs away).
            if (position() < kPosLimit)
                bid_id_ = submit(Side::Bid, clamp_px(est_ - half_), size_, Tif::GTC);
            else
                bid_id_.reset();
            if (position() > -kPosLimit)
                ask_id_ = submit(Side::Ask, clamp_px(est_ + half_), size_, Tif::GTC);
            else
                ask_id_.reset();
        }

        private:
        Price est_, half_;
        Qty   size_;
        std::optional<OrderId> bid_id_, ask_id_;
    };

    // Skews its reservation price against inventory so quotes mean-revert
    // position toward flat (Avellaneda-Stoikov in spirit). The "good" bot.
    class InventoryMM final : public Bot{
        public:
        InventoryMM(BotId id, OrderEntry* oe, Price est, Price half_spread = 2, Qty size = 3)
            : Bot(id, oe), est_(est), half_(half_spread), size_(size) {}

        const char* name() const override { return "inv_mm"; }

        void act() override {
            if (bid_id_) cancel(*bid_id_);
            if (ask_id_) cancel(*ask_id_);
            // Long -> quote lower to sell; short -> quote higher to buy.
            const Price skew = static_cast<Price>(position()) / 4;
            const Price center = est_ - skew;
            if (position() < kPosLimit)
                bid_id_ = submit(Side::Bid, clamp_px(center - half_), size_, Tif::GTC);
            else
                bid_id_.reset();
            if (position() > -kPosLimit)
                ask_id_ = submit(Side::Ask, clamp_px(center + half_), size_, Tif::GTC);
            else
                ask_id_.reset();
        }

        private:
        Price est_, half_;
        Qty   size_;
        std::optional<OrderId> bid_id_, ask_id_;
    };

    // Lifts/hits stale quotes when they cross its value signal -- picks off
    // mispriced resting orders left by the market makers.
    class SniperBot final : public Bot{
        public:
        SniperBot(BotId id, OrderEntry* oe, Price est, Price edge = 2, Qty size = 5)
            : Bot(id, oe), est_(est), edge_(edge), size_(size) {}

        const char* name() const override { return "sniper"; }
        void on_market_data(const wire::MarketUpdate& md) override { md_ = md; }

        void act() override {
            // Lift a cheap ask (priced below fair) if we have room to go long.
            if (md_.best_ask != kNoPrice && md_.best_ask < est_ - edge_ &&
                position() < kPosLimit) {
                submit(Side::Bid, md_.best_ask, std::min<Qty>(size_, md_.ask_qty), Tif::IOC);
            }
            // Hit a rich bid (priced above fair) if we have room to go short.
            if (md_.best_bid != kNoPrice && md_.best_bid > est_ + edge_ &&
                position() > -kPosLimit) {
                submit(Side::Ask, md_.best_bid, std::min<Qty>(size_, md_.bid_qty), Tif::IOC);
            }
        }

        private:
        Price est_, edge_;
        Qty   size_;
        wire::MarketUpdate md_{};
    };
}
