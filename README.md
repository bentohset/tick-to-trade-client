# Tick To Trade Client

A low latency trading client in C++23 that simulates a trading firm's side of an exchange connection.
It parses ITCH 5.0 market feed, handles the orders in an order book and sends orders over interchangeable OUCH
and FIX gateways.

- **Market data in:** receives ITCH over MoldUDP64 multicast, detects and
  recovers from dropped packets, and maintains a per-symbol mirror of the
  exchange's order book.
- **Orders out:** sends orders via OUCH over SoupBinTCP or FIX 4.4, selected by
  config, behind one protocol-neutral gateway interface. Sessions survive
  disconnects with no lost executions.
- **Test harness:** replays real Nasdaq ITCH data and includes a mock exchange
  supporting both protocols.

## Build

```bash
./build.sh --test --bench
./build.sh --release --bench
```

## Run a Scenario

book_dump:

``` bash
# check file is complete (parser only)
./build/book_dump data/itch/12302019.NASDAQ_ITCH50
# check whole day's book replay (parser + order book)
./build/book_dump data/itch/12302019.NASDAQ_ITCH50 --check
# check symbol at a point in time
./build/book_dump data/itch/12302019.NASDAQ_ITCH50 --symbol AAPL --at 10:30 --depth 5
```

book_dump useful commands:

```bash
# just before the opening cross
book_dump FILE --symbol AAPL --at 09:29:59.999
# half a second after the open
book_dump FILE --symbol AAPL --at 09:30:00.5 --depth 3
# into the close
book_dump FILE --symbol AAPL --at 15:59:59 --depth 20
# just after pre-market opens
book_dump FILE --symbol AAPL --at 04:00:01
```

itch_replay + feed_rx (simulate itch moldudp64):

```bash
# in one terminal (do this first)
./build/feed_rx --symbol AAPL --depth 5
# in second terminal
./build/itch_replay data/itch/12302019.NASDAQ_ITCH50
./build/itch_replay data/itch/12302019.NASDAQ_ITCH50 --drop-rate 0.01
```

## Benchmark

Build with `--bench` flag

```bash
./build/bench/bench_itch_parse data/itch/12302019.NASDAQ_ITCH50 --benchmark_repetitions=5 --benchmark_report_aggregates_only=true

./build/bench/bench_itch_parse data/itch/12302019.NASDAQ_ITCH50 --benchmark_repetitions=5 --benchmark_report_aggregates_only=true --benchmark_out=results.json --benchmark_out_format=json
```

## Performance

Order book replaying a full Nasdaq trading day: `12302019.NASDAQ_ITCH50`,
268.7 M messages across ~8,900 symbols, every message applied to a full-depth
book (up to 1.9 M live orders). Single thread on a GitHub Actions runner
(AMD EPYC 7763, x86-64), GCC 13.3, `-O3`. 0 book errors.


| Throughput | Latency p50 | Latency p99 | Latency p99.9 |
|---|---|---|---|
| 146 ns/msg (6.8 M msg/s) | 160 ns | 611 ns | 1,012 ns |


Per component (ns):


| Component | p50 | p99 |
|---|---|---|
| Market event | 160 | 611 |
| Order book insert | 60 | 240 |
| Order book remove | 80 | 431 |
| Order map lookup | 50 | 291 |
| Order map insert | 140 | 411 |

- Throughput is the median of 5 full-day replays; latencies time each call
  individually with `rdtsc` and include the timer itself (~30 ns per reading).
- ITCH parsing costs 8 ns/msg; the rest is the book. Inserting new orders
  into the hash map is the largest remaining cost.

Design, methodology and full results:
[docs/design/order-book-optimizations.md](docs/design/order-book-optimizations.md).

## Directory

