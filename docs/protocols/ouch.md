# OUCH

> Assisted by AI to enhance learning.

Nasdaq's binary order entry protocol. The client sends orders and
cancel/replace requests. The exchange sends acknowledgements, executions,
cancels, and rejects. OUCH runs on top of [SoupBinTCP](soupbintcp.md):

- Client → exchange messages travel in SoupBinTCP **Unsequenced Data (`U`)** packets.
- Exchange → client messages travel in SoupBinTCP **Sequenced Data (`S`)**
  packets, so they can be replayed after a reconnect.

This document covers **OUCH 4.2**, which has fixed-size messages and is the
simplest version to implement first. A summary of the differences in OUCH 5.0
is at the end.

- Spec: *Nasdaq OUCH 4.2* (and *OUCH 5.0* for the newer layout)
- Code: `src/gateway/ouch/codec.hpp` (encode/decode),
  `ouch_gateway.*` (maps to `gateway/common/order_types.hpp`),
  `soupbintcp_session.*`
- Server side for tests: `apps/mock_exchange`

## Data types

| Type      | Encoding                                                         |
|-----------|------------------------------------------------------------------|
| Integer   | Unsigned, big-endian                                             |
| Price     | 4-byte integer, **4 implied decimals**. This matches ITCH Price(4) |
| Timestamp | 8-byte integer, nanoseconds since midnight                       |
| Alpha     | ASCII, space-padded (Stock: 8, Order Token: 14, Firm: 4)         |


## Order Token

Each order is identified by a client-chosen **14-byte Order Token**:

- It must be **unique for the day** for the OUCH account.
- An Enter Order with a token that has already been used is **silently
  ignored**. This makes resending after a disconnect safe.
- All responses (Accepted, Executed, Canceled, and so on) echo the token,
  which is how the client correlates them with its own orders.
- A Replace assigns a **new** token. The order is identified by that token
  from then on.

Suggested scheme: `<session prefix><monotonic counter>`, zero-padded to 14
characters, with the counter persisted so a restart cannot reuse tokens.

## Inbound messages (client → exchange)

### `O` Enter Order (49 bytes)

| Off | Len | Field                          | Notes                                    |
|----:|----:|--------------------------------|------------------------------------------|
| 0   | 1   | Type = `O`                     |                                          |
| 1   | 14  | Order Token                    |                                          |
| 15  | 1   | Buy/Sell Indicator             | `B` buy, `S` sell, `T` sell short, `E` sell short exempt |
| 16  | 4   | Shares                         |                                          |
| 20  | 8   | Stock                          |                                          |
| 28  | 4   | Price                          |                                          |
| 32  | 4   | Time in Force                  | Seconds. `0` = IOC, `99998` = market hours, `99999` = system hours (day) |
| 36  | 4   | Firm                           | MPID                                     |
| 40  | 1   | Display                        | e.g. `Y` displayed, `N` non-displayed     |
| 41  | 1   | Capacity                       | e.g. `A` agency, `P` principal            |
| 42  | 1   | Intermarket Sweep Eligibility  | `Y`/`N`                                  |
| 43  | 4   | Minimum Quantity               | `0` = none                               |
| 47  | 1   | Cross Type                     | `N` = continuous market                  |
| 48  | 1   | Customer Type                  |                                          |

### `U` Replace Order (47 bytes)

| Off | Len | Field                         |
|----:|----:|-------------------------------|
| 0   | 1   | Type = `U`                    |
| 1   | 14  | Existing Order Token          |
| 15  | 14  | Replacement Order Token       |
| 29  | 4   | Shares                        |
| 33  | 4   | Price                         |
| 37  | 4   | Time in Force                 |
| 41  | 1   | Display                       |
| 42  | 1   | Intermarket Sweep Eligibility |
| 43  | 4   | Minimum Quantity              |

### `X` Cancel Order (19 bytes)

| Off | Len | Field                                       |
|----:|----:|---------------------------------------------|
| 0   | 1   | Type = `X`                                  |
| 1   | 14  | Order Token                                 |
| 15  | 4   | Shares: the new **remaining** size. `0` = cancel everything |

### `M` Modify Order (20 bytes)

Type (0, 1), Order Token (1, 14), Buy/Sell Indicator (15, 1), Shares (16, 4).
Used to change the side between sell / sell short / sell short exempt, or to
reduce shares.

## Outbound messages (exchange → client)

Every outbound message has an 8-byte **Timestamp** at offset 1, right after the type.

