# Order book optimizations

An AI-assisted Analysis on benchmarks

For reference, the first version will be called v1 and the optimized version be v2.

Related: [architecture.md](../architecture.md) (threads, memory rules),
[protocols/itch.md](../protocols/itch.md) (message semantics).

## Goal

Cut the cost of applying one ITCH message to the order book from ~350 ns (v1),
with **identical output**: every message of the same full trading day.

Speed is measured in two ways (see [Measurement](#measurement)):
- **Throughput**: ns/msg averaged over a full replay (Google Benchmark).
- **Latency distribution**: p50 / p90 / p99 / p99.9 / max of individual
  message applies, per message type

## Workload

Measured on `12302019.NASDAQ_ITCH50` (full trading day).


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

- Orders are addressed by `uint32_t` index, not pointer: half the size and
  indices stay valid if the pool is ever relocated.
- Capacity: **4 M** orders (2× the measured 1.92 M peak), 128 MB. Allocated
  and touched (prefaulted) at startup.
- Free list using `next` of free slots: O(1) alloc and free, no
  system calls.
- Pool exhaustion is counted as an error (new `Errors::pool_full`) and the
  add is dropped. Never grow on the hot path; size it from data instead.

### O2. Open-addressing order map

Replace `std::unordered_map` with a flat hash table: `ref -> pool index`.

```cpp
struct Slot { OrderRef ref; uint32_t index; };   // 16 bytes with padding; ref 0 = empty
```

- **Capacity 8 M slots** (power of two), load factor ≤ 0.24 at the 1.92 M
  peak, 128 MB. A smaller table (4 M slots, load ≤ 0.48) is worth considering
  too: half the memory, slightly longer probes.
- **Linear probing**: a lookup reads consecutive slots, usually in one cache
  line, so typically **one cache miss** per lookup instead of two or three.
- **Backward-shift deletion**, no tombstones. The workload deletes as often as
  it inserts (140 M each), and tombstones would pile up and lengthen probes
  all day.
- **Hash:** a multiplicative (Fibonacci) mix, `(ref * 0x9E3779B97F4A7C15) >> (64 - bits)`.
  Identity hashing can be used because refs are roughly sequential,
  but they are not monotonic and only ~45% dense.
- Ref 0 is reserved as "empty". The lowest order ref on the sample day is 42;
  `P` messages send 0 but never enter the book. An add with ref 0 is counted
  as an error.

### O3. Intrusive FIFO per price level

Each level keeps its orders in time priority as a doubly linked list using
`Order::prev/next`, with `head` (oldest) and `tail` (newest) in the
level.

- Unlink any order in O(1) given its pool index; no search within the level.
- A replace (`U`) is unlink + push to the back, matching exchange priority.

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
- Find: linear scan from the back for the first N levels (N ≈ 8–16, configurable),
  then binary search. Contiguous memory, so the scan is cache-friendly.
- `reserve()` each side at symbol setup (e.g. 64 levels) so normal days never
  reallocate during trading; busy symbols grow once early in the day.
- Orders cannot store a level index: inserting a level shifts the ones after
  it. They store their price and find the level by price (cheap near the top).

### O5. No allocation after startup

- `books_` and per-symbol state are sized when `R` messages arrive, which is
  before trading starts.
- Pool, hash table and level vectors are preallocated and prefaulted.
- A debug allocation hook (counting `operator new` calls during replay after
  the first `S 'Q'` system event) to assert zero allocations in tests.

### Not in v2

- Multi-threading, SIMD, prefetching, huge pages. Huge pages (2 MB) for the
  pool and hash table is feasible: 256 MB of 4 KB pages is 65 k TLB entries' worth.

## Correctness

v1 lives on the `feat/orderbook-v1` branch; v2 replaced it in place on `main`
with the same public interface (`BookManager`, `OrderBook` queries).

1. **Unit tests**: `book_manager_test` (28 message-level tests written for v1)
   passes unchanged on v2. New tests cover `OrderBook`, `PriceLevel` (FIFO
   links), `OrderMap` (including backward-shift deletion against
   `std::unordered_map`) and `OrderPool`.
2. **Full-day replay**: 268,744,780 messages, all error counters 0, 0 live
   orders at end of day: the same result as v1.
3. **Decomposed replay check**: `latency_order_book_micro --verify` replays the
   day through the components and through `BookManager` and compares every
   book's full depth (passes for v2).

## Measurement

Same input, same machine, Release (`-O3 -DNDEBUG`), file prefaulted in memory.

### Throughput: `bench/bench_order_book`

Google Benchmark, replaying the file through `BookManager` once per
iteration. Reports ns/msg and messages/s.

```bash
build-release/bench/bench_order_book FILE --benchmark_repetitions=5 \
    --benchmark_display_aggregates_only=true \
    --benchmark_out=docs/design/order-book-results/<impl>/<impl>-<machine>-bench.json --benchmark_out_format=json
```

### Latency distribution: `bench/latency_order_book`

Replays the file and times **each message's book apply** individually with
`core/clock.hpp`, recording into a histogram per message type. Writes p50,
p90, p99, p99.9, max and the timer overhead as JSON:

```bash
build-release/bench/latency_order_book FILE --label <impl>-<machine> \
    > docs/design/order-book-results/<impl>/<impl>-<machine>-latency.json
```

Timer: `rdtsc` on the x86-64 runners, roughly 20–40 cycles (~7–15 ns) per
read with sub-ns resolution. The JSON reports the cost of an empty timer read
pair next to the results; nothing is subtracted.

Mean latency under per-message timing comes out higher than the throughput
number: reading the timer around every message stops the CPU from overlapping
consecutive messages.

### Component latency: `bench/latency_order_book_micro`

Measures `map_find`,`map_insert`, `map_erase`, `book_add`, `book_reduce`, `book_remove`.

```bash
build-release/bench/latency_order_book_micro FILE --label <impl>-<machine> --verify \
    > docs/design/order-book-results/<impl>/<impl>-<machine>-latency-micro.json
```

Each value includes one timer pair (~25–30 ns); timed components don't add
up to the untimed ns/msg.

### Environment: GitHub Actions

All results come from GitHub-hosted runners (`gha-x86`: `ubuntu-24.04`,
x86-64, 4 vCPU, 16 GB RAM for a public repo).

`.github/workflows/bench.yml`  builds Release, runs bench tests and uploads the 
JSON as the `bench-results-<label>` artifact. It uses the
whole day by default; `itch_bytes` can select a slice from the start of the
day instead.

Each version is benchmarked in its own workflow run. Runners are shared VMs,
so to keep runs comparable, we do:
- **check the CPU model** in `gha-x86-machine.txt`: only compare runs on the
  same model (currently AMD EPYC 7763); rerun if a runner lands on different
  hardware;
- use **5 repetitions** and compare medians, with the minimum as a second view;
- every repetition is kept in the JSON (`--benchmark_display_aggregates_only`
  only trims console output);
- treat p99.9 and max as indicative: hypervisor may pause and land in the tail.

## Results

Each version has a folder, [`docs/design/order-book-results/<impl>/`](order-book-results/),
holding the result artifact of its Bench run:

| File | Contents |
|---|---|
| `<impl>-gha-x86-bench.json` | `bench_order_book` throughput (Google Benchmark JSON) |
| `<impl>-gha-x86-latency.json` | `latency_order_book` percentiles per message type |
| `<impl>-gha-x86-latency-micro.json` | `latency_order_book_micro` percentiles per component (v2 onwards) |
| `parse-gha-x86-bench.json` | `bench_itch_parse` from the same run |
| `gha-x86-machine.txt` | Runner `lscpu`, memory, compiler, TSC flags |
| `book.txt`, `latency.txt`, `latency-micro.txt`, `parse.txt` | Console output |

Both runs: GitHub Actions, AMD EPYC 7763 (Zen 3), 2 cores / 4 threads, 15 GB,
GCC 13.3, whole 12302019 day (268,744,780 messages). Both replays: 0 book
errors, 0 live orders at end of day.

### Summary

| | v1 | v2 | Speedup |
|---|---|---|---|
| Throughput, median ns/msg | 263.2 | **146.3** | **1.80x** |
| Throughput, best repetition | 248.5 | **135.7** | 1.83x |
| Book cost (throughput minus 8.0 ns parse) | ~255 | **~138** | 1.85x |
| Latency p50 (all book messages) | 321 | **160** | 2.0x |
| Latency p99 | 1,493 | **611** | 2.4x |
| Latency p99.9 | 2,054 | **1,012** | 2.0x |
| Latency max | 1.84 ms | 0.61 ms | 3.0x |
| Throughput cv | 10.8% | 4.0% | |

### Throughput (full day, ns/msg, gha-x86)

| Step | ns/msg (median) | Repetitions | Notes |
|---|---|---|---|
| Parse only (`bench_itch_parse`, `parse_book`) | 7.97 | | Floor: no book work. Same in both runs (cv 0.2%) |
| v1 | 263.2 | 325.3 / 281.5 / 263.2 / 248.5 / 262.2, cv 10.8% | [v1/](order-book-results/v1/). With warm-up pass |
| **v2** (O1–O4 together) | **146.3** | 148.0 / 147.8 / 146.3 / 138.5 / 135.7, cv 4.0% | [v2/](order-book-results/v2/). With warm-up pass |


### Latency per message (ns, gha-x86)

Parse + apply of one message, timed with `rdtsc` (0.41 ns/tick). An empty timer
pair costs 25 ns mean / 30 ns p50 and is included in every value; readings move
in ~10 ns steps. Single pass, no warm-up, for both versions.

| Type | count | v1 p50 | **v2 p50** | v1 p99 | **v2 p99** | v1 p99.9 | **v2 p99.9** | v1 max | v2 max |
|---|---|---|---|---|---|---|---|---|---|
| **book (A F E C X D U)** | 263.2 M | 321 | **160** | 1,493 | **611** | 2,054 | **1,012** | 1.84 ms | 610 µs |
| A add | 117.1 M | 251 | **160** | 992 | **451** | 1,553 | **561** | 1.84 ms | 473 µs |
| D delete | 114.4 M | 381 | **140** | 1,463 | **571** | 1,954 | **811** | 269 µs | 610 µs |
| U replace | 21.6 M | 581 | **341** | 1,904 | **972** | 3,046 | **1,643** | 209 µs | 531 µs |
| E execute | 5.7 M | 411 | **160** | 1,563 | **551** | 2,044 | **771** | 213 µs | 92 µs |
| X cancel | 2.8 M | 211 | **100** | 1,062 | **431** | 1,573 | **661** | 73 µs | 51 µs |
| F add (MPID) | 1.5 M | 210 | **170** | 1,233 | **481** | 2,715 | **2,064** | 722 µs | 118 µs |
| C execute w/ price | 0.1 M | 180 | **110** | 1,383 | **461** | 1,893 | **701** | 66 µs | 16 µs |

Means: v1 396 ns, v2 192 ns (book). Sources:
[v1](order-book-results/v1/v1-gha-x86-latency.json),
[v2](order-book-results/v2/v2-gha-x86-latency.json).

### Component latency, v2 (ns, gha-x86)

Each value includes one timer pair (~25–30 ns; the `timer` row).
`--verify`: OK, all 8,906 books identical to `BookManager`, 0 messages skipped.

| Component | count | mean | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| timer (empty pair) | 10 M | 25 | 30 | 30 | 30 | 30 |
| `map_find` | 30.2 M | 81 | 50 | 140 | 291 | 471 |
| `map_insert` | 140.3 M | **143** | **140** | 160 | 411 | 491 |
| `map_erase` | 140.3 M | 79 | 50 | 150 | 291 | 481 |
| `book_add` | 140.3 M | 69 | 60 | 100 | 240 | 491 |
| `book_reduce` | 4.3 M | 60 | 50 | 80 | 230 | 421 |
| `book_remove` | 140.3 M | 109 | 80 | 191 | 431 | 781 |

Source: [v2-gha-x86-latency-micro.json](order-book-results/v2/v2-gha-x86-latency-micro.json).

### v1 analysis

- **The book was ~97% of the time**: 263 ns/msg total against an 8 ns parse.
- **Replace was the slowest message** (p50 581 ns): a lookup, a full remove and an add.
- **Deletes and executes were slower than adds** (p50 381 and 411 vs 251 ns):
  following `std::unordered_map` and `std::map` node pointers to orders
  allocated long ago, then freeing nodes.

### v2 analysis

- **1.8x faster throughput, 2–2.4x better latency percentiles.** The gain is
  larger in the tail (p99 2.4x) than the median (2.0x): with no allocation on
  the hot path, slow outliers became rare. Run-to-run noise fell too
  (cv 10.8% -> 4.0%).
- **Every message type improved.** Deletes gained most (p50 381 -> 140 ns,
  2.7x) and are now faster than adds: removing no longer walks heap nodes or 
  frees memory, it's a hash erase with an O(1) unlink.
- The component breakdown:
  - **`map_insert` is the single largest cost** (p50 140 ns, ~110 ns net of the
    timer), against 50 ns for `map_find` and `map_erase`. The multiplicative
    (Fibonacci) hash sends every new ref to a random slot of the
    128 MB table, so almost every insert misses the cache. Finds and erases are
    cheap because most target orders added recently (slots are
    still cached)
  - Net of the ~30 ns timer, an add is ~insert + `book_add` ≈ 110 + 30 ns,
    which accounts for A's p50 (160 - 30 = 130 ns). A delete is 110 ns net; 
    the rest is parsing, the crossed-book check and freeing the pool slot. 
  - `book_remove` (p50 80, mean 109) costs more than `book_add` (60 / 69):
    unlinking writes to both neighbouring orders which may not be cached, and
    an emptied level is erased from the vector.
- **Tail**: p99.9 is ~1 µs and max 0.6 ms. Max are single events (VM pauses,
  page faults) and vary between runs;

### Next steps

1. **Hash for locality (O2b).** ITCH refs are roughly increasing (~45% dense),
   so taking the low bits of the ref (`ref & mask`) instead of a multiplicative
   hash would put consecutive new orders in neighbouring slots, 4 per cache
   line, turning most `map_insert` misses into hits. Risk: clustering for
   long-lived orders and the ~25% of adds whose refs arrive out of order.
   Measure `map_insert` and throughput;
2. **Smaller hash slots**. A slot is 16bytes (8-byte ref + 4-byte index + padding).
   Storing only the index plus part of the ref can give 8-byte slots (8 per cache line).
   Risk: tag match on different ref costs one extra pool read. 32-bit tags only happen when
   2 live refs share their low 32 bits which has low chance of happening within a day;
3. **Huge pages** for the pool and the hash table (256 MB of 4 KB pages), to
   cut TLB misses on the random accesses that remain. `std::vector` does not let us control alignment
   or page size, so we should allocate storage ourselves with a new `huge_array` container.
   Currently, github runner machines have about 8MB of TLB coverage against 280MB of randomly accessed memory
   from our OrderPool+orderMap+price-level vectors (a given page would be in TLB only about 3% of the time).
   By increasing page size, we can reduce TLB miss and TLB page walking (~100ns on hyper-v VMs).

