# Trading Bot Arena

A limit-order-book exchange simulator in C++20. It's a matching engine fast enough
to be interesting, wrapped in a little game: you write trading bots, they compete
in an arena over a hidden "true price," and get ranked by P&L at the end.

I built it mostly to understand how a real matching engine is put together and to
have something where I could actually measure the latency of the choices — flat
arrays vs trees, pooled memory vs `new`, that kind of thing. The whole thing is
header-only for the engine, single-threaded, and deterministic: same seed, same
game, down to the byte.

## What's in the box

The engine handles limit and market orders, GTC and IOC, and new/cancel/modify,
all with price-time priority (best price first, ties broken by arrival order).
Cancel and modify are O(1) — no scanning the book to find your order.

Around that there's a small bot SDK (subclass `Bot`, implement `act()`), four
reference strategies to play against, and an arena that runs the game, settles
everyone's P&L against the revealed value, and prints a leaderboard. You can
record a game to a log and replay it later and get *exactly* the same output,
which turned out to be the most useful debugging tool in the whole project.

## How the engine works

The order book is really four data structures stacked together, each picked so one
operation is O(1):

```
bid_levels_ / ask_levels_   flat array indexed by price     ->  find best/level: O(1)
  └ each level: a doubly-linked FIFO of orders              ->  time priority + O(1) unlink
      └ orders live in one pre-allocated pool + free list   ->  no malloc while trading
id_map_: open-addressing hash, order_id -> pool slot        ->  find an order to cancel: O(1)
```

The trick that makes it fast is that **the price is the array index**. A resting
order at price 500 lives in `bid_levels_[500]`, so finding a price level or the
best bid/ask is just an array lookup — no tree walk, no hashing. The catch is that
this only works because prices are bounded small integers (`[0, 1024)` here). A
real venue with arbitrary prices would swap that array for a sorted structure and
keep everything else.

Everything else follows from wanting to avoid allocation on the hot path. Orders
don't get `new`'d one at a time; they live in a big array allocated once at
startup, and a free list hands out and reclaims slots. The linked-list pointers
are actually 4-byte indices into that pool, not real pointers. And cancel is O(1)
because a hash map remembers where each order id lives, so you can jump straight to
it and splice it out.

Trades execute at the *resting* order's price, not the incoming one — whoever was
in the book first set the terms.

## Layout

```
include/arena/
  types.hpp          shared value types (fixed-width, so they're safe on the wire)
  protocol.hpp       the binary messages, packed and size-checked with static_assert
  open_addr_map.hpp  the little allocation-free hash map
  order_book.hpp     the matching engine — the meat of it
  bot.hpp            bot base class + the OrderEntry handle bots send through
  reference_bots.hpp the four demo strategies
  wal.hpp            write-ahead log + replay
src/
  arena_main.cpp     runs the game, settles P&L, leaderboard, --wal / --replay
  bench_main.cpp     the benchmark
tests/
  test_book.cpp      the invariant / property tests
```

## Building

You need a C++20 compiler (I've been using g++ 13). On Windows I build under WSL.

```sh
./build.sh          # builds arena, tests, bench into build/
./build.sh --run    # ...and runs the tests + a demo game
```

## Running it

```sh
./build/arena --seed 42            # play a game + the 200-game aggregate
./build/arena --seed 42 --quiet    # just one game
./build/arena --seed 42 --wal g.wal  # record it
./build/arena --replay g.wal       # replay a recording and check it matches
./build/arena --cases              # engine smoke-test scenarios
./build/bench                      # benchmark (--n to set order count)
./build/test_book                  # the tests
```

## The bots

Each bot gets a slightly noisy guess at a hidden value `V` and quotes around it, so
over the game the book's price drifts toward the truth. The four that ship:

- **noise** — random orders, just there to provide liquidity and something to trade against.
- **naive_mm** — a market maker that quotes a fixed spread around its guess and ignores its inventory.
- **inv_mm** — same idea, but it skews its quotes based on how long/short it is, to pull its position back toward flat. This is the "good" one.
- **sniper** — sits back and only trades when someone's leaving a quote that's clearly mispriced, picking it off.

At the end everyone settles: `pnl = cash + position * V`. Run it over 200 games and
the skill ordering is pretty stable — the inventory-aware market maker wins on both
average P&L and Sharpe, and the noise bots reliably donate to everyone else:

```
rank bot            mean_pnl       stddev   sharpe
1    inv_mm           2595.7       1508.3     1.72
2    naive_mm         1012.0       1841.7     0.55
3    sniper             44.6        140.5     0.32
4    noise           -1757.2       1230.9    -1.43
5    noise           -1895.0       1261.0    -1.50
```

One game is mostly luck; the aggregate is where you can actually tell the
strategies apart, which is what the Sharpe column is for.

## Determinism and replay

A game is a pure function of its seed — one RNG seeds the hidden value, each bot's
guess, and each bot's own randomness. `--wal` writes every incoming order to a file
before the engine sees it; `--replay` feeds that file back through a fresh engine
and hashes the entire outbound stream (FNV-1a), then checks it against the hash from
the original run:

```
Replay byte-identical ... ok
```

So any game, including one that hit a bug, can be reproduced exactly. That's the
whole point — a bug that shows up once in a million messages is a lot easier to
chase when you can replay the exact million.

## Tests

`test_book` does a few hand-written cases and then throws about a million random
new/cancel/modify/market orders at the engine, re-checking the invariants after
*every single one*: the book is never crossed, the cached best bid/ask always match
what you'd get by scanning the whole book, and total volume bought equals total
sold. Around 2M assertions, no failures, and it's clean under ASan/UBSan. The
random test caught things the hand-written cases never would have.

## Performance

Single-threaded, in WSL2 with g++ 13 at `-O2`, pushing 2M mixed orders through it:
roughly **16M orders/sec**, about **62 ns/order**, with latency percentiles around
p50 ~100ns / p90 ~200ns / p99 ~340ns / p99.9 ~550ns.

A couple of honest caveats: the throughput number is the amortized cost over a
tight loop, while the latency pass times each call individually and so eats some
measurement overhead on top. The far tail (max, occasionally into the microseconds)
is OS scheduling and first-touch page faults, not the engine. Numbers move around
with how loaded the machine is — measure on an idle box. Run `./build/bench` and see.

## Known limits

The price range is bounded (that's the price of the flat-array trick). It's
single-writer — everything goes through `submit()` on one thread, which is what
keeps the hot path lock-free. The wire format is little-endian and would need
byte-swapping to talk across architectures. And the WAL is for determinism and
debugging, not crash durability — it doesn't `fsync`.
