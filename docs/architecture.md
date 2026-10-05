# Architecture

This document describes how the tick-to-trade client is put together: the
processes, the threads, how data moves between them, and why. Wire-level
details for each protocol are in [`protocols/`](protocols/).

> Status: this is the target design. Nothing is implemented yet. Update this
> file as decisions change or benchmarks contradict assumptions.

## Goals and non-goals

**Goals**
- Low and *predictable* tick-to-trade latency: the time from a packet arriving
  on the NIC to the order leaving the socket. The target is good p99/p99.9,
  not only a good p50.
- A correct order book. It must match the exchange's book exactly, including
  after packet loss.
- Correct order state. It must survive disconnects with no lost or duplicated
  executions.
- Interchangeable order-entry protocols (OUCH, FIX) behind one interface.

**Non-goals**
- Kernel bypass (DPDK, ef_vi, Onload). Use standard sockets with busy
  polling. This could be added later in `src/net/`.
- A realistic strategy. `simple_mm` exists only to exercise the pipeline.
- Multi-venue routing, persistence beyond sequence numbers and order tokens,
  and a UI.

## Process topology

`scripts/run_local.sh` starts three processes on one host:

```
┌──────────────┐  MoldUDP64 / UDP multicast   ┌──────────────────────────┐
│ itch_replay  │ ───────────────────────────► │                          │
│              │ ◄─── re-request (unicast) ── │         trader           │
│ (Nasdaq file)│ ──── retransmit (unicast) ─► │                          │
└──────────────┘                              │ feed → processing        │
                                              │            → gateway     │
┌──────────────┐  OUCH / SoupBinTCP  or  FIX  │                          │
│mock_exchange │ ◄──────── orders ─────────── │                          │
│              │ ───── acks / fills ────────► │                          │
└──────────────┘                              └──────────────────────────┘
```

- **itch_replay** memory-maps a Nasdaq ITCH 5.0 file, packs messages into
  MoldUDP64 packets, and multicasts them. Messages can be paced at the
  original timestamps or sent as fast as possible. It can drop packets on
  purpose and answers re-requests.
- **mock_exchange** is a minimal SoupBinTCP/OUCH (and FIX) server. It
  acknowledges orders and fills them using simple rules, keeps a sequenced
  outbound log so it can replay after a reconnect, and can kill the
  connection on command.
- **trader** is the system under test.
- **book_dump** is offline only. It parses a file and prints the book for one
  symbol. It is used to make snapshots and for debugging.

The mock exchange does not match orders against the replayed ITCH book. Fills
are synthetic. This keeps the exchange simple and the tests deterministic.

## Trader threads

Each latency-critical thread is pinned to its own isolated core
(`core/cpu_affinity.hpp`, cores set in `config/dev.toml`) and **busy-polls**.
It never blocks in the kernel waiting for data.

```
        NIC
         │ UDP multicast
         ▼
┌─────────────────────────────────┐
│ T1  FEED THREAD   (pinned, spin)│
│  udp_multicast_rx               │
│   → moldudp64::Session          │──── re-request (unicast UDP)
│   → itch::Parser                │
│   → book::BookManager           │
│   → emit BookUpdate             │
└───────────────┬─────────────────┘
                │ SPSC queue<BookUpdate>
                ▼
┌─────────────────────────────────┐        TCP
│ T2  TRADING THREAD (pinned,spin)│ ◄────────────────► exchange
│  Strategy::on_book_update       │
│   → PreTradeRisk::check         │
│   → Gateway::send_new / cancel  │
│  Gateway::poll() → ExecEvent    │
│   → OrderState → PositionKeeper │
│   → Strategy::on_fill           │
└───────────────┬─────────────────┘
                │ SPSC queues (log records)    (T1 has one too)
                ▼
┌─────────────────────────────────┐
│ T3  LOGGER THREAD  (unpinned)   │  formats and writes log records, latency samples
└─────────────────────────────────┘

    T0  MAIN / CONTROL: config, startup, signal handling, kill switch, stats
```

### Thread ownership rules

| Thread | Owns (sole writer)                                                     |
|--------|------------------------------------------------------------------------|
| T1 Feed | Multicast socket, MoldUDP64 session and recovery state, symbol directory, **all order books** |
| T2 Trading | Strategy state, risk limits, **order state**, positions, gateway session and TCP socket (**both directions**) |
| T3 Logger | Log files, latency sample dump                                     |
| T0 Control | Configuration, kill switch trigger, process lifecycle              |

- Data crosses threads only through **SPSC queues** (`core/spsc_queue.hpp`)
  or single atomics (the kill switch flag, the feed-health flag). There are no
  mutexes on the hot path.
