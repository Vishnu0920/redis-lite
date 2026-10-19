# Available commands through Chapter 13

The server implements a focused Redis-style command set over RESP2. Command names
are case-insensitive. Keys, values, and sorted-set member names are binary-safe.
All connections in one process share one in-memory database, in either server mode.
Restarting the server starts with an empty database: no files, snapshots, or append
log are written. Optional key TTLs work for both value types in both server modes.

Start a server and use the supplied client:

```bash
make
./build/server --mode event-loop
```

In another terminal:

```bash
./build/client
```

## Strings and keys

| Command | Action and reply |
| --- | --- |
| `PING [message]` | Returns `PONG`, or echoes the optional message. |
| `ECHO message` | Returns the message as a bulk string. |
| `SET key value` | Creates or replaces a string value and clears its TTL. Returns `OK`. Replaces a sorted-set value too. |
| `GET key` | Returns a string value, or null when absent. A sorted-set key produces `WRONGTYPE`. |
| `DEL key [key ...]` | Deletes whole keys of either type. Returns the number actually removed; duplicates count once. |
| `KEYS` or `KEYS *` | Returns all keys in bytewise lexical order. Only the `*` pattern is supported. |

`SET` supports only these three arguments; options such as `NX`, `XX`, and `EX`
are not implemented. `KEYS` scans and sorts the whole keyspace, so it can take
substantial time on a large database.

## Key expiration

| Command | Action and reply |
| --- | --- |
| `EXPIRE key seconds` | Sets or updates the timer. Returns integer `1` for an existing key, `0` for a missing key. Nonpositive seconds delete an existing key immediately. |
| `TTL key` | Returns seconds remaining, rounded to the nearest second; `-1` means the key has no timer, `-2` means it is missing or expired. A live key with less than half a second left returns `0`. |
| `PERSIST key` | Removes the timer without deleting the value. Returns `1` if a timer was removed, otherwise `0`. |

New keys have no timer. `EXPIRE` accepts a signed 64-bit decimal integer;
malformed values and positive durations that overflow the internal millisecond
deadline are rejected without changing the existing timer. Conditional expiration
options, `PEXPIRE`, and `PTTL` are not implemented.

`SET` clears a TTL. Updating an existing sorted set with `ZADD` or `ZREM` preserves
its TTL; deleting its last member deletes the key and timer together. `DEL`
also removes the timer. Expired keys behave as missing for every command,
including `KEYS`, even before the next scheduled expiration pass.

The table describes the server's RESP integer replies. The supplied client uses
the original command to display readable timer messages, in both interactive and
one-command mode. `TTL -2` displays "Timer expired or key does not exist" because
the client cannot distinguish those cases from the reply alone. `TTL 0` displays
"Less than 1 second remaining". Errors keep their usual `(error)` display.

```text
SET session value     -> OK
EXPIRE session 60     -> Timer set to 60 seconds.
TTL session           -> Time remaining: 60 seconds. (if queried immediately)
PERSIST session       -> Timer removed; this key has no expiration.
TTL session           -> No timer for this key.
DEL session           -> (integer) 1
TTL session           -> Timer expired or key does not exist.
```

`EXPIRE` returning zero displays "Key does not exist or has expired; no timer was
set". A successful nonpositive `EXPIRE` reports immediate key deletion. `PERSIST`
returning zero displays "No timer to remove (key has no timer or does not exist)".

Key TTLs are independent of connection activity. The event-loop runner also closes
connections after 60 idle seconds by default. See [TIMERS.md](TIMERS.md) for the
two timer structures, scheduling, and ownership.

## Sorted sets

A sorted set stores unique member names with numeric scores. Members are ordered
by ascending score, then by bytewise member name when scores tie. Updating a
member's score changes its position without creating a second member.

