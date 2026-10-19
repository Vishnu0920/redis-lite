# Project details — Chapters 4–13: RESP2, storage, sorted sets, and timers

Linux / WSL, C++17. This is a learning project with a focused RESP2 subset.
It implements PING/ECHO, GET/SET/DEL/KEYS, and sorted-set commands using a custom
hash table and augmented AVL tree. Optional key TTLs use an indexed min-heap;
event-loop idle timeouts use embedded doubly linked nodes.
Data lives only in memory and is lost on restart.
See [COMMANDS.md](COMMANDS.md) for syntax and [STORAGE.md](STORAGE.md) for the simple
storage interfaces and command flow.

## Build and run

From this directory:

```bash
make
./build/server
```

The default is the original thread-per-connection server. Select either mode:

```bash
./build/server --mode threaded
./build/server --mode event-loop
```

Event-loop input buffering can also be selected for later performance comparisons:

```bash
./build/server --mode event-loop --buffer regular  # Original string prefix erasure
./build/server --mode event-loop --buffer offset   # Improved read offset (default)
```

`--buffer` requires event-loop mode. Both strategies use the same event loop and
output buffering. See [BUFFER_HANDLING.md](BUFFER_HANDLING.md) for implementation
details and tradeoffs.

Run one server at a time; both use 127.0.0.1:6379. Stop it with Ctrl+C before
switching modes. The same client works with both, and no rebuild is needed to switch.
`./build/server --help` prints the options.

In another WSL terminal, in the same directory:

```bash
./build/client
```

Try:

```text
PING
ECHO "hello world"
ECHO ""
ping hello
SET name Vishnu
GET name
ZADD leaderboard 20 bob 10 alice
ZRANK leaderboard bob
ZQUERY leaderboard -inf "" 0 2
DEL name leaderboard
exit
```

The interactive client keeps one connection open. `exit` is a local client action.
Its input syntax supports double-quoted arguments, not full shell/redis-cli syntax.
Inside double quotes, backslash escapes the following character; it does not decode
textual sequences such as `\n` into control bytes. The wire protocol itself is binary-safe.

One command per invocation also works (your shell processes the quotes):

```bash
./build/client PING
./build/client ECHO "hello world"
```

Stop the server using Ctrl+C. Run `make test` with no server already occupying 6379.
Tests require Python 3 and exercise both modes, including fragmented requests,
pipelining, concurrent clients, slow readers, and disconnects. They check correctness,
not performance, and exercise both event-loop buffer strategies. If port 6379 is occupied, change `net::Port` in common/net.h
and rebuild both programs; integration.py uses 6379 and must be updated too.

## File responsibilities and reading order

1. common/resp.h: parser contracts and limits.
2. common/resp.cpp: encoding and parsing, independent of sockets.
3. common/net.h: socket ownership, blocking receive and send-all helpers.
4. server/commands/commands.cpp: shared command validation, storage calls, and reply encoding.
5. server/main.cpp: select mode, create the listener and one shared command processor.
6. server/runners/threaded.cpp: original accept/thread-per-connection and blocking reply loop.
7. server/runners/event_loop.cpp: Conn, nonblocking accept/read/write handlers, poll loop.
8. client/main.cpp: connect, turn user arguments into RESP, decode replies.
9. tests/: parser boundary checks and actual TCP round trips for both modes.
10. server/storage/storage.h and server/storage/storage.cpp: replaceable storage/set contracts and custom adapters.
11. server/storage/data_structures/: hash migration and AVL internals, optional for tracing commands.

The client and server are separate executables. Each compiles its own copy of
common/resp.cpp; sharing source does not mean sharing runtime memory.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the startup decision, shared components,
connection lifetimes, diagrams, and how to add another connection model.

## How the server works

In threaded mode, each client thread owns its socket and input buffer. Parse the buffer first:

- Complete: execute exactly one command, send the entire reply, remove the consumed
  bytes, then parse again. Extra commands remain available in the buffer.
- Incomplete: receive more bytes and append them; keep the old bytes.
- Error: send a protocol error if possible and close the connection.

The parser accepts nonempty arrays of non-null bulk strings. It checks declared
lengths before copying data. Limits: 128 arguments, 64 KiB per argument, 1 MiB per
encoded command, 20 decimal digits per length header. It reparses incomplete input
from the start for clarity; a resumable parser is a later optimization. Event-loop
mode now supports a buffer cursor through `--buffer offset`.
Each receive appends at most 4096 bytes, so buffered data may briefly include bytes
from a following frame. Per-frame limits are independent of that extra data.

Replies support simple/error strings, bulk strings, nulls, signed integers, and
arrays, including nested arrays in the parser. RESP3, inline commands,
authentication, persistence, and Redis client handshake commands are outside this milestone.

