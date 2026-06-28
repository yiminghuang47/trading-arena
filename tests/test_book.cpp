// Matching-engine invariant + property tests.
//
// A handful of targeted unit tests, then a randomized fuzz test that fires
// hundreds of thousands of random messages at the engine and re-checks the core
// invariants after every one:
//   * the book is never crossed (best_bid < best_ask),
//   * best_bid / best_ask always equal the true extreme occupied level,
//   * total bought volume == total sold volume (every trade has two sides).
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "arena/order_book.hpp"

using namespace arena;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      ++g_fails;                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                     \
  } while (0)

// Independently recompute best bid/ask by scanning every level, and confirm the
// engine's cached values match. This is the strong check on on_rest/advance_best.
static void check_best(const MatchingEngine& eng) {
  Price hi_bid = kNoPrice;
  for (Price p = kPriceLevels - 1; p >= 0; --p)
    if (eng.bid_qty_at(p) > 0) { hi_bid = p; break; }
  Price lo_ask = kNoPrice;
  for (Price p = 0; p < kPriceLevels; ++p)
    if (eng.ask_qty_at(p) > 0) { lo_ask = p; break; }
  CHECK(eng.best_bid() == hi_bid);
  CHECK(eng.best_ask() == lo_ask);
  CHECK(!eng.crossed());
}

static wire::InboundOrder mk(wire::MsgType t, OrderId id, Side s, Price px, Qty q,
                             Tif tif = Tif::GTC) {
  return {t, 0, id, s, px, q, tif};
}

// --- targeted unit tests -----------------------------------------------------
static void test_rest_and_best() {
  MatchingEngine eng(1 << 12);
  eng.submit(1, mk(wire::MsgType::New, 1, Side::Bid, 500, 3));
  eng.submit(1, mk(wire::MsgType::New, 2, Side::Bid, 501, 2));  // better bid
  eng.submit(1, mk(wire::MsgType::New, 3, Side::Ask, 510, 5));
  CHECK(eng.best_bid() == 501);
  CHECK(eng.best_ask() == 510);
  CHECK(eng.resting_orders() == 3);
  check_best(eng);
}

static void test_match_and_partial() {
  MatchingEngine eng(1 << 12);
  eng.submit(1, mk(wire::MsgType::New, 1, Side::Ask, 510, 5));
  eng.submit(2, mk(wire::MsgType::New, 2, Side::Bid, 510, 7));  // crosses
  // 5 filled, 2 rest as a bid @ 510
  CHECK(eng.best_ask() == kNoPrice);
  CHECK(eng.best_bid() == 510);
  CHECK(eng.bid_qty_at(510) == 2);
  CHECK(eng.resting_orders() == 1);
  check_best(eng);
}

static void test_cancel_and_modify() {
  MatchingEngine eng(1 << 12);
  eng.submit(1, mk(wire::MsgType::New, 1, Side::Bid, 500, 5));
  eng.submit(1, mk(wire::MsgType::New, 2, Side::Bid, 499, 4));
  eng.submit(1, mk(wire::MsgType::Cancel, 1, Side::Bid, 0, 0));  // remove top
  CHECK(eng.best_bid() == 499);
  CHECK(eng.resting_orders() == 1);
  eng.submit(1, mk(wire::MsgType::Modify, 2, Side::Bid, 498, 4));  // reprice down
  CHECK(eng.best_bid() == 498);
  CHECK(eng.resting_orders() == 1);
  check_best(eng);
}

static void test_ioc_and_market() {
  MatchingEngine eng(1 << 12);
  eng.submit(1, mk(wire::MsgType::New, 1, Side::Ask, 505, 2));
  eng.submit(2, mk(wire::MsgType::New, 2, Side::Bid, 505, 5, Tif::IOC));  // fill 2, cancel 3
  CHECK(eng.resting_orders() == 0);
  eng.submit(1, mk(wire::MsgType::New, 3, Side::Ask, 505, 2));
  eng.submit(2, mk(wire::MsgType::New, 4, Side::Bid, kNoPrice, 10));  // market: fill 2
  CHECK(eng.resting_orders() == 0);
  CHECK(eng.best_ask() == kNoPrice);
  check_best(eng);
}

// --- randomized property test ------------------------------------------------
static void test_fuzz(std::uint64_t seed, int iters) {
  MatchingEngine eng(1 << 16);
  std::uint64_t buy = 0, sell = 0;
  eng.set_report_sink([&](BotId, const wire::ExecReport& r) {
    if (r.type == wire::ReportType::Fill) {
      if (r.side == Side::Bid) buy += r.qty;
      else                     sell += r.qty;
    }
  });

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int>   op(0, 4);
  std::uniform_int_distribution<int>   coin(0, 1);
  std::uniform_int_distribution<Price> pd(1, kPriceLevels - 2);
  std::uniform_int_distribution<Qty>   qd(1, 10);
  std::vector<OrderId> live;
  OrderId next_id = 1;
  const BotId owner = 1;

  for (int i = 0; i < iters; ++i) {
    const int o = op(rng);
    if (o <= 1 || live.empty()) {  // NEW limit order (weighted)
      const Side side = coin(rng) ? Side::Ask : Side::Bid;
      const Tif  tif  = coin(rng) ? Tif::IOC : Tif::GTC;
      const OrderId id = next_id++;
      eng.submit(owner, mk(wire::MsgType::New, id, side, pd(rng), qd(rng), tif));
      if (tif == Tif::GTC) live.push_back(id);
    } else if (o == 2) {           // cancel a (maybe stale) id
      const OrderId id = live[rng() % live.size()];
      eng.submit(owner, mk(wire::MsgType::Cancel, id, Side::Bid, 0, 0));
    } else if (o == 3) {           // modify a (maybe stale) id
      const OrderId id = live[rng() % live.size()];
      eng.submit(owner, mk(wire::MsgType::Modify, id,
                           coin(rng) ? Side::Ask : Side::Bid, pd(rng), qd(rng)));
    } else {                       // market order (never rests)
      const Side side = coin(rng) ? Side::Ask : Side::Bid;
      eng.submit(owner, mk(wire::MsgType::New, next_id++, side, kNoPrice, qd(rng)));
    }

    CHECK(!eng.crossed());      // cheap, every iteration
    CHECK(buy == sell);         // both sides of each trade emitted within submit
    if (i % 1000 == 0) check_best(eng);  // expensive full scan, periodically
  }
  check_best(eng);
  std::printf("  fuzz seed=%llu iters=%d volume=%llu resting=%zu msgs=%llu\n",
              static_cast<unsigned long long>(seed), iters,
              static_cast<unsigned long long>(buy), eng.resting_orders(),
              static_cast<unsigned long long>(eng.exchange_seq()));
}

int main() {
  test_rest_and_best();
  test_match_and_partial();
  test_cancel_and_modify();
  test_ioc_and_market();
  std::printf("unit tests done\n");

  for (std::uint64_t s = 1; s <= 5; ++s) test_fuzz(s, 200000);

  std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
  std::printf("RESULT: %s\n", g_fails == 0 ? "PASS" : "FAIL");
  return g_fails == 0 ? 0 : 1;
}