| Command | Action and reply |
| --- | --- |
| `ZADD key score member [score member ...]` | Creates a sorted set or adds/updates members. Returns the number of new members. All scores are validated before modifying storage. |
| `ZREM key member [member ...]` | Removes members and returns the number removed. Removes the key when its last member is deleted. |
| `ZSCORE key member` | Returns the score as a bulk string, or null if the key/member is absent. |
| `ZRANK key member` | Returns the zero-based position in ascending order, or null if absent. |
| `ZCARD key` | Returns the number of members; an absent key returns zero. |
| `ZQUERY key score member offset limit` | Seeks to the first pair greater than or equal to `(score, member)`, applies a signed offset, then returns up to `limit` pairs in ascending order. |

An existing string key produces `WRONGTYPE` for all sorted-set commands. Missing
keys behave as empty sets for removals, counts, and queries. Missing scores/ranks
return null. A score may be negative or infinite (`-inf` or `inf`); NaN, malformed
numbers, and numeric overflow are rejected. Scores are stored as doubles and
returned with enough precision to round-trip.

`ZQUERY` is the book's simplified range command, not a standard Redis command.
Its reply is a flat RESP2 array: `member, score, member, score, ...`. Here `limit`
counts **pairs**; the book's sample loop counts output array elements instead.
Offsets may move backward from the sought pair; output then proceeds forward.
If the initial seek finds no pair, or the offset leaves the set, the reply is
empty. A nonpositive limit also returns an empty array. Offset and limit must be
signed 64-bit decimal integers.

The basic `ZADD` form supports multiple pairs but no options such as `NX`, `XX`,
or `CH`. `ZRANGE` and the broader Redis command family are not implemented.
`ZRANK` implements the book's rank-query exercise; `ZCARD` exposes the set count.

## Example session

```text
SET greeting "hello world"        -> OK
GET greeting                      -> hello world
GET missing                       -> (nil)
ZADD leaderboard 20 bob 10 alice   -> (integer) 2
ZADD leaderboard 15 bob            -> (integer) 0
ZSCORE leaderboard bob            -> 15
ZRANK leaderboard bob             -> (integer) 1
ZCARD leaderboard                 -> (integer) 2
ZQUERY leaderboard -inf "" 0 2
                                  -> 1) alice
                                     2) 10
                                     3) bob
                                     4) 15
ZQUERY leaderboard 15 bob -1 1     -> alice, 10
ZREM leaderboard alice            -> (integer) 1
DEL greeting leaderboard          -> (integer) 2
KEYS                              -> (empty array)
```

The arrows describe output; enter only the command on their left. The client
also accepts one command per invocation, for example `./build/client GET greeting`.
`exit` closes the interactive client; it is not a server command.

## Protocol and limits

Requests remain nonempty RESP2 arrays of non-null bulk strings. Replies use simple
strings, errors, integers, bulk strings, null bulk strings, and arrays. The client
prints nulls as `(nil)`, integers as `(integer) N`, and arrays as numbered entries.
Timer commands use the messages above instead of the generic integer display;
their wire replies remain RESP integers.
An empty string is different from null: `SET key ""` stores a real empty value.

Request limits remain 128 elements including the command name, 64 KiB per bulk
string, and 1 MiB per encoded frame. Encoded array replies larger than 1 MiB are
replaced by an `ERR response exceeds 1 MiB` reply. That is a command error and the
connection remains usable. Large listing/query replies are currently assembled
in memory before this size check; the cap does not limit transient allocations.
The client limits parsed reply frames to 1 MiB and nesting depth to 32.

See [STORAGE.md](STORAGE.md) for the storage interfaces and each command's data path.
The implementation follows the concepts in [Chapter 8](https://build-your-own.org/redis/08b_hashtables_impl)
and [Chapter 11](https://build-your-own.org/redis/11_sortedset), using C++ ownership
and RESP2 instead of the book's custom serialization. `SET` replacing either type
and deleting an empty sorted-set key are explicit choices closer to Redis behavior;
the book's sample rejects `SET` on a sorted set and retains an empty set.