Command names are ASCII case-insensitive. Values retain their original bytes.
The response line encoders are for single-line text only; arbitrary values use bulk().
sendAll retries partial writes and EINTR; MSG_NOSIGNAL is Linux-specific.
Socket uses RAII to close descriptors on handler return. EOF during a request is
treated as truncation. A command error leaves the connection open.

Both modes share the parser, command executor, listener setup, and wire behavior.
All clients share one database through `CommandProcessor`. Storage commands hold
a mutex for their operation and reply snapshot, releasing it before socket I/O;
PING/ECHO bypass storage. Both server modes use this same command layer.
There is no connection cap or graceful shutdown/drain mechanism. Event-loop
connections have a 60-second idle timeout; threaded connections retain their
previous lifetime. The listener is bound to loopback for local learning.

## Event-loop mode (Chapters 5–6)

This follows the book-inspired event-loop reference. One thread owns all connections. `fd2Conn` is a
vector indexed by socket fd; each nonempty slot owns a Conn, and each Conn owns its
socket with RAII. Unused slots are empty. Instead of independent `want_read` and
`want_write` booleans, one state determines the requested event:

| State | poll interest | Work |
| --- | --- | --- |
| Request | POLLIN | Parse retained input; receive more if incomplete. |
| Response | POLLOUT | Send the pending reply, advancing the `sent` offset. |
| End | Removed before the next poll | Destroy Conn and close its socket. |

The loop rebuilds `pollfds` with the listener first and live connections afterward,
then calls `poll(..., nextTimerMS())` to sleep until a socket is ready or the
nearest idle/TTL timer is due. Finding that deadline reads only the idle-list front
and TTL heap root. The listening socket
and every accepted client socket have O_NONBLOCK enabled. The accept handler takes
available connections until EAGAIN or a 64-connection turn limit, allowing existing
clients to make progress during a connection burst.
Fatal listener errors end the server, as in threaded mode.

`handleRead` parses before receiving. Complete input produces one reply and changes
the state to Response; incomplete input stays buffered. `handleWrite` retains the
unsent suffix after a partial send. EAGAIN/EWOULDBLOCK means return to poll, EINTR
means retry, and other I/O failures end that connection. Read and write handlers
each have a 64 KiB byte budget per turn. The parser's existing frame limits still
apply; a large incomplete command can span many turns.

After a reply drains, the state returns to Request. Any already buffered next
command is parsed immediately, so pipelines continue without waiting for new
incoming bytes. A protocol error is sent before closing; ordinary command errors
leave the connection usable. A peer that shuts down its write side still receives
replies for complete requests; EOF with an incomplete request closes the connection.

Only one reply is queued per connection. While it is pending, the loop watches
POLLOUT and pauses reads for that client. This backpressure bounds application
buffering even when a client pipelines requests but reads slowly. Idle connections
watch only POLLIN, avoiding continuous wakeups on writable sockets. `sendAll` stays
exclusive to the blocking client and threaded server.

The poll list and fd-indexed vector are scanned each iteration. This is the book's
learning stage; epoll, resumable parsing, and background
workers remain future work. Command execution itself runs on the event-loop thread,
so large listings, queries, and deletions can delay other clients.

See [PERFORMANCE.md](PERFORMANCE.md) for a future comparison plan. No benchmark
harness or performance instrumentation is included in this milestone.

## Incremental progression

This adapts the supplied book's progression:

- Chapter 4 (complete): RESP protocol milestone (instead of the book's custom framing).
- Chapters 5–6 (complete): nonblocking sockets, poll(), connection state, input/output buffers;
  threaded mode remains available for comparison.
- Chapter 7 (complete): SET, GET, DEL and shared in-memory storage.
- Chapter 8 (complete): custom hash table and incremental rehashing.
- Chapter 9 (complete): RESP2 null, integer, and array replies instead of custom serialization.
- Chapters 10–11 (complete): augmented AVL tree and sorted sets, including rank queries.
- Chapters 12–13 (complete): embedded idle timers, an indexed min-heap, key expiration,
  and EXPIRE/TTL/PERSIST commands in seconds. See [TIMERS.md](TIMERS.md).
- Chapter 14: selected background work using a thread pool.

Start by tracing encodeCommand({"ECHO", "hello"}), then Reader::readBulk(),
parseCommand(), handleClient(), and query(). For the new stage, follow Conn,
prepareResponse(), handleRead(), handleWrite(), and runEventLoopServer().
For storage, trace `CommandProcessor::execute()` into `storage::Storage` and then
the adapter in `storage.cpp`. See [STORAGE.md](STORAGE.md) before reading low-level
nodes and rotations. For timers, trace `nextTimerMS()`, `refreshTimer()`, and
`processTimers()`, then `Storage::expire()` and the indexed heap.
