# SoupBinTCP

> Assisted by AI to enhance learning.

Nasdaq's session-layer protocol over TCP. It provides login, heartbeats,
and **sequenced, replayable delivery from server to client**. OUCH (and
GLIMPSE) run on top of it. It is the TCP counterpart of MoldUDP64.

- Spec: *SoupBinTCP Specification v4.0* (Nasdaq)
- Code: `src/gateway/ouch/soupbintcp_session.*`, over `src/net/tcp_client.*`
- Server side for tests: `apps/mock_exchange`

GLIMPSE: snapshot service for ITCH feeds. Gives the client the full current state of the book at a single point in time, so it can
start processing the live feed from middle of the day instead of from the first message.

## Key property: only one direction is sequenced

| Direction       | Packet type          | Sequenced? | Recovered on reconnect? |
|-----------------|----------------------|-----------|-------------------------|
| Server → client | Sequenced Data `S`   | Yes (implicit) | **Yes**, by requesting a sequence number at login |
| Client → server | Unsequenced Data `U` | No        | **No**. The application layer (OUCH) must handle it |

Sequence numbers are **not sent on the wire** with each message. They are
implicit. The client learns the next sequence number from Login Accepted and
counts each `S` packet after that.

## Framing

Every packet has this layout:

| Offset | Len | Field         | Notes                                            |
|-------:|----:|---------------|--------------------------------------------------|
| 0      | 2   | Packet Length | Big-endian. Counts bytes **after** this field (type + payload) |
| 2      | 1   | Packet Type   | ASCII char                                       |
| 3      | …   | Payload       | Type-specific                                    |

TCP is a byte stream, so the reader must handle partial packets and several
packets per `recv()`. Read the 2-byte length first, then wait until
`length` more bytes are buffered.

Alpha fields are space-padded ASCII. Numeric fields in the session-layer
packets (sequence numbers) are **ASCII digits, left-padded with spaces**, not
binary integers.

## Packet types

### Both directions

| Type | Name  | Payload                  |
|------|-------|--------------------------|
| `+`  | Debug | Free text. Ignore it      |


### Server → client

| Type | Name              | Payload                                                         |
|------|-------------------|-----------------------------------------------------------------|
| `A`  | Login Accepted    | Session (10, alpha, left-padded) + Seq Num (20, numeric ASCII, left-padded) |
| `J`  | Login Rejected    | Reject Reason Code (1): `A` = not authorized, `S` = requested sesh not available |
| `S`  | Sequenced Data    | One application message (e.g. an OUCH outbound message)         |
| `U`  | Unsequenced Data  | One application message, not sequenced                          |
| `H`  | Server Heartbeat  | (empty)                                                         |
| `Z`  | End of Session    | (empty). No more data will be sent for this session             |

In Login Accepted, the Sequence Number is the number of the **next**
sequenced message the server will send.

### Client → server

| Type | Name              | Payload                                                         |
|------|-------------------|-----------------------------------------------------------------|
| `L`  | Login Request     | see below                                                       |
| `U`  | Unsequenced Data  | One application message (e.g. an OUCH Enter Order)              |
| `R`  | Client Heartbeat  | (empty)                                                         |
| `O`  | Logout Request    | (empty)                                                         |

#### Login Request (`L`): 46-byte payload, packet length 47

| Offset* | Len | Field                     | Notes                                       |
|--------:|----:|---------------------------|---------------------------------------------|
| 0       | 6   | Username                  | Alpha, right-padded with spaces             |
| 6       | 10  | Password                  | Alpha, right-padded with spaces             |
| 16      | 10  | Requested Session         | Left-padded. All spaces = current session   |
| 26      | 20  | Requested Sequence Number | Numeric ASCII, left-padded. `0` = start from the most recent; `n` = replay from msg `n` |

\* Offsets are relative to the start of the payload, after the 3-byte header.

## Session lifecycle

```
client                                 server
  | ---- TCP connect -------------------> |
  | ---- L (session, next_seq) ---------> |
  | <--- A (session, next_seq) ---------- |   or J, then the server closes
  | <--- S S S ... (replayed backlog) --- |
  | <--- S ... (live) ------------------- |
  | ---- U (orders) --------------------> |
  | <--> H / R heartbeats when idle ----- |
  | ---- O -----------------------------> |   or the server sends Z
```

## Heartbeats and timeouts

- Each side sends a heartbeat (`H` or `R`) if it has sent **nothing else for
  1 second**.
- If **15 seconds** pass with nothing received, treat the link as dead and
  disconnect.
- Run the heartbeat timer in the same loop that polls the socket, so a stuck
  hot path shows up as missed heartbeats.

## Reconnect / recovery

1. Persist the session ID and the **last sequenced message number processed**.
2. On reconnect, send `L` with that session and `last_processed + 1`.
3. The server replays every `S` message from that number onward. The client
   gets each execution, cancel, and other event **exactly once**, provided it
   only advances `last_processed` after it has applied the message to order state.
4. Messages the client sent while disconnected, or that were in flight when
   the link dropped, are **not** recovered by SoupBinTCP. See
   [ouch.md](ouch.md) for how OUCH order tokens make resending safe.

## Design notes for this project

- `soupbintcp_session` should know nothing about OUCH. It exposes
  `send_unsequenced(span)` and an `on_sequenced(span, seq)` callback.
- The mock exchange needs a **kill connection** switch, a persisted outbound
  message log (so it can replay), and a way to enforce login rejection.
  `tests/integration/gateway_session_test.cpp` checks that disconnecting and
  reconnecting mid-session yields every missed execution exactly once.
- Use `TCP_NODELAY` (`src/net/socket_opts.hpp`). Orders are small and must not
  wait due to batching small packets (Nagle's algo).

