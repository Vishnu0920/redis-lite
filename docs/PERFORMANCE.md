# Comparing threaded and event-loop servers later

Both implementations coexist in the same executable and use the same RESP2 parser,
command processor, storage backend, address, port, and protocol limits. Select them with:

```bash
./build/server --mode threaded
./build/server --mode event-loop
```

Run them separately, stopping the first before starting the second. This document
is a plan for a later benchmark; no load generator, measurement code, or performance
claims are included in the current change. `make test` checks correctness only.

## Compare the two event-loop input buffers

The original and improved input buffers are also selectable in the same binary:

```bash
./build/server --mode event-loop --buffer regular
./build/server --mode event-loop --buffer offset
```

`regular` erases the consumed prefix of a string, shifting remaining bytes after
each command. `offset` advances a read index and occasionally compacts on append.
Event-loop mode defaults to `offset`. Selection occurs once at startup through
two instantiations of the same event-loop template, avoiding per-request mode
checks. The startup banner identifies the buffer used.

Run the same workload against each, keeping the event-loop mode fixed. Sweep
pipeline depth and payload size, include sequential requests as a baseline, and
validate every response. Output buffering, I/O budgets, optimistic writes,
backpressure, and parser behavior are shared. This isolates buffer strategy from
thread scheduling differences. The loop still queues one reply at a time, so
network and poll overhead may limit the observed benefit.

Record throughput, latency, CPU, baseline RSS, loaded RSS, and peak RSS. The offset
buffer adds a read index and retains consumed prefixes until reuse or compaction;
it may allocate more capacity. Lower copying cost does not guarantee lower memory
usage. Both strings can retain allocated capacity after becoming empty. Use fresh
processes, identical optimization flags, repeated trials, and alternating run order
as described below. Record buffer choice in every result row. No speed or memory
improvement has been measured yet. See [BUFFER_HANDLING.md](BUFFER_HANDLING.md).

## Build and control the comparison

Use the same compiler and optimization settings for both modes. The default build
has debug symbols and no optimization flag, which is useful for learning. For a
later optimized comparison, rebuild explicitly because make does not notice changed
compiler flags:

```bash
make clean
make CXXFLAGS='-std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -pthread -Icommon'
```

Use that same binary for both runs. Keep payloads, client count, request count or
duration, pipeline depth, socket settings, and machine conditions identical. Record
the commit, compiler version, flags, Linux/WSL version, CPU count, and file-descriptor
limit (`ulimit -n`). Both servers need one fd per client; threaded mode also needs
one worker thread per connection. Keep below resource limits initially, and record
failures separately when deliberately testing capacity. The listen backlog is 16
in both modes, so connection bursts can measure backlog pressure as well as handlers.

Warm up connections and commands before the measured interval. Start a fresh server
for each trial so previous buffer allocations do not bias memory comparisons.
Repeat each workload several times and alternate mode order. Report the median
and spread across runs. Keep CPU limits identical: the event loop uses one thread,
while the threaded implementation can use several CPU cores. A second experiment
with both servers restricted to one CPU can isolate scheduling overhead.

## Simulate increasing loads

A future standalone Python harness can open many persistent TCP connections,
encode RESP2 arrays of bulk strings, and parse every reply. Use asynchronous I/O
or selectors so the generator can drive many clients without requiring one Python
thread per connection. Keep its measurements outside the server.

| Workload | Suggested sweep | What it reveals |
| --- | --- | --- |
| Sequential PING | 1, 10, 100, 500 active clients; one outstanding request each | Round-trip latency and scheduling under concurrency. |
| ECHO | Small payloads, 1 KiB, and 64 KiB; same concurrency sweep | Parsing, copying, and network costs. |
| Pipelining | 1, 8, 32 requests outstanding per connection | Throughput when clients avoid waiting after every command. |
| Idle connections | Increasing idle clients plus a fixed small active set | Thread cost versus poll scanning cost. |
| Connection churn | Repeated connect, PING, reply, close | Accept and thread-creation cost, kept separate from command throughput. |
| Slow/fragmented senders | Send a request in chunks while other clients run | Whether incomplete requests interfere with other clients. |
| Slow readers | Delay reading ECHO replies while other clients run | Backpressure and responsiveness when writes cannot finish. |