```
tick-to-trade-client/
├── docs/
│   ├── design/                  # feature design docs
│   └── protocols/               # notes on ITCH 5.0, OUCH, MoldUDP64, SoupBinTCP
├── config/
│   ├── dev.toml                 # multicast group, gateway host/port, core pinning
│   └── risk_limits.toml         # max order size, max position, price bands
│
├── src/
│   ├── core/                     # low-level building blocks, no biz logic
│   │   ├── mapped_file.hpp/.cpp  # RAII mmap of a whole file as a byte span
│   │   ├── format.hpp            # parse/print time of day, print Price(4)
│   │   ├── latency_histogram.hpp # 1 ns buckets up to 65 us, percentiles
│   │   ├── cpu_affinity.hpp      # thread pinning
│   │   └── async_logger.hpp/.cpp
│   │
│   ├── net/                     # raw transport only, knows nothing about protocols
│   │   ├── udp_socket.hpp/.cpp
│   │   ├── tcp_client.hpp/.cpp
│   │   └── socket_opts.hpp      # busy polling, buffer sizes, TCP_NODELAY
│   │
│   ├── feed/                    # MARKET DATA IN
│   │   ├── moldudp64/  # session and gap detection
│   │   ├── itch/  # ITCH 5.0 parser
│   │   ├── book/                    # order book and manager
│   │   └── feed_handler.hpp/.cpp     # glues the above, emits book update events
│   │
│   ├── strategy/
│   │   ├── strategy.hpp         # interface: on_book_update(), on_fill(), ...
│   │   └── simple_mm.hpp/.cpp   # toy market maker to exercise the pipeline
│   │
│   ├── risk/
│   │   ├── pre_trade_risk.hpp   # header-only so it inlines on the hot path
│   │   ├── position_keeper.hpp/.cpp
│   │   └── kill_switch.hpp
│   │
│   └── gateway/                 # ORDERS OUT, ACKS AND FILLS BACK
        ├── common/
        │   ├── order_types.hpp          # NewOrder, ExecEvent, Side, TimeInForce
        │   ├── order_gateway.hpp        # the concept
        │   └── order_state.hpp/.cpp     # one state machine shared by both adapters
        ├── ouch/
        │   ├── soupbintcp_session.hpp/.cpp
        │   ├── codec.hpp
        │   └── ouch_gateway.hpp/.cpp
        └── fix/
            ├── session.hpp/.cpp         # logon, heartbeats, resend, gap fill
            ├── sequence_store.hpp/.cpp  # persist seq numbers across restarts
            ├── codec.hpp                # tag=value, BodyLength, CheckSum
            └── fix_gateway.hpp/.cpp
│
├── apps/
│   ├── trader/main.cpp          # full pipeline: feed -> strategy -> risk -> gateway
│   ├── itch_replay/main.cpp     # reads Nasdaq sample file, sends MoldUDP64 multicast
│   ├── book_dump/main.cpp       # offline: parse a file, print book or replay day
│   └── mock_exchange/main.cpp   # minimal SoupBinTCP/OUCH server that acks and fills
│
├── tests/
│   ├── unit/
│   ├── utils/
│   ├── integration/
│   │   ├── feed_end_to_end_test.cpp   # replay file -> compare final book snapshot
│   │   └── gateway_session_test.cpp   # login, disconnect, replay against mock exchange
│   └── data/                    # small ITCH captures + golden book snapshots
│
├── bench/
│   ├── bench_itch_parse.cpp         # messages/sec
│   ├── bench_order_book.cpp         # full-day replay throughput, ns/msg
│   ├── latency_order_book.cpp       # message latency by message type
│   ├── latency_order_book_micro.cpp # component latency
│   ├── bench_spsc_queue.cpp
│   └── bench_tick_to_trade.cpp  # packet in -> order out latency
│
├── scripts/
│   ├── fetch_itch_sample.sh     # download a Nasdaq ITCH day, or its first N messages
│   ├── format.sh                # clang-format all .cpp/.hpp (--check for CI)
│   ├── run_local.sh             # starts mock_exchange, itch_replay, trader
│   └── plot_latency.py          # histograms from bench output
│
├── build.sh                     # configure + build: [--release] [--test] [--bench]
│
└── .github/workflows/
    ├── ci.yml                   # gcc/clang Debug+Release build and tests, ASan+UBSan
    └── bench.yml                # manual: full-day benchmarks on a GitHub x86 runner
```

## TODO Steps

1. [x] Study

ITCH, MoldUDP64, SoupBinTCP, OUCH, FIX. Download sample ITCH files.

2. [x] ITCH offline parser

Reads a sample file from disk (memory-mapped) without networking. Helpers for big-endian, message layouts.

3. [x] Order book

  a. [x] Naive implementation

The client maintains its own read-only order book to match with the exchange's order book.
First implement without optimizations and ensure it can sync.

  b. [x] Optimized implementation

After that, optimize with intrusive lists (for cancel), object pools and others.

4. [x] Live feed

Wrap ITCH in MoldUDP64 packets to send the file over UDP multicast. Then the multicast receiver and sequence tracking.
Also add packet-drop option to implement gap detection and recovery. 

5. [ ] Core infra

Build SPSC queue for multithreading, async logger, rdtsc clock and CPU pinning.

6. [ ] OUCH gateway

Starting with the common adapter gateway. Then the OUCH protocol with SoupBinTCP session with a minimal mock_exchange to accept requests.
Build the order state machine that tracks each order from pending to live to filled or cancelled. 
Should be able to: kill connection in the middle of a session, reconnect and receive every missed execution once with correct order states.

7. [ ] E2E

Add strategy interface, toy market-making strategy, pre-trade risk checks, position keeper, and kill switch. Can be trivial strategy.
Should be able to: one command runs the system, positions reconcile against mock exchange records, kill switch stops trading immediately.

8. [ ] Bench

Timestamp each packet when it arrives and each order when it leaves. Produce tick-to-trade latency histograms. Good experiments to include are the multi-threaded design versus single-threaded, busy polling versus blocking reads, and virtual dispatch versus templates.

9. [ ] Add FIX

Add FIX codec and test it against both gateways. Benchmark

