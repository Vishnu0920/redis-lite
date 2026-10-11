# Redis Lite

A Redis-style server and command-line client built in C++17, following
*Build Your Own Redis* through Chapter 11. It uses RESP2 for communication and
custom hash tables and an augmented AVL tree for strings and sorted sets.

The project offers two ways to handle client connections and two event-loop
input buffers, making it useful for learning and later performance comparisons.
All clients share one database. **Data is stored only in memory and is lost when
the server stops.** Persistence and TTLs are not implemented.

## Build

Use Linux or WSL with `g++` supporting C++17 and GNU Make. Run these commands
from the project directory:

```bash
make
```

This creates `build/server` and `build/client`.

## Project layout

```text
server/
  main.cpp                 Startup and mode selection
  commands/                Command validation and execution
  buffers/                 Regular and offset input buffers
  storage/                 Storage interfaces and custom backend
    data_structures/       Hash table and AVL tree
  runners/                 Threaded and event-loop connection handling
common/                    Shared RESP2 protocol and socket helpers
client/                    Command-line client
tests/                     C++ and TCP correctness checks
docs/                      Architecture and implementation documentation
visualizer/                Interactive webpage and its checks
.github/workflows/         GitHub Pages deployment
```

`build/` is generated locally and ignored by Git. The Makefile stays in the
repository so anyone can rebuild the executables.

## Choose a server mode

Start **one** of the following in your first terminal:

| Configuration | Command | How it works |
| --- | --- | --- |
| Threaded | `./build/server --mode threaded` | Creates one worker thread per client. Each worker uses blocking socket reads/writes and a regular string input buffer. |
| Event loop with regular buffer | `./build/server --mode event-loop --buffer regular` | One thread serves all clients using `poll()` and nonblocking sockets. Consuming a request erases its string prefix and shifts the remaining bytes. |
| Event loop with offset buffer | `./build/server --mode event-loop --buffer offset` | Uses the same event loop, but consumes requests by advancing a read offset. It occasionally compacts storage when appending needs space, reducing repeated copying. |

Defaults:

```bash
./build/server                     # Threaded mode
./build/server --mode event-loop   # Event loop with offset buffer
```

`--buffer` applies only to event-loop mode. The regular buffer is a wrapper around
the original `std::string` behavior; the offset buffer is the alternative FIFO
implementation. Both modes provide the same commands and storage backend.

The server listens on **127.0.0.1:6379**. Stop it with **Ctrl+C** before switching
configurations; no rebuild is needed. Restarting also clears the database.
Use `./build/server --help` to see the options.

## Connect and run commands

In a second terminal, from the same project directory:

```bash
./build/client
```

Enter commands at the `redis>` prompt:

```text
PING
SET greeting "hello world"
GET greeting
ZADD leaderboard 20 bob 10 alice
ZRANK leaderboard bob
ZQUERY leaderboard -inf "" 0 2
DEL greeting leaderboard
exit
```

Double quotes group arguments containing spaces or represent an empty string.
`exit` closes the interactive client. You can also send one command per invocation:

```bash
./build/client SET greeting "hello world"
./build/client GET greeting
```

Available commands: `PING`, `ECHO`, `SET`, `GET`, `DEL`, `KEYS`, `ZADD`, `ZREM`,
`ZSCORE`, `ZRANK`, `ZCARD`, and `ZQUERY`. This is a focused subset; `ZQUERY` is the
book's simplified range command. See [COMMANDS.md](docs/COMMANDS.md) for syntax,
replies, examples, and limits.

## Run the checks

With Python 3 installed and the server stopped:

```bash
make test
```

The tests cover parsing, custom structures, storage commands, concurrent clients,
pipelining, and both server modes and event-loop buffer choices. These are
correctness tests, not performance benchmarks.

## Publish the visualization

The repository includes a Pages workflow that publishes `visualizer/dist/` on
pushes to `main`. No webpage build or running C++ server is needed.

Enable **Settings → Pages → Build and deployment → Source → GitHub Actions**
in the GitHub repository, then commit and push normally. After the first
successful deployment, the interactive page will be available at
[vishnu0920.github.io/redis-lite](https://vishnu0920.github.io/redis-lite/).
See [visualization and publishing instructions](docs/VISUALIZER.md).

## Further reading

- [Interactive request explorer](visualizer/dist/index.html): open this file locally to animate the event-loop and offset-buffer request path; see [how to use it](docs/VISUALIZER.md).
- [Project details](docs/PROJECT_DETAILS.md): implementation walkthrough and chapter progression.
- [Architecture](docs/ARCHITECTURE.md): connection handling and ownership.
- [Storage](docs/STORAGE.md): simple interfaces, command flow, and future STL replacement.
- [Buffer handling](docs/BUFFER_HANDLING.md): regular versus offset buffering.
- [Performance comparison plan](docs/PERFORMANCE.md): workloads and measurement criteria.
