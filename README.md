# Tick To Trade Client

A low latency trading client in C++23 that simulates a trading firm's side of an exchange connection.
It consumes Nasdaq ITCH 5.0 market data, rebuilds the full order book, and sends orders
over interchangeable OUCH and FIX gateways.

- **Market data in:** receives ITCH over MoldUDP64 multicast, detects and
  recovers from dropped packets, and maintains a per-symbol mirror of the
  exchange's order book.
- **Orders out:** sends orders via OUCH over SoupBinTCP or FIX 4.4, selected by
  config, behind one protocol-neutral gateway interface. Sessions survive
  disconnects with no lost executions.
- **Test harness:** replays real Nasdaq ITCH data and includes a mock exchange
  supporting both protocols.

## Build

```
cmake --build build

./build/book_dump data/itch/12302019.NASDAQ_ITCH50
```

## Directory

```
tick-to-trade-client/
├── docs/
│   ├── architecture.md          # threads, data flow, design decisions
│   ├── latency-results.md       # p50/p99/p99.9 numbers, hardware used
│   └── protocols/               # notes on ITCH 5.0, OUCH, MoldUDP64, SoupBinTCP
├── config/
│   ├── dev.toml                 # multicast group, gateway host/port, core pinning
│   └── risk_limits.toml         # max order size, max position, price bands
│
├── src/
│   ├── core/                    # low-level building blocks, no trading logic
│   │   ├── spsc_queue.hpp       # lock-free ring buffer between threads
│   │   ├── object_pool.hpp      # preallocated orders, no malloc on hot path
│   │   ├── clock.hpp            # rdtsc timestamps + calibration
│   │   ├── endian.hpp           # big-endian reads for wire formats
│   │   ├── cpu_affinity.hpp     # thread pinning
│   │   └── async_logger.hpp/.cpp
│   │
│   ├── net/                     # raw transport only, knows nothing about protocols
│   │   ├── udp_multicast_rx.hpp/.cpp
│   │   ├── tcp_client.hpp/.cpp
│   │   └── socket_opts.hpp      # busy polling, buffer sizes, TCP_NODELAY
│   │
│   ├── feed/                    # MARKET DATA IN
│   │   ├── moldudp64/
│   │   │   ├── session.hpp/.cpp     # sequence tracking, gap detection
│   │   │   └── recovery.hpp/.cpp    # retransmit requests / snapshot fallback
│   │   ├── itch/
│   │   │   ├── messages.hpp         # ITCH 5.0 layouts and field offsets
│   │   │   ├── parser.hpp           # zero-copy decode, dispatch on msg type
│   │   │   ├── framing.hpp          # byte buffer
│   │   │   └── symbol_directory.hpp # stock locate code -> symbol
│   │   ├── book/
│   │   │   ├── order_book.hpp/.cpp  # per-symbol book, O(1) cancel by order id
│   │   │   ├── price_level.hpp
│   │   │   └── book_manager.hpp/.cpp
│   │   └── feed_handler.hpp/.cpp    # glues the above, emits book update events
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
│   ├── book_dump/main.cpp       # offline: parse a file, print book for a symbol
│   └── mock_exchange/main.cpp   # minimal SoupBinTCP/OUCH server that acks and fills
│
├── tests/
│   ├── unit/
│   ├── integration/
│   │   ├── feed_end_to_end_test.cpp   # replay file -> compare final book snapshot
│   │   └── gateway_session_test.cpp   # login, disconnect, replay against mock exchange
│   └── data/                    # small ITCH captures + golden book snapshots
│
├── bench/
│   ├── bench_itch_parse.cpp     # messages/sec
│   ├── bench_order_book.cpp     # ns per add/cancel/execute
│   ├── bench_spsc_queue.cpp
│   └── bench_tick_to_trade.cpp  # packet in -> order out latency
│
├── scripts/
│   ├── fetch_itch_sample.sh
│   ├── run_local.sh             # starts mock_exchange, itch_replay, trader
│   └── plot_latency.py          # histograms from bench output
│
└── .github/workflows/ci.yml     # build, tests, sanitizers (ASan, UBSan, TSan)
```

## TODO Steps

1. [x] Study

ITCH, MoldUDP64, SoupBinTCP, OUCH, FIX. Download sample ITCH files.

2. [x] ITCH offline parser

Reads a sample file from disk (memory-mapped) without networking. Helpers for big-endian, message layouts.

3. [ ] Order book

  a. [x] Naive implementation

The client maintains its own read-only order book to match with the exchange's order book.
First implement without optimizations and ensure it can sync.

  b. [ ] Optimized implementation

After that, optimize with intrusive lists (for cancel), object pools and others.

4. [ ] Live feed

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

