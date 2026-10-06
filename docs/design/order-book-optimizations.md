# Order book optimizations

Status: **design**. first version (naive, standard containers) is implemented and is the
reference. This document plans the next version and is where its results get recorded.

For reference, the first version will be called v1 and the optimized version be v2.

Related: [architecture.md](../architecture.md) (threads, memory rules),
[protocols/itch.md](../protocols/itch.md) (message semantics).

## Goal

Cut the cost of applying one ITCH message to the order book from ~350 ns (v1)
to tens of ns, with **identical output**: same levels, same quantities, same
error counters, on every message of a full trading day.

Speed is measured two ways (see [Measurement](#measurement)):
- **Throughput**: ns/msg averaged over a full replay (Google Benchmark).
- **Latency distribution**: p50 / p90 / p99 / p99.9 / max of individual
  message applies, per message type

## Workload

Measured on `12302019.NASDAQ_ITCH50` (full trading day). These numbers drive
the sizing decisions below.

| Fact | Value |
|---|---|
| Messages | 268.7 M (A 117.1 M, D 114.4 M, U 21.6 M, E 5.7 M, X 2.8 M, F 1.5 M, C 0.1 M) |
| Live orders at peak, all symbols | 1.92 M |
| Live orders at end of day | 0 |
| Order adds (A + F + U) | 140.3 M |
| Max order reference number | 308.6 M (refs used: ~45% of the range) |
| Adds whose ref is lower than the previous add's | 36 M (refs are not monotonic) |
| Busiest symbol, peak live orders | AMZN, 37 k |
| Max price levels on one side | 4,227 (bid), 2,570 (ask) |

## Baseline: v1

| Component | v1 implementation |
|---|---|
| Order lookup | `std::unordered_map<OrderRef, Order>`, `reserve(4M)` |
| Price levels | `std::map<Price, Agg>` per side (red-black tree) |
| Order queue within a level | none: levels only hold total qty and order count |


## Optimizations

Each one is introduced and measured separately, in this order, so the results
table can show what each contributed.

### O1. Order pool

Replace per-order heap nodes with one preallocated array of orders and a free
list.

```cpp
struct Order {            // 32 bytes: two per 64-byte cache line
  OrderRef ref;           // 8
  Price    price;         // 4
  Qty      qty;           // 4
  uint32_t prev;          // 4  FIFO links (O3), pool indices
  uint32_t next;          // 4
  uint16_t locate;        // 2
  Side     side;          // 1
  // 5 bytes padding
};
static_assert(sizeof(Order) == 32);
```

- Orders are addressed by `uint32_t` index, not pointer: half the size, and
  indices stay valid if the pool is ever relocated.
- Capacity: **4 M** orders (2× the measured 1.92 M peak), 128 MB. Allocated
  and touched (prefaulted) at startup.
- Free list threaded through `next` of free slots: O(1) alloc and free, no
  system calls.
- Pool exhaustion is counted as an error (new `Errors::pool_full`) and the
  add is dropped. Never grow on the hot path; size it from data instead.

Removes cost 1 entirely.

### O2. Open-addressing order map

Replace `std::unordered_map` with a flat hash table: `ref -> pool index`.

```cpp
struct Slot { OrderRef ref; uint32_t index; };   // 16 bytes with padding; ref 0 = empty
```

- **Capacity 8 M slots** (power of two), load factor ≤ 0.24 at the 1.92 M
  peak, 128 MB. A smaller table (4 M slots, load ≤ 0.48) is worth measuring
  too: half the memory, slightly longer probes.
- **Linear probing**: a lookup reads consecutive slots, usually in one cache
  line, so typically **one cache miss** per lookup instead of two or three.
- **Backward-shift deletion**, no tombstones. The workload deletes as often as
  it inserts (140 M each), and tombstones would pile up and lengthen probes
  all day.
- **Hash:** a multiplicative (Fibonacci) mix, `(ref * 0x9E3779B97F4A7C15) >> (64 - bits)`.
  Identity hashing would be tempting because refs are roughly sequential,
  but they are not monotonic and only ~45% dense; measure both.
- Ref 0 is reserved as "empty". The lowest order ref on the sample day is 42;
  `P` messages send 0 but never enter the book. An add with ref 0 is counted
  as an error.

Rejected: a flat array indexed directly by ref. Max ref 308.6 M × 4 bytes =
1.2 GB, mostly empty, and the max isn't known in advance.

Removes most of cost 2.

### O3. Intrusive FIFO per price level

Each level keeps its orders in time priority as a doubly linked list threaded
through `Order::prev/next`, with `head` (oldest) and `tail` (newest) in the
level.

- Unlink any order in O(1) given its pool index; no search within the level.
- Gives **queue position** ("shares ahead of my order"), which the strategy
  will need later for its own orders.
- A replace (`U`) is unlink + push to the back, matching exchange priority.

v1's output (aggregated levels) doesn't need the FIFO, so O3 **adds** work
(extra writes per add and remove). It is measured on its own so its cost is
known; it is kept because later steps need queue position.

### O4. Price levels as a sorted vector

Replace `std::map` per side with a `std::vector<PriceLevel>` sorted so the
**best price is at the back**:

```
bids_: ascending  price -> back() = highest bid
asks_: descending price -> back() = lowest ask
```

```cpp
struct PriceLevel {       // 24 bytes
  Price    price;
  uint32_t count;         // orders at this price
  uint64_t qty;           // total shares
  uint32_t head, tail;    // FIFO (O3)
};
```

- Most adds, cancels and executes hit the top few levels. Near the back,
  finding a level is a short scan, and inserting or erasing one moves only
  the few elements behind it.
- Find: linear scan from the back for the first N levels (N ≈ 8–16, tune),
  then binary search. Contiguous memory, so the scan is cache-friendly.
- `reserve()` each side at symbol setup (e.g. 64 levels) so normal days never
  reallocate during trading; busy symbols grow once early in the day.
- Orders cannot store a level index: inserting a level shifts the ones after
  it. They store their price and find the level by price (cheap near the top).

Alternative to measure if level lookup still shows up in profiles: levels in
their own pool with stable indices (orders store a level index), and the
sorted vector holds level indices. One more indirection on scans, no search
on reduce/remove.

Rejected for now: an array indexed by price tick. Price ranges per symbol are
not known in advance and can be very wide (4,000+ levels, stub quotes far from
the market).

Removes cost 3.

### O5. No allocation after startup

- `books_` and per-symbol state are sized when `R` messages arrive, which is
  before trading starts.
- Pool, hash table and level vectors are preallocated and prefaulted.
- A debug allocation hook (counting `operator new` calls during replay after
  the first `S 'Q'` system event) asserts zero allocations in tests.

### Not in v2

- Multi-threading, SIMD, prefetching, huge pages. Huge pages (2 MB) for the
  pool and hash table are a good follow-up experiment: 256 MB of 4 KB pages
  is 65 k TLB entries' worth.
- Changing the public interface: `BookManager` and `OrderBook` keep the same
  methods, so `book_dump` and the tests don't change.

## Correctness

v1 stays in the codebase as the reference implementation.

1. **Differential replay**: feed every message of the full day to both books.
   After each message compare best bid/ask of the touched symbol; every
   1 M messages compare full depth of all symbols; at the end compare error
   counters and live order counts. The first difference stops the run and
   prints the message.
1. **`book_dump --check`** on the full day: all error counters 0, live orders
   0 at end of day.

## Measurement

Same input, same machine, Release (`-O3 -DNDEBUG`), file prefaulted in memory.

### Throughput: `bench/bench_order_book`

Google Benchmark, replaying the file through `BookManager` once per
iteration. Reports ns/msg and messages/s.

```bash
build-release/bench/bench_order_book FILE --benchmark_repetitions=5 \
    --benchmark_display_aggregates_only=true \
    --benchmark_out=docs/design/results/<impl>/<impl>-<machine>-bench.json --benchmark_out_format=json
```

### Latency distribution: `bench/latency_order_book`

Replays the file and times **each message's book apply** individually with
`core/clock.hpp`, recording into a histogram per message type. Writes p50,
p90, p99, p99.9, max and the timer overhead as JSON:

```bash
build-release/bench/latency_order_book FILE --label <impl>-<machine> \
    > docs/design/results/<impl>/<impl>-<machine>-latency.json
```

Timer: `rdtsc` on the x86-64 runners, roughly 20–40 cycles (~7–15 ns) per
read with sub-ns resolution. The JSON reports the cost of an empty timer read
pair next to the results; nothing is subtracted.

Mean latency under per-message timing comes out higher than the throughput
number: reading the timer around every message stops the CPU from overlapping
consecutive messages. Compare throughput with throughput and percentiles with
percentiles.

### Where it runs: GitHub Actions

All results come from GitHub-hosted runners (`gha-x86`: `ubuntu-24.04`,
x86-64, 4 vCPU, 16 GB RAM for a public repo).

`.github/workflows/bench.yml` (Actions tab -> **Bench** -> Run workflow) builds
Release, runs `bench_itch_parse`, `bench_order_book` and `latency_order_book`,
and uploads the JSON as the `bench-results-<label>` artifact. It uses the
**whole day** by default; `itch_bytes` can select a slice from the start of the
day instead. Only compare results that used the same input.

Each version is benchmarked in its own workflow run. Runners are shared VMs,
so to keep runs comparable:
- **check the CPU model** in `gha-x86-machine.txt`: only compare runs on the
  same model (so far: AMD EPYC 7763); rerun if a runner lands on different
  hardware;
- use **5 repetitions** and compare medians, with the minimum as a second view
  (the run least disturbed by neighbouring VMs); check the `cv`;
- every repetition is kept in the JSON (`--benchmark_display_aggregates_only`
  only trims the console output);
- treat p99.9 and max as indicative: hypervisor pauses land in the tail.

## Results

Each version has a folder, [`docs/design/results/<impl>/`](results/), holding
the `bench-results-<impl>` artifact of its Bench run:

| File | Contents |
|---|---|
| `<impl>-gha-x86-bench.json` | `bench_order_book` throughput (Google Benchmark JSON) |
| `<impl>-gha-x86-latency.json` | `latency_order_book` percentiles per message type |
| `parse-gha-x86-bench.json` | `bench_itch_parse` from the same run |
| `gha-x86-machine.txt` | Runner `lscpu`, memory, compiler, TSC flags |
| `book.txt`, `latency.txt`, `parse.txt` | Console output |

Throughput is the median of the repetitions over the full day. Latency is per
message apply.

### Throughput (full day, ns/msg, gha-x86)

| Step | ns/msg | Notes |
|---|---|---|
| Parse only (`bench_itch_parse`, `parse_book`) | 8.0 | Floor: no book work. cv 0.07% |
| v1 | **325.4** | 87.5 s/day, 3.07 M msgs/s. 3 reps, cv 12.1%: roughly 277 / 325 / 353 ns/msg ([v1/](results/v1/)) |
| v2 + O1 pool | – | |
| v2 + O2 hash map | – | |
| v2 + O3 FIFO | – | Expected to cost a little |
| v2 + O4 vector levels | – | |
| v2 final | – | |

v1 run: AMD EPYC 7763 (Zen 3), 2 cores / 4 threads, 15 GB, GCC 13.3. The v1
per-repetition times were reconstructed from mean, median and stddev (that run
only saved aggregates); later runs keep every repetition.

### Latency (ns per message apply, gha-x86)

Parse + apply of one message, timed with `rdtsc` (0.41 ns/tick). An empty timer
pair costs 25 ns mean / 30 ns p50 and is included in every value; readings
move in ~10 ns steps on these VMs.

| Impl | Type | count | mean | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| v1 | **book (A F E C X D U)** | 263.2 M | 382 | **310** | 741 | **1,483** | 2,094 | 1.83 ms |
| v1 | A add | 117.1 M | 297 | 240 | 561 | 972 | 1,593 | 1.83 ms |
| v1 | D delete | 114.4 M | 413 | 351 | 791 | 1,453 | 1,994 | 107 µs |
| v1 | U replace | 21.6 M | 686 | 561 | 1,352 | 1,933 | 3,246 | 71 µs |
| v1 | E execute | 5.7 M | 443 | 391 | 852 | 1,553 | 2,094 | 45 µs |
| v1 | X cancel | 2.8 M | 253 | 200 | 461 | 1,012 | 1,583 | 53 µs |
| v1 | F add (MPID) | 1.5 M | 318 | 220 | 621 | 1,252 | 2,735 | 681 µs |
| v1 | C execute w/ price | 0.1 M | 287 | 171 | 561 | 1,353 | 1,913 | 18 µs |
| v2 | book (A F E C X D U) | – | – | – | – | – | – | – |

Source: [v1/v1-gha-x86-latency.json](results/v1/v1-gha-x86-latency.json).
Replay check: 268,744,780 messages, 0 book errors, 0 live orders at end of day.

### v1 analysis

- **The book is ~97% of the time**: 325 ns/msg total against an 8 ns parse floor.
- **Replace is the slowest message** (p50 561 ns): a lookup, a full remove and an add.
- **Deletes and executes are slower than adds** (p50 351 and 391 vs 240 ns).
  Removing means finding an order allocated long ago, likely out of cache;
  an add writes into memory the allocator recently freed, still warm. This is
  the cost O2 (flat hash table, one miss per lookup) targets. Not yet
  confirmed with a profiler.
- **Moderate tail**: p99 ≈ 4.8x p50, p99.9 = 2.1 µs. Only 54 of 263 M
  messages took over 65 µs; the 1.83 ms max is a single outlier (VM pause or
  page fault), not the code.
- **Throughput is noisy run to run** (cv 12% vs < 4% for parsing): v1 is bound
  by cache misses on ~1.9 M scattered nodes, so it depends on the shared L3
  and memory bandwidth that neighbouring VMs compete for.
- The per-message mean (382 ns) is above the throughput number (325 ns): the
  timer costs ~25 ns and stops consecutive messages overlapping.
- **For v2's percentiles**, the ~30 ns timer cost and ~10 ns steps are a large
  share of a ~30–50 ns message; rely on throughput for absolute numbers and on
  latency for the shape of the distribution.
