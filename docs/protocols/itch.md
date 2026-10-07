# Nasdaq TotalView-ITCH 5.0

> Assisted by AI to enhance learning.

A binary, order-by-order market data feed. It reports every add, execute,
cancel, delete, and replace of displayed orders on the Nasdaq book, which is
enough to rebuild the **full depth** order book for every symbol. It is
delivered over [MoldUDP64](moldudp64.md) in real time, or in files for
historical data.

- Spec: *Nasdaq TotalView-ITCH 5.0*
- Sample data: `https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/` (gzip'd day files,
  e.g. `*.NASDAQ_ITCH50.gz`). See `scripts/fetch_itch_sample.sh`
- Code: `src/feed/itch/messages.hpp` (layouts), `parser.hpp` (decode and
  dispatch), `symbol_directory.hpp`, and `src/feed/book/*`

## Data types

| Type         | Encoding                                                          |
|--------------|-------------------------------------------------------------------|
| Integer      | Unsigned, **big-endian**, 2/4/8 bytes                              |
| Timestamp    | **6-byte** big-endian integer, nanoseconds since midnight (ET)     |
| Price (4)    | 4-byte integer, **4 implied decimal places** (`1234500` = $123.45) |
| Price (8)    | 8-byte integer, 8 implied decimal places (MWCB levels only)        |
| Alpha        | ASCII, left-justified, right-padded with spaces (e.g. Stock is 8 bytes) |

Keep prices as integers (`uint32_t` ticks) throughout the book. Do not convert
them to floating point.

## File framing

In the historical files, each message is starts with a **2-byte big-endian
length**. The length does not include itself. This is the same framing as a
MoldUDP64 message block. Memory-map the file and walk `[len][msg][len][msg]...`.

## Common header (all messages, 11 bytes)

| Offset | Len | Field           | Notes                                              |
|-------:|----:|-----------------|----------------------------------------------------|
| 0      | 1   | Message Type    | ASCII char                                         |
| 1      | 2   | Stock Locate    | Day-specific index for the symbol (0 = not symbol-specific). Use it as an array index for books |
| 3      | 2   | Tracking Number | Nasdaq internal                                     |
| 5      | 6   | Timestamp       | ns since midnight                                   |

## Message catalogue

Sizes include the type byte and exclude the file/MoldUDP64 length prefix.

| Type | Name                                   | Size | Affects book? |
|------|----------------------------------------|-----:|---------------|
| `S`  | System Event                           | 12   | Lifecycle     |
| `R`  | Stock Directory                        | 39   | Symbol setup  |
| `H`  | Stock Trading Action                   | 25   | Trading state |
| `Y`  | Reg SHO Short Sale Price Test          | 20   | –             |
| `L`  | Market Participant Position            | 26   | –             |
| `V`  | MWCB Decline Level                     | 35   | –             |
| `W`  | MWCB Status                            | 12   | –             |
| `K`  | IPO Quoting Period Update              | 28   | –             |
| `J`  | LULD Auction Collar                    | 35   | –             |
| `h`  | Operational Halt                       | 21   | –             |
| `A`  | Add Order (no MPID)                    | 36   | **Yes**       |
| `F`  | Add Order with MPID Attribution        | 40   | **Yes**       |
| `E`  | Order Executed                         | 31   | **Yes**       |
| `C`  | Order Executed with Price              | 36   | **Yes**       |
| `X`  | Order Cancel (partial)                 | 23   | **Yes**       |
| `D`  | Order Delete                           | 19   | **Yes**       |
| `U`  | Order Replace                          | 35   | **Yes**       |
| `P`  | Trade (non-cross)                      | 44   | No (hidden liquidity) |
| `Q`  | Cross Trade                            | 40   | No            |
| `B`  | Broken Trade                           | 19   | No            |
| `I`  | NOII (Net Order Imbalance Indicator)   | 50   | No            |
| `N`  | Retail Price Improvement Indicator     | 20   | No            |
| `O`  | Direct Listing w/ Capital Raise Price Discovery | 48 | No   |

A parser should skip unknown types by length rather than fail. The length
comes from the framing, so this is always possible.

## Order book messages (byte offsets)

All offsets are from the start of the message, so the header occupies 0–10.

**`A` Add Order (36)**

| Off | Len | Field                  |
|----:|----:|------------------------|
| 11  | 8   | Order Reference Number |
| 19  | 1   | Buy/Sell Indicator (`B`/`S`) |
| 20  | 4   | Shares                 |
| 24  | 8   | Stock                  |
| 32  | 4   | Price (4)              |

**`F` Add Order with MPID (40)**: same as `A`, plus `36 | 4 | Attribution (MPID)`.

**`E` Order Executed (31)**