| Type | Name                     | Size | Key fields after timestamp |
|------|--------------------------|-----:|-----------------------------|
| `S`  | System Event             | 10   | Event Code (`S` start of day, `E` end of day) |
| `A`  | Accepted                 | 66   | Token, side, shares, stock, price, TIF, firm, display, **Order Reference Number (8)**, capacity, ISE, min qty, cross type, **Order State** (`L` live / `D` dead), BBO weight |
| `U`  | Replaced                 | 80   | Replacement token, side, shares, stock, price, TIF, firm, display, order ref, capacity, ISE, min qty, cross type, order state, **Previous Order Token**, BBO weight |
| `C`  | Canceled                 | 28   | Token (9, 14), **Decrement Shares** (23, 4), Reason (27, 1) |
| `D`  | AIQ Canceled             | –    | Cancel caused by self-match prevention (anti-internalization) |
| `E`  | Executed                 | 40   | Token (9, 14), Executed Shares (23, 4), Execution Price (27, 4), Liquidity Flag (31, 1), **Match Number** (32, 8) |
| `B`  | Broken Trade             | 32   | Token, Match Number, Reason |
| `J`  | Rejected                 | 24   | Token (9, 14), Reason (23, 1) |
| `P`  | Cancel Pending           | 23   | Token. The cancel is queued (e.g. during a cross) |
| `I`  | Cancel Reject            | 23   | Token. The cancel could not be applied |
| `T`  | Order Priority Update    | –    | Price, display, order ref changed (e.g. by reprice) |
| `M`  | Order Modified           | –    | Response to Modify Order |

Notes:

- **Accepted with Order State `D`** means the order was accepted but is
  already dead, for example an IOC that did not execute. A separate Canceled
  message may not follow, so the state machine must handle this case.
- **Canceled carries a decrement**, not the remaining size. The remaining size
  is `open_shares - decrement`. A partial cancel leaves the order live.
- **Executed** messages may arrive before Accepted is processed by
  application code, but never before it on the wire. They are sequenced.
- The Match Number is the same one that appears in ITCH `E`/`C`/`P`
  messages. That allows an execution to be tied to the market data that
  reflected it, which is useful for tick-to-trade analysis.

Common **cancel reasons**: `U` user requested, `I` immediate-or-cancel,
`T` timeout (TIF expired), `S` supervisory, `D` regulatory restriction,
`Q` self-match prevention, `Z` system cancel.

Common **reject reasons**: `T` test mode, `H` halted, `Z` shares exceed safety
threshold, `S` invalid stock, `D` invalid display type, `C` exchange closed,
`L` firm not authorized, `O` other.

## Order state machine (`gateway/common/order_state.*`)

| From              | Event                        | To                         |
|-------------------|------------------------------|----------------------------|
| NEW               | send `O`                     | PENDING_NEW                |
| PENDING_NEW       | `A` (state `L`)              | LIVE                       |
| PENDING_NEW       | `A` (state `D`)              | DONE (e.g. unfilled IOC)   |
| PENDING_NEW       | `J`                          | REJECTED                   |
| LIVE              | `E`, open qty > 0            | LIVE                       |
| LIVE              | `E`, open qty == 0           | FILLED                     |
| LIVE              | send `X`                     | PENDING_CANCEL             |
| LIVE              | send `U`                     | PENDING_REPLACE            |
| PENDING_CANCEL    | `C`, open qty == 0           | CANCELED                   |
| PENDING_CANCEL    | `C`, open qty > 0 (partial)  | LIVE                       |
| PENDING_CANCEL    | `I` (cancel reject)          | LIVE (or FILLED if already done) |
| PENDING_CANCEL    | `E`                          | stays pending; may become FILLED |
| PENDING_REPLACE   | `U` Replaced                 | LIVE under the new token   |
| PENDING_REPLACE   | `J` on the new token         | LIVE under the old token   |
| any live state    | `C` (unsolicited, e.g. TIF)  | CANCELED                   |

Track `orig_qty`, `open_qty`, `cum_exec_qty`, and the current token for each
order. Executions update the position through `risk/position_keeper`.
Executions can race a pending cancel or replace. An `E` can arrive after `X`
has been sent and before `C`.

## Reconnect without losing executions

1. Record the last processed SoupBinTCP sequence number **after** each
   outbound message has been applied to order state.
2. On reconnect, log in with `last + 1`. The exchange replays every missed
   Accepted, Executed, Canceled, and so on, so each is applied exactly once.
3. For orders still in `PENDING_NEW` after the replay (sent, but no response
   replayed), resend the identical Enter Order. Duplicate tokens are ignored,
   so this is safe. Alternatively, mark them unknown and wait.
4. Reconcile `position_keeper` against the mock exchange's records.
   `tests/integration/gateway_session_test.cpp` should do this.

## OUCH 5.0 differences (summary)

OUCH 5.0 is the current Nasdaq version.

- The 14-byte Order Token is replaced by a 4-byte binary **UserRefNum**, which
  must be **strictly increasing** per account per day. A separate optional
  14-byte **ClOrdID** is provided for client use.
- Prices are wider. Time in Force is a 1-byte code instead of seconds.
- Messages end with an **Appendage Length** plus optional tag-length-value
  **appendages** for less common fields (min qty, max floor, peg, and so on),
  so messages are no longer fixed size.
- Messages are renamed slightly, but the overall flow (Enter → Accepted →
  Executed / Canceled) is the same, so `order_state` can be shared.