- T2 owns the gateway socket for **both sending and receiving**. Fills and
  acknowledgements are handled on the same thread that sends orders, so order
  state and positions have one writer and need no synchronization. SoupBinTCP
  and FIX heartbeats are also driven from T2's poll loop.
- Books are never read from T2. T1 copies whatever the strategy needs into the
  `BookUpdate` event. This keeps the book single-threaded and the event
  self-contained.

### Single-threaded variant

A config switch collapses T1 and T2 into one thread. The feed loop calls the
strategy directly instead of pushing to the queue, and polls the gateway
between packets. This removes the queue hop, which saves about 50–100 ns plus
cache-line transfers, but a burst of market data can delay execution
handling. Comparing the two is one of the planned benchmarks (see
[Experiments](#experiments)).

## Data flow: market data in

1. **Receive.** `net::UdpMulticastRx` does a non-blocking `recvmsg()` into a
   preallocated buffer. A timestamp is taken with `rdtsc` (`core/clock.hpp`)
   as soon as the call returns. This `rx_tsc` follows the data through the
   pipeline.
2. **Sequence.** `moldudp64::Session` checks the session and sequence number.
   - In order: pass each message block to the parser.
   - Duplicate: drop it.
   - Gap: buffer the packets after the gap, set **feed state = RECOVERING**,
     and have `moldudp64::Recovery` send re-requests with a timeout and retry.
     Once the gap is filled, drain the buffer and return to **LIVE**.
3. **Parse.** `itch::Parser` dispatches on the type byte and decodes fields in
   place through `core/endian.hpp`. There are no copies and no allocations. It
   calls typed handlers (`on_add`, `on_execute`, `on_cancel`, …), using
   templates rather than virtual calls.
4. **Book.** `book::BookManager` keeps a
   `vector<OrderBook>` indexed by **stock locate** and one global
   `order_ref → Order*` hash map, because execute, cancel, delete and replace
   messages carry only the order reference.
5. **Emit.** If a message changed the best price or size on either side, or
   any level the strategy subscribes to, T1 pushes a `BookUpdate` onto the
   SPSC queue.

```cpp
struct BookUpdate {            // trivially copyable, ≤ 64 bytes (one cache line)
    uint64_t rx_tsc;           // NIC→user-space receive timestamp
    uint64_t seq;              // MoldUDP64 sequence of the triggering message
    uint16_t locate;
    uint32_t bid_px, bid_qty;  // top of book after this message
    uint32_t ask_px, ask_qty;
    uint8_t  reason;           // add / exec / cancel / replace / trading-state
};
```

Updates only go to symbols the strategy cares about (configured list). Books
for all symbols are still maintained, so the trader can switch symbols and
validate against golden snapshots.

## Data flow: orders out, executions back

1. `Strategy::on_book_update(const BookUpdate&)` returns zero or more
   intents (new, cancel, replace).
2. `PreTradeRisk::check()` is header-only so it inlines. In order, it checks:
   kill switch, feed health is LIVE, symbol trading state is `T`, max order
   size, price band against the current top of book, max position including
   open orders, and max open orders / order rate.
   A rejected intent is logged and dropped.
3. `OrderState` gets an order object from `core/object_pool.hpp` and assigns a
   token or ClOrdID. The order enters `PENDING_NEW`.
4. The gateway encodes the order straight into the session's send buffer and
   calls `send()`. `tx_tsc` is taken right after `send()` returns.
   **Tick-to-trade = `tx_tsc − rx_tsc`**, and the sample goes to T3.
5. On every pass of the loop, `Gateway::poll()` reads the socket, decodes the
   session framing and the application message, and produces protocol-neutral
   `ExecEvent`s (accepted, rejected, executed, canceled, replaced).
6. `OrderState` applies each event (see the state table in
   [`protocols/ouch.md`](protocols/ouch.md)). Executions update
   `PositionKeeper` and call `Strategy::on_fill`. The session sequence number
   is saved only **after** the event is applied.

### Gateway abstraction

`gateway/common/order_gateway.hpp` defines a C++20 **concept**, not a virtual
base class:

```cpp
template <class G>
concept OrderGateway = requires(G g, const NewOrder& n, OrderId id, Qty q, Px p) {
    { g.send_new(n) }          -> std::same_as<bool>;
    { g.send_cancel(id, q) }   -> std::same_as<bool>;
    { g.send_replace(id, q, p) } -> std::same_as<bool>;
    { g.poll(std::declval<ExecSink&>()) };   // drains socket, emits ExecEvents
    { g.connected() }          -> std::same_as<bool>;
};
```

Concept is chosen because:
- The gateway is picked once during compile time. A virtual call makes the same choice again on every order.
- Virtual calls does not allow the compiler to optimize. With templates, the compiler can keep values in registers and drop checks it proves unnecessary
- Does not need a base-class ptr, no heap allocation and no vtable ptr to load. A small win and avoids allocation on hot paths.
- The decode loop and handlers will compile into one specialized function. If virtual, each decode and fill would cross several
  virtual calls that can't be inclined.

`apps/trader/main.cpp` reads `gateway = "ouch" | "fix"` from config and
instantiates the whole trading loop once for that type
(`run<OuchGateway>(cfg)` or `run<FixGateway>(cfg)`). Dispatch happens once at
startup, and the hot path is fully static. Both adapters convert to the
shared types in `order_types.hpp` and use the same `OrderState`, so strategy,
risk and position code are written once.

## Memory and performance rules

- **No heap allocation on the hot path after startup.** Orders (book side and
  own-order side) come from object pools. Hash maps use open addressing with
  capacity reserved at startup. Books are preallocated by locate. CI can
  enforce this with a debug allocation hook that aborts.
- **Parse in place.** Never copy wire messages into structs. Read fields at
  fixed offsets with `memcpy` and byteswap.
- **Intrusive containers.** Each book order is a node in a doubly linked list
  at its price level, so removing it given an `Order*` is O(1). Price levels
  per side live in a structure sized for the common case, where most activity
  is near the top.
- **Cache-line awareness.** SPSC head and tail indices sit on separate cache
  lines, sized per architecture (see [Platform support](#platform-support)).
  `BookUpdate` fits in 64 bytes. Nothing shared between threads is
  written by more than one thread.
- **Syscalls.** The hot threads make only non-blocking `recv`/`send` calls.
  Logging is a push to an SPSC queue of fixed-size binary records, and T3
  formats them.
- **Socket options** (`net/socket_opts.hpp`): `SO_RCVBUF` large enough to
  absorb bursts, `SO_BUSY_POLL` where supported, and `TCP_NODELAY` on the
  gateway socket.

## Failure handling

| Failure                         | Detection                          | Response |
|---------------------------------|------------------------------------|----------|
| Multicast packet loss           | MoldUDP64 sequence gap / heartbeat | RECOVERING: risk blocks new orders, re-request the range, resume when filled. If recovery times out, cancel all orders and halt (snapshot rebuild is future work) |
| Feed silent                     | No packet or heartbeat for N s     | Mark feed STALE and block orders |
| Gateway disconnect              | `recv` returns 0 or error, or heartbeat timeout | Stop sending. Reconnect with backoff. Log in at `last_seq + 1` and apply the replay. Resolve orders still `PENDING_NEW` (see protocol docs) |
| Exchange reject                 | `Rejected` / `Cancel Reject`       | Update order state and notify the strategy. Too many rejects in a row trips the kill switch |
| Risk breach / operator action   | Limit check, `SIGUSR1`, control command | **Kill switch**: atomic flag set. T2 stops sending new orders immediately, cancels every open order, and stays halted until restart |
| Process restart                 | n/a                                | Sequence store and token counter are reloaded from disk, so tokens are never reused and replay starts at the right point |

The kill switch flag is checked first in `PreTradeRisk::check()` and before
every send. T0 sets it, and T2 sees it on its next loop iteration, which is
far below a microsecond while spinning.

## Configuration

- `config/dev.toml`: multicast group, interface and port; re-request server
  address; gateway type, host, port and credentials; core numbers for T1, T2
  and T3; threading mode (`pipeline` / `single`); subscribed symbols.
- `config/risk_limits.toml`: max order size, max position per symbol, max
  notional, price band (bps from top of book), max open orders, max order rate.

Configuration is read once at startup. Nothing reloads it at runtime.

## Startup sequence

1. T0 parses config, allocates all pools, books and queues, and touches
   (prefaults) the memory.
2. Start T3 (logger).
3. Start T2. Connect the gateway, log in, apply any replay, and reconcile open
   orders and positions.
4. Start T1. Join multicast, process symbol directory (`R`) messages, and
   build books. Feed state becomes LIVE once a full contiguous sequence is
   being applied.
5. Enable trading only when the gateway is logged in **and** the feed is LIVE.

When joining a feed mid-day without a snapshot, the book is incomplete. For
local runs, `itch_replay` and the trader start together so the trader sees the
feed from sequence 1.

## Testing strategy

| Layer        | Test                                       | What it proves |
|--------------|--------------------------------------------|----------------|
| Unit         | `itch_parser_test`                         | Each message decodes to the right fields at the right offsets |
| Unit         | `order_book_test`                          | Add, execute, cancel, delete and replace semantics; level aggregation; O(1) removal |
| Unit         | `moldudp64_gap_test`                       | Duplicates, overlaps, gaps, tail gaps, recovery drain order |
| Unit         | `ouch_codec_test`                          | Encode/decode round trips against byte-exact fixtures |
| Unit         | `risk_test`                                | Each limit, kill switch precedence |
| Integration  | `feed_end_to_end_test`                     | Replay file → final book equals golden snapshot, with and without injected drops |
| Integration  | `gateway_session_test`                     | Log in, kill the connection mid-stream, reconnect; every execution applied exactly once; positions match the mock exchange |

CI (`.github/workflows/ci.yml`) builds and runs all of these under ASan,
UBSan and TSan. TSan is mainly useful for the SPSC queue and thread handoff.

## Platform support

**Linux is the only supported OS.** Each job runs on a fixed machine type:

| Purpose                      | Where                                                 | Arch    |
|------------------------------|-------------------------------------------------------|---------|
| Day-to-day build and test    | Linux dev container on Mac (Docker, native arm64 image) | arm64   |
| CI (build, tests, sanitizers)| GitHub `ubuntu-latest` runners                        | x86-64  |
| Benchmarks / `latency-results.md` | Dedicated or bare-metal x86-64 Linux host       | x86-64  |

macOS is not a build target. Using only Linux means `net/` and `core/` have no
macOS code paths (macOS has no thread pinning, no `SO_BUSY_POLL`, and no
`SO_TIMESTAMPING`).

### Architecture-specific code

The only code that differs by architecture is in `core/`:

| Concern            | x86-64                          | Other (arm64)                                |
|--------------------|---------------------------------|----------------------------------------------|
| Timestamps (`clock.hpp`) | `rdtsc` / `rdtscp`, calibrated against `CLOCK_MONOTONIC` | `clock_gettime(CLOCK_MONOTONIC)` (resolution is coarser; fine for functional runs) |
| Cache line size    | 64 bytes                        | Up to 128 bytes (Apple Silicon)              |

- Padding for false sharing uses one constant, set per architecture (or
  `std::hardware_destructive_interference_size`), never a hardcoded `64`.
  `BookUpdate` stays at most 64 bytes on every platform.
- Wire decoding does not depend on the architecture. Both targets are
  little-endian, and `endian.hpp` byteswaps explicitly.
- ARM orders memory more weakly than x86. Running the SPSC queue tests under
  TSan on the arm64 dev container is a deliberate extra check: bugs from a
  wrong `memory_order` that x86 hides tend to appear there.

### Dev container notes

- Run `itch_replay`, `mock_exchange` and `trader` **in one container**, with
  multicast over loopback. This avoids multicast across Docker networks.
  `--network host` on macOS exposes the Docker VM's network, not the Mac's.
- Keep the build directory on a Docker volume rather than the source bind
  mount, which is much faster.
- Pin the image's distro and compiler to match `ci.yml`.
- Do not use an emulated `linux/amd64` image for performance work. Under
  emulation, timings are meaningless.
- **Do not quote latency numbers from the container.** It runs in a VM sharing
  cores with macOS, so CPU pinning applies only to virtual CPUs and the tail
  latencies are mostly noise.

## Measurement

- `rx_tsc` and `tx_tsc` are read with `rdtsc`. `core/clock.hpp` calibrates TSC
  ticks to nanoseconds at startup and assumes an invariant TSC. On non-x86
  builds the clock falls back to `CLOCK_MONOTONIC` (see
  [Platform support](#platform-support)). All published numbers come from
  x86-64.
- Samples are recorded without allocation into a preallocated ring and dumped
  by T3. `scripts/plot_latency.py` produces histograms and
  p50/p99/p99.9/max, which are recorded in `docs/latency-results.md` along
  with the hardware and kernel settings.
- Microbenchmarks in `bench/` isolate parse, book, queue and end-to-end costs.

### Experiments

| Question                                    | Variants                                         |
|---------------------------------------------|--------------------------------------------------|
| Is the extra thread worth it?               | `pipeline` (T1 → SPSC → T2) vs `single` thread   |
| Spin vs block                               | Busy-poll non-blocking `recv` vs blocking `recv` / `epoll_wait` |
| Static vs dynamic dispatch                  | Concept/template gateway and parser handlers vs virtual interfaces |
| Book data structure                         | `std::map` levels vs sorted vector vs tick-indexed array |
| Protocol cost                               | OUCH (binary, fixed offsets) vs FIX (tag=value, checksum) encode and decode |


## Open questions

- Conflation: if T2 falls behind, should T1 coalesce updates per symbol
  (latest top of book wins) instead of queueing every one?
- Hardware or socket receive timestamps (`SO_TIMESTAMPING`) as the start of
  the tick-to-trade measurement instead of a user-space `rdtsc`.