Start small and increase concurrency until throughput stops improving, latency
grows sharply, or errors appear. The sweep values are examples, not promised
capacity. Honor the 64 KiB argument and 1 MiB encoded-command limits. PING/ECHO
remain useful I/O baselines; GET/SET and sorted-set commands now exercise storage.
Keep datasets and operation sequences identical for those workloads. A generic Redis
benchmark may send unsupported handshake or setup commands; any later tool must
be configured for this subset or replaced by the small RESP2 harness.

Persistent clients isolate command processing from connection establishment. The
existing `build/client` is convenient for manual checks, but launching a new client
process per request would mix process startup into the result.

For steady traffic, first use clients that send the next request after receiving
the previous reply. This measures throughput and observed latency under a fixed
concurrency. Later add a controlled arrival-rate test, scheduling sends independently
of completions: record both intended and actual send times so an overloaded generator
does not silently hide queuing delay. Bound outstanding requests and record dropped
or timed-out work.

## Measure time, memory, and CPU

The harness should use a monotonic clock (for example `time.monotonic_ns()`) and
count completed, validated replies rather than requests merely sent.

- **Throughput:** successful replies divided by measured elapsed seconds. Also
  record payload bytes per second for ECHO.
- **Latency:** time from sending a request to fully receiving its reply. Report
  p50, p95, p99, and maximum, plus connection establishment time separately. For
  pipelines, pair replies with send timestamps in order; batch elapsed time divided
  by batch size is not an individual request's latency.
- **Fixed-work completion:** elapsed time to complete the same total request
  count, at the same concurrency and pipeline depth.
- **Memory:** sample the server's `/proc/<pid>/status`. VmRSS is resident process
  memory, VmHWM its peak resident memory, and VmSize virtual address space. Record
  idle baseline, loaded values, and values after clients disconnect. Thread stack
  reservations increase virtual memory without necessarily being resident. String
  capacity and allocator caches can retain memory after data is consumed.
- **CPU and scheduling:** sample user/system CPU time from `/proc/<pid>/stat` and
  thread count from `/proc/<pid>/task`. Sum context-switch counters across live
  threads using `/proc/<pid>/task/<tid>/status`; the process status alone is not a
  sum across all worker threads. Sampling live threads can miss counters from
  workers that already exited, especially in churn tests, so label that limitation.
- **Correctness and overload:** count unexpected replies, disconnects, connection
  failures, and timeouts. Report attempted and completed work separately.

Take before/after CPU deltas around the measured interval and state whether CPU
percentage is relative to one core or the entire machine. Sample memory at a fixed
interval using a separate observer; report its interval. VmRSS excludes kernel
socket buffers, and VmHWM covers the server's whole lifetime, including warmup.
If kernel networking memory matters later, measure it separately and document the
method rather than treating RSS as total system memory.

At first, both generator and server run on loopback. They compete for the same CPU
and memory bandwidth, so monitor the generator too: a saturated generator can make
both servers appear to have the same limit. Use separate CPU allocations if useful.
A separate load machine would require an explicit future change to the server's
loopback bind address; it is not supported by the current launch options.

## Interpret and record results

Use one row per workload configuration and mode, recording client count, active/idle
mix, payload size, pipeline depth, warmup, duration/request count, completed requests,
errors, throughput, latency percentiles, CPU deltas, baseline/peak RSS, VmSize, and
thread count. Keep raw samples so summaries can be recomputed.

Expect different tradeoffs rather than a guaranteed winner. Threads can execute
on multiple cores but add stacks and scheduling work. The single-thread event loop
avoids per-client threads, but rebuilds/scans the poll list and fd-indexed vector,
and handles command execution synchronously. It queues only one reply per client
and pauses reads while that reply is pending. Both modes still reparse incomplete
RESP input. Threaded mode and the regular event-loop buffer erase consumed string
prefixes; the offset event-loop buffer advances a cursor. Record the event-loop
buffer choice when comparing connection modes so this variable is explicit.

Storage through Chapter 11 is now present. Storage commands share a mutex in both
server modes; threaded-mode measurements include contention on that mutex.
PING/ECHO bypass it. The custom hash table migrates progressively, and sorted sets
use an augmented AVL tree. STL backends remain future comparison work, with
replacement boundaries described in [STORAGE.md](STORAGE.md). No benchmark results
are claimed; timers, TTL, and background deletion are not yet implemented.
