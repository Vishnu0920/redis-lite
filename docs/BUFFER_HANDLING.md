# Improved event-loop buffer handling

The event loop uses `InputBuffer` by default for incoming bytes. Removing a parsed
command advances a read offset instead of moving all remaining bytes. This
implements the idea in [Build Your Own Redis, section 6.9](https://build-your-own.org/redis/06b_event_loop_more):
keep separate positions for the allocation and its unread data, and reclaim or
grow storage when appending requires space.

## Select either implementation

Both implementations are compiled into the same executable. Choose at startup:

```bash
./build/server --mode event-loop --buffer regular
./build/server --mode event-loop --buffer offset
```

`regular` uses `RegularInputBuffer`, which restores the previous string append and
`erase(0, consumed)` behavior. `offset` uses the improved `InputBuffer` described
below. Omitting `--buffer` in event-loop mode selects `offset`. Options may appear
in either order. An explicit `--buffer` option requires event-loop mode; threaded
mode keeps its own existing buffer. Stop the server before switching and restart
on the same port. No rebuild is needed.

`server/main.cpp` parses the option into `InputBufferMode`, declared in
`server/runners/runners.h`. `runEventLoopServer()` selects a template instantiation once:
`runEventLoop<RegularInputBuffer>()` or `runEventLoop<InputBuffer>()`. The connection
type and handlers use that buffer type throughout. Both provide `view()`,
`append()`, `consume()`, `empty()`, and `clear()`, so the parser and I/O algorithm
are shared. There is no virtual dispatch or per-operation mode branch.

This isolates the input-buffer implementation for comparison. Output buffering,
optimistic writes, I/O budgets, backpressure, parsing, and command execution use
the same code in both runs. The startup message records the selected buffer.

## Where it is used

- [server/buffers/input_buffer.h](../server/buffers/input_buffer.h) defines both buffer strategies.
- [server/runners/event_loop.cpp](../server/runners/event_loop.cpp) stores one buffer in each
  connection's `Conn::input`.
- `handleRead()` appends bytes received from the socket.
- `prepareResponse()` passes `input.view()` to `resp::parseCommand()` and consumes
  `result.consumed` bytes after executing a complete command.
- `handleWrite()` calls `prepareResponse()` after a reply finishes, allowing the
  next buffered command to proceed without another read event.

The parser still parses one command per call. On incomplete input, all unread
bytes stay available for the next attempt. On a protocol error, `input.clear()`
discards the bytes before the error reply is sent and the connection closes.

The selection applies to event-loop input. Threaded mode and the client retain
their existing buffers. Event-loop output already uses `conn.sent` to track
partial writes without erasing a prefix after each send.

## How the new buffer works

`InputBuffer` owns a `std::string storage_` and an index `begin_`:

```text
storage_ allocation
+-------------------+---------------------+--------------------+
| consumed prefix   | unread bytes        | unused tail space  |
+-------------------+---------------------+--------------------+
0                   begin_                storage_.size()      capacity()
```

The string manages allocation and lifetime. The offset provides the book's
advancing read position without storing raw pointers that could become invalid
when the allocation grows.

### Reading and consuming

`view()` returns a `std::string_view` covering only the unread bytes. It creates
no copy. Once parsing completes, `consume(n)` advances `begin_` by `n`.
If every byte has been consumed, it clears the string and resets the offset to
zero; the allocated capacity is retained for reuse.

For three buffered commands:

```text
Before parsing:      [ command A | command B | command C ]
Unread view:           ^
After consuming A:   [ consumed  | command B | command C ]
Unread view:                        ^
```

Commands B and C remain at the same addresses during consumption. Their bytes
are moved only if a later append compacts or reallocates storage.

### Appending, compaction, and growth

`append(data, length)` follows these rules:

1. If the string has enough unused capacity at its end, append there.
2. Otherwise, if the consumed prefix is nonempty and at least as large as the
   unread portion, erase that prefix once and reset `begin_` to zero. This is
   **compaction**: it moves the unread bytes to the front to reclaim space.
3. Append the new bytes. If capacity is still insufficient, `std::string`
   allocates more storage through its normal growth behavior.

The half-consumed threshold matters: compacting after every small consumption
could repeatedly copy a large unfinished request. Here, the number of bytes
moved by a compaction is no greater than the prefix consumed since the previous
reset. Compaction work can therefore be spread over those consumed bytes.
Growth can still copy bytes, and compaction may still be followed by growth if
the incoming chunk is large. The optimization removes per-command prefix shifts;
it does not eliminate all buffer copies.

A view is borrowed and must not be retained across buffer mutations. The event
loop parses immediately, and `parseCommand()` copies arguments into its result
before the buffer is consumed or appended again.

## Previous buffer versus new buffer

| Operation | `regular`: previous string behavior | `offset`: improved `InputBuffer` |
| --- | --- | --- |
| Bytes given to parser | Entire input string | View of the unread suffix |
| Remove a completed command | `erase(0, consumed)` shifts every remaining byte | `consume(consumed)` advances an offset |
| Receive more bytes | Append to the string | Append; occasionally compact or grow |
| Retain a partial request | Leave its bytes in the string | Leave its bytes in the unread suffix |
| Empty buffer | String has no bytes | String cleared and offset reset; capacity reused |
| Memory tradeoff | Reclaims the prefix position immediately | Keeps consumed prefix space until compaction or reset |

Previously, a batch of N similar-sized pipelined commands could shift the
remaining commands after each parse: roughly N-1, N-2, and so on. That makes
prefix-removal work quadratic in the number of commands in that batch.

Now each consumption is constant time, and draining an already buffered batch
does not move the remaining commands. Occasional compaction adds copying only
when appending needs space and enough prefix bytes have been consumed. The
request order, replies, and one-reply-at-a-time connection behavior stay the same.

This is a buffer-consumption improvement, not a claim that the whole server is
linear or that throughput has been measured. Parsing still copies arguments,
and an incomplete command is parsed again when more bytes arrive. The buffer
also retains its peak allocated capacity until the connection is destroyed.

For a later benchmark, use identical workloads and compiler flags, fresh server
processes, repeated trials, and several pipeline depths. Measure throughput,
latency, CPU, and baseline/peak resident memory. Offset buffering adds one index
per connection and can retain consumed prefix space or grow to a larger capacity,
so lower memory usage is not guaranteed. With one pending reply per connection,
I/O and scheduling can also hide the copying savings. See
[PERFORMANCE.md](PERFORMANCE.md) for the comparison plan.

## Verification

Run `make test`. `tests/input_buffer_test.cpp` exercises both strategies and
checks that offset consumption preserves the address of remaining bytes, binary data survives repeated
consumption/appends, empty buffers can be reused, and a partial command survives
consumption of a preceding command. Existing integration checks cover pipelines,
fragmented requests, large frames, protocol errors, half-closes, and slow readers
in threaded mode and in event-loop mode with default, regular, and offset buffers.
They also check CLI validation, option order, and the startup buffer label.