| Off | Len | Field                  |
|----:|----:|------------------------|
| 11  | 8   | Order Reference Number |
| 19  | 4   | Executed Shares        |
| 23  | 8   | Match Number           |

The execution is at the resting order's price.

**`C` Order Executed with Price (36)**

| Off | Len | Field                  |
|----:|----:|------------------------|
| 11  | 8   | Order Reference Number |
| 19  | 4   | Executed Shares        |
| 23  | 8   | Match Number           |
| 31  | 1   | Printable (`Y`/`N`). `N` means do not count toward volume |
| 32  | 4   | Execution Price (4)    |

**`X` Order Cancel (23)**

| Off | Len | Field                  |
|----:|----:|------------------------|
| 11  | 8   | Order Reference Number |
| 19  | 4   | Cancelled Shares       |

**`D` Order Delete (19)**

| Off | Len | Field                  |
|----:|----:|------------------------|
| 11  | 8   | Order Reference Number |

**`U` Order Replace (35)**

| Off | Len | Field                           |
|----:|----:|---------------------------------|
| 11  | 8   | Original Order Reference Number |
| 19  | 8   | New Order Reference Number      |
| 27  | 4   | Shares                          |
| 31  | 4   | Price (4)                       |

**`P` Trade, non-cross (44)**: Order Ref (11, 8), Side (19, 1), Shares (20, 4),
Stock (24, 8), Price (32, 4), Match Number (36, 8). This reports an execution
against a **non-displayed** order. It never touches the visible book.

## Book-building rules

| Msg   | Action                                                                     |
|-------|----------------------------------------------------------------------------|
| `A`/`F` | Insert order `{ref, side, shares, price}` into book `[locate]` at the tail of its price level (time priority) |
| `E`   | `shares -= executed`. Remove the order when it reaches 0                   |
| `C`   | Same as `E`. The price may differ from the order's price (e.g. crosses); the book uses the order's resting price |
| `X`   | `shares -= cancelled`. Remove the order when it reaches 0                  |
| `D`   | Remove the order entirely                                                  |
| `U`   | Delete the original order, then add a **new** order with the new ref, shares, and price. **Side and symbol are inherited** from the original. The new order loses time priority |

Consequences for the data structures:

- `E`, `C`, `X`, `D`, and `U` carry **only the order reference number**, not
  the symbol or side. The book manager needs a global
  `order_ref -> Order*` map. This is the source of the "O(1) cancel by order
  id" requirement in `order_book.hpp`.
- Order reference numbers are unique for the day across all symbols.
- Stock Locate codes are assigned per day starting at 1. `R` (Stock Directory)
  messages arrive before trading and map locate to symbol. They also give the
  round lot size and other attributes. Size book arrays by locate.
- Nasdaq lists about 8–12k symbols and a full day has hundreds of millions of
  messages. Preallocate orders in an object pool and use intrusive lists per
  price level.

## System Event codes (`S`, field at offset 11)

| Code | Meaning                  |
|------|--------------------------|
| `O`  | Start of Messages        |
| `S`  | Start of System Hours    |
| `Q`  | Start of Market Hours    |
| `M`  | End of Market Hours      |
| `E`  | End of System Hours      |
| `C`  | End of Messages          |


## Trading Action (`H`)

Stock (11, 8), Trading State (19, 1): `H` halted, `P` paused, `Q` quotation
only, `T` trading. Reserved (20, 1), Reason (21, 4). The strategy and risk
layers should not send orders for a symbol that is not in state `T`.

## Stock Directory (`R`) layout

| Off | Len | Field                        |
|----:|----:|------------------------------|
| 11  | 8   | Stock                        |
| 19  | 1   | Market Category              |
| 20  | 1   | Financial Status Indicator   |
| 21  | 4   | Round Lot Size               |
| 25  | 1   | Round Lots Only              |
| 26  | 1   | Issue Classification         |
| 27  | 2   | Issue Sub-Type               |
| 29  | 1   | Authenticity (`P` live / `T` test) |
| 30  | 1   | Short Sale Threshold Indicator |
| 31  | 1   | IPO Flag                     |
| 32  | 1   | LULD Reference Price Tier    |
| 33  | 1   | ETP Flag                     |
| 34  | 4   | ETP Leverage Factor          |
| 38  | 1   | Inverse Indicator            |


## Validation strategy

- `book_dump` and the golden snapshots in `tests/data/` should match the
  book's top levels at fixed timestamps.
- Useful invariants: no crossed book after each message outside of crosses or
  auctions, no order removed that does not exist, share counts never negative,
  and every `E`/`C`/`X`/`D`/`U` refers to a known order ref.
- Use `static_assert(sizeof(...) == N)` or explicit offset constants for each
  layout in `messages.hpp`. Read fields through `endian.hpp` helpers using
  `memcpy` and byteswap, never through reinterpret_cast of packed structs.
  Messages are not aligned.

