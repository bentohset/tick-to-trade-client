# MoldUDP64

> Assisted by AI to enhance learning.

Nasdaq's lightweight transport for delivering a sequenced stream of messages
over UDP multicast. It carries ITCH 5.0 in this project. MoldUDP64 adds
**session identity, sequence numbers, and batching**, and nothing else. It
does not guarantee delivery. The receiver detects gaps and asks a separate
re-request server to fill them.

- Spec: *MoldUDP64 Protocol Specification v1.00* (Nasdaq)
- Code: `src/feed/moldudp64/session.*` (sequencing, gap detection),
  `src/feed/moldudp64/recovery.*` (re-requests), `src/net/udp_multicast_rx.*`
- Producer for tests: `apps/itch_replay` wraps a raw ITCH file in MoldUDP64 packets

## Wire format

All integers are **big-endian, unsigned**. Alpha fields are ASCII padded with spaces.

### Downstream packet (multicast, server → client)

| Offset | Len | Field           | Notes                                                     |
|-------:|----:|-----------------|-----------------------------------------------------------|
| 0      | 10  | Session         | Alpha. Identifies the session (typically changes daily)   |
| 10     | 8   | Sequence Number | Sequence number of the **first** message in this packet    |
| 18     | 2   | Message Count   | Number of message blocks that follow (see special values) |
| 20     | …   | Message Blocks  | `Message Count` blocks, back to back                      |

Header size: **20 bytes**.

### Message block

| Offset | Len | Field          | Notes                                      |
|-------:|----:|----------------|--------------------------------------------|
| 0      | 2   | Message Length | Length of the data that follows (excl. this field) |
| 2      | N   | Message Data   | One ITCH message, starting with its type byte |

Message *i* (0-based) in the packet has sequence number `SequenceNumber + i`.
Sequence numbers start at **1** for each session.

### Special packets

| Message Count | Meaning        | Sequence Number field holds   |
|---------------|----------------|-------------------------------|
| `0`           | Heartbeat      | Next expected sequence number |
| `0xFFFF`      | End of Session | Next expected sequence number |

Heartbeats are sent about once per second while the stream is idle. They let a
receiver detect a gap at the **tail** of the stream, where no further data
packet will arrive to reveal it.

### Re-request packet (unicast UDP, client → re-request server)

| Offset | Len | Field                   | |
|-------:|----:|-------------------------|-|
| 0      | 10  | Session                 | |
| 10     | 8   | Sequence Number         | First sequence number wanted |
| 18     | 2   | Requested Message Count | How many messages            |

The server replies by **unicast to the requester's source address**, using
ordinary downstream packets. A response may hold fewer messages than were
requested, because it is limited by the packet size. The client keeps
re-requesting until the gap is closed.

## Receiver state machine

```
expected = 1 (or first seq seen, if joining late)

on packet(seq, count):
    if session != current_session: handle session change
    if count == 0xFFFF: end of session
    if count == 0:      # heartbeat
        if seq > expected: gap [expected, seq)
        return
    last = seq + count            # one past the last message in packet
    if last <= expected: drop (duplicate)
    if seq > expected:  gap [expected, seq) -> request / buffer packet
    deliver messages with sequence >= expected, in order
    expected = last
```

Main cases:

- **Duplicate / old packet** (`seq + count <= expected`): drop the packet. This
  happens when A/B feeds are arbitrated or a retransmission arrives late.
- **Partial overlap** (`seq < expected < seq + count`): skip the first
  `expected - seq` blocks and deliver the rest.
- **Gap** (`seq > expected`): ITCH must be applied **strictly in order**. Options:
  1. Buffer packets that arrive after the gap, re-request the missing range,
     then drain the buffer once it is filled. This is the normal path.
  2. If the gap is too large or re-requests time out, fall back to a snapshot
     (in production, Nasdaq GLIMPSE over SoupBinTCP), rebuild the books, and
     resume from the snapshot's sequence number.
- **Tail gap**: detected through a heartbeat whose sequence number is greater
  than `expected`.

## Design notes for this project

- Parse in place over the receive buffer. Do not copy blocks. The ITCH parser
  takes a `(ptr, len)` view of each message block.
- `itch_replay` should support a **drop-rate / drop-specific-seq** option so
  that `tests/unit/moldudp64_gap_test.cpp` can exercise gap and recovery paths
  deterministically.
- `itch_replay` must also run a re-request responder. It already has the whole
  file, so it can serve any range by sequence number.
- Batch several ITCH messages per packet, staying under the MTU (about 1400
  bytes of payload is safe). Real feeds batch, and batching affects
  per-packet parse cost in benchmarks.
- On-disk Nasdaq ITCH samples use a 2-byte length prefix for each message.
  That prefix is the same as the MoldUDP64 message-block framing, so a replayer
  can copy blocks straight from the file into packets.

