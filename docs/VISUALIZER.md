# Interactive request explorer

Open [visualizer/dist/index.html](../visualizer/dist/index.html) in a browser. No build, dependencies,
server, or internet connection are needed. The files are plain HTML/CSS/JS and
also work through a static HTTP server:

```bash
python3 -m http.server 8080 --directory visualizer/dist
```

Then open `http://localhost:8080`. Run the command from the project root.

Choose a scenario and press **Play trace**. Pause, step with the arrow buttons,
drag the timeline, or click a stage to jump to its next call. The keyboard
shortcuts are **Space** and **Left/Right**. **Edit & explore** accepts commands,
one per line, with the CLI's double-quote convention. Every rebuilt trace starts
with `greeting = hello` and a leaderboard containing `alice: 10`, `bob: 20`.

The page shows event-loop mode with `InputBuffer` only. It follows:

- Client arguments → RESP2 byte encoding → simulated ordered TCP delivery.
- `poll()`, `fd2Conn[fd]`, `Conn` states, and nonblocking read/write calls.
- `InputBuffer::append()`, `view()`, `consume()`, and conditional compaction.
- One-command RESP parsing, decoded arguments, and exact consumed byte counts.
- Command execution, storage-interface effects, reply encoding, and client decoding.
- Fragmentation, pipelining, partial sends, EAGAIN, and protocol-error cleanup.

This is a **source-guided simulation**, not live server instrumentation. Socket
outcomes, timing, and buffer capacities are illustrative. The compaction condition
and offset updates follow `server/buffers/input_buffer.h`; the real C++ allocation capacity
and growth policy depend on the standard library. Storage results are modeled at
the interface boundary; the page does not implement the custom hash/AVL internals.
The diagram maps interacting components; the timeline gives their actual modeled
call order, including repeated parsing and returns to poll.

The pipeline demo uses a protocol-capable client. The repository's normal CLI
calls `query()` sequentially and waits for each reply. The visualization focuses
on one connection and the selected scenarios; it is not a benchmark or an exhaustive
simulation of all possible socket errors, signals, or concurrent connections.
It illustrates the earlier request/buffer milestone: TTL commands and connection
idle timers are described in [TIMERS.md](TIMERS.md) and are not simulated here.

Commands use UTF-8 byte lengths. The editor has a 16 KiB total request limit and
a 1,000-step trace limit for manageable playback. Larger deliveries or send chunks
can reduce the number of steps; the C++ server's frame limit remains 1 MiB and it
has no teaching step limit. The scenario's
custom hash operations and sorted-set ordering are explained at the storage boundary.

## Checks

The browser files require no Node installation to run. Developers can check the
simulation using Node 18+:

```bash
node visualizer/tests/model.test.cjs
```

To compare every preset and additional edge cases against **the actual C++ parser,
command processor, and custom storage**, compile the standalone test oracle:

```bash
g++ -std=c++17 -O2 -pthread -Icommon -Iserver \
    visualizer/tests/oracle.cpp common/resp.cpp \
    server/commands/commands.cpp server/storage/storage.cpp \
    -o /tmp/redis-lite-flow-oracle
node visualizer/tests/model.test.cjs /tmp/redis-lite-flow-oracle
```

The oracle never opens a socket, modifies the server executable, or writes data to
a running server.

## GitHub Pages

[The deployment workflow](../.github/workflows/deploy-pages.yml) publishes the
contents of `visualizer/dist/`. This folder contains the actual HTML/CSS/JS source,
so it stays tracked even though the C++ `build/` directory is ignored. Asset links
are relative and work under the repository's `/redis-lite/` Pages path.

One-time setup:

1. On GitHub, open the repository's **Settings → Pages**.
2. Under **Build and deployment → Source**, select **GitHub Actions**.
3. Commit the staged project changes and push to `main` as usual:

   ```bash
   git commit -m "Organize project and add request visualization"
   git push origin main
   ```

4. Open the **Actions** tab and wait for **Publish request visualization** to
   succeed. The deployment publishes the page at
   [https://vishnu0920.github.io/redis-lite/](https://vishnu0920.github.io/redis-lite/).

Later pushes to `main` automatically publish the latest webpage files. Local
commits alone do not publish anything. You can also redeploy from **Actions →
Publish request visualization → Run workflow**, choosing `main`; this is useful
if Pages was enabled after the first push.

GitHub Pages serves the browser simulation; the C++ server runs separately.
For public repositories, Pages is available on GitHub Free. See GitHub's
[publishing configuration](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site)
and [custom workflow documentation](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).
