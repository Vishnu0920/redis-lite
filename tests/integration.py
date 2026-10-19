"""Correctness checks for both connection models; this is not a benchmark."""

import concurrent.futures
import contextlib
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "build/server"
CLIENT = ROOT / "build/client"
PORT = 6379  # Keep aligned with net::Port in common/net.h.


def command(*args):
    return b"*%d\r\n" % len(args) + b"".join(bulk(arg) for arg in args)


def bulk(value):
    return b"$%d\r\n" % len(value) + value + b"\r\n"

def array(*values):
    return b"*%d\r\n" % len(values) + b"".join(bulk(value) for value in values)


def connect():
    return socket.create_connection(("127.0.0.1", PORT), timeout=5)


def receive_exact(sock, expected):
    # Compare the wire bytes too, including framing and binary payloads.
    offset = 0
    while offset < len(expected):
        data = sock.recv(min(65536, len(expected) - offset))
        assert data, "server closed before the complete reply"
        assert data == expected[offset:offset + len(data)], "wrong reply bytes/order"
        offset += len(data)


def read_server_log(log):
    # Read without seeking the file description shared with the child: seeking
    # while it logs could make its next write overwrite an earlier message.
    return os.pread(log.fileno(), os.fstat(log.fileno()).st_size, 0).decode(errors="replace")


@contextlib.contextmanager
def running_server(mode, buffer=None, reverse_options=False, idle_timeout_ms=None, log_capture=None):
    # Refuse to test someone else's server on this fixed port.
    with socket.socket() as probe:
        try:
            probe.connect(("127.0.0.1", PORT))
        except ConnectionRefusedError:
            pass
        else:
            raise RuntimeError(f"Port {PORT} is occupied; stop that server first")

    with tempfile.TemporaryFile() as log:
        args = [str(SERVER)] if mode is None else [str(SERVER), "--mode", mode]
        if buffer is not None:
            if reverse_options:
                args[1:1] = ["--buffer", buffer]
            else:
                args += ["--buffer", buffer]
        if idle_timeout_ms is not None:
            args += ["--idle-timeout-ms", str(idle_timeout_ms)]
        process = subprocess.Popen(args, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 5
            while True:
                if process.poll() is not None:
                    raise RuntimeError("server exited during startup")
                try:
                    with connect() as probe:
                        probe.sendall(command(b"PING"))
                        receive_exact(probe, b"+PONG\r\n")
                        break
                except ConnectionRefusedError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("server did not start")
                    time.sleep(0.02)
            banner = read_server_log(log).splitlines()[0]
            if mode == "event-loop":
                assert f"buffer={buffer or 'offset'}" in banner, banner
                assert f"idle-timeout-ms={idle_timeout_ms or 60000}" in banner, banner
            yield process
            assert process.poll() is None, "server unexpectedly exited"
        except BaseException:
            print(read_server_log(log))
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if log_capture is not None:
                log_capture.append(read_server_log(log))


def check_connection_logs(log, expected_reasons):
    # A reused fd must finish its old lifecycle before its next acceptance.
    active = set()
    reasons = set()
    accepted_count = 0
    for line in log.splitlines():
        accepted = re.fullmatch(r"Connection accepted \(fd=(\d+)\)", line)
        closed = re.fullmatch(r"Connection closing \(fd=(\d+)\): (.+)", line)
        if accepted:
            fd = int(accepted[1])
            assert fd not in active, f"duplicate acceptance without closure: {line}"
            active.add(fd)
            accepted_count += 1
        elif closed:
            fd = int(closed[1])
            assert fd in active, f"closure without acceptance: {line}"
            active.remove(fd)
            reasons.add(closed[2])
    assert accepted_count > 0, "no connection acceptance logs"
    assert set(expected_reasons) <= reasons, reasons
    # SIGTERM ends the test server without a graceful drain, so clients still
    # alive at termination need not have a closure message.


def check_commands():
    with connect() as sock:
        exchanges = [
            ((b"PING",), b"+PONG\r\n"),
            ((b"ping", b"hello"), bulk(b"hello")),
            ((b"ECHO", b""), bulk(b"")),
            ((b"echo", b"a\x00b\r\n"), bulk(b"a\x00b\r\n")),
            ((b"PING", b"a", b"b"), b"-ERR wrong number of arguments for PING\r\n"),
            ((b"ECHO",), b"-ERR wrong number of arguments for ECHO\r\n"),
            ((b"UNSUPPORTED",), b"-ERR unknown command\r\n"),
            ((b"PING",), b"+PONG\r\n"),
        ]
        for args, reply in exchanges:
            sock.sendall(command(*args))
            receive_exact(sock, reply)

    result = subprocess.run([str(CLIENT), "ECHO", "hello world"], capture_output=True,
                            text=True, timeout=5, check=True)
    assert result.stdout == "hello world\n"
    result = subprocess.run([str(CLIENT)], input='PING\nECHO ""\nexit\n',
                            capture_output=True, text=True, timeout=5, check=True)
    assert "PONG" in result.stdout

def check_storage():
    with connect() as sock, connect() as other:
        key, value = b"binary\x00key", b"a\x00b\r\nvalue"
        sock.sendall(command(b"SET", key, value))
        receive_exact(sock, b"+OK\r\n")
        other.sendall(command(b"GET", key))
        receive_exact(other, bulk(value))  # Shared across connections.
        sock.sendall(command(b"GET", b"missing"))
        receive_exact(sock, b"$-1\r\n")
        sock.sendall(command(b"DEL", key, key, b"missing"))
        receive_exact(sock, b":1\r\n")
        exchanges = [
            ((b"ZADD", b"board", b"10", b"bob", b"10", b"alice", b"20", b"carol"), b":3\r\n"),
            ((b"ZADD", b"board", b"5", b"bob"), b":0\r\n"),
            ((b"ZSCORE", b"board", b"bob"), bulk(b"5")),
            ((b"ZRANK", b"board", b"alice"), b":1\r\n"),
            ((b"ZCARD", b"board"), b":3\r\n"),
            ((b"ZQUERY", b"board", b"-inf", b"", b"0", b"3"), array(b"bob", b"5", b"alice", b"10", b"carol", b"20")),
            ((b"ZQUERY", b"board", b"10", b"alice", b"-1", b"1"), array(b"bob", b"5")),
            ((b"ZQUERY", b"board", b"10", b"alice", b"9223372036854775807", b"1"), b"*0\r\n"),
            ((b"KEYS", b"*"), array(b"board")),
            ((b"ZSCORE", b"board", b"missing"), b"$-1\r\n"),
        ]
        # A mixed pipeline exercises nulls, integers, arrays, and storage changes.
        sock.sendall(b"".join(command(*args) for args, _ in exchanges))
        receive_exact(sock, b"".join(reply for _, reply in exchanges))
        sock.sendall(command(b"GET", b"board"))
        receive_exact(sock, b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
        sock.sendall(command(b"ZREM", b"board", b"alice", b"bob", b"carol"))
        receive_exact(sock, b":3\r\n")
        sock.sendall(command(b"KEYS"))
        receive_exact(sock, b"*0\r\n")
        # Fragmented writes followed by reads and deletions remain ordered.
        frame = command(b"SET", b"fragmented", b"value")
        sock.sendall(frame[:-2])
        sock.sendall(b"\r\n" + command(b"GET", b"fragmented") + command(b"DEL", b"fragmented"))
        receive_exact(sock, b"+OK\r\n" + bulk(b"value") + b":1\r\n")

    # Supplied client decodes all newly supported reply types.
    result = subprocess.run([str(CLIENT)],
                            input='GET missing\nSET cli value\nDEL cli\nKEYS\nZADD cli-board 1 alice\nZQUERY cli-board 0 "" 0 1\nDEL cli-board\nexit\n',
                            capture_output=True, text=True, timeout=5, check=True)
    assert "(nil)" in result.stdout and "(integer) 1" in result.stdout
    assert "(empty array)" in result.stdout and "1) alice" in result.stdout

    # Both connection models must safely share the database under concurrent writes.
    def add_members(client):
        with connect() as sock:
            frames = [command(b"ZADD", b"shared", str(i).encode(), f"{client}:{i}".encode())
                      for i in range(100)]
            sock.sendall(b"".join(frames))
            receive_exact(sock, b":1\r\n" * 100)

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(add_members, range(8)))
    with connect() as sock:
        sock.sendall(command(b"ZCARD", b"shared") + command(b"DEL", b"shared"))
        receive_exact(sock, b":800\r\n:1\r\n")


def check_fragmentation_and_pipeline():
    with connect() as sock:
        frame = command(b"ECHO", b"fragmented\x00\r\n")
        for byte in frame:
            sock.sendall(bytes([byte]))
        receive_exact(sock, bulk(b"fragmented\x00\r\n"))
        # More than one recv buffer; replies must not depend on further writes.
        sock.sendall(command(b"PING") * 1000)
        receive_exact(sock, b"+PONG\r\n" * 1000)
        sock.sendall(command(b"PING") + command(b"ECHO", b"next")[:-2])
        receive_exact(sock, b"+PONG\r\n")
        sock.sendall(b"\r\n")
        receive_exact(sock, bulk(b"next"))


def check_ttl_commands():
    with connect() as sock, connect() as other:
        exchanges = [
            ((b"TTL", b"missing"), b":-2\r\n"),
            ((b"EXPIRE", b"missing", b"1"), b":0\r\n"),
            ((b"PERSIST", b"missing"), b":0\r\n"),
            ((b"SET", b"expires", b"value"), b"+OK\r\n"),
            ((b"TTL", b"expires"), b":-1\r\n"),
            ((b"expire", b"expires", b"1"), b":1\r\n"),
            ((b"EXPIRE", b"expires", b"30"), b":1\r\n"),
            ((b"EXPIRE", b"expires", b"1"), b":1\r\n"),
            ((b"EXPIRE", b"expires", b"1.5"), b"-ERR value is not an integer or out of range\r\n"),
            ((b"EXPIRE", b"expires", b"9223372036854775807"), b"-ERR invalid expire time\r\n"),
            ((b"SET", b"persists", b"value"), b"+OK\r\n"),
            ((b"EXPIRE", b"persists", b"1"), b":1\r\n"),
            ((b"persist", b"persists"), b":1\r\n"),
            ((b"PERSIST", b"persists"), b":0\r\n"),
            ((b"SET", b"replaced", b"old"), b"+OK\r\n"),
            ((b"EXPIRE", b"replaced", b"1"), b":1\r\n"),
            ((b"SET", b"replaced", b"new"), b"+OK\r\n"),
            ((b"ZADD", b"ttl-board", b"1", b"alice"), b":1\r\n"),
            ((b"EXPIRE", b"ttl-board", b"1"), b":1\r\n"),
            ((b"ZADD", b"ttl-board", b"2", b"bob"), b":1\r\n"),
            ((b"SET", b"zero", b"value"), b"+OK\r\n"),
            ((b"EXPIRE", b"zero", b"0"), b":1\r\n"),
            ((b"TTL", b"zero"), b":-2\r\n"),
            ((b"SET", b"negative", b"value"), b"+OK\r\n"),
            ((b"EXPIRE", b"negative", b"-1"), b":1\r\n"),
            ((b"GET", b"negative"), b"$-1\r\n"),
        ]
        sock.sendall(b"".join(command(*args) for args, _ in exchanges))
        receive_exact(sock, b"".join(reply for _, reply in exchanges))
        # No requests arrive while these timers expire. Both runners must still
        # preserve permanent keys and avoid exposing expired strings/sets.
        time.sleep(1.2)
        exchanges = [
            ((b"GET", b"expires"), b"$-1\r\n"),
            ((b"TTL", b"expires"), b":-2\r\n"),
            ((b"PERSIST", b"expires"), b":0\r\n"),
            ((b"GET", b"persists"), bulk(b"value")),
            ((b"TTL", b"persists"), b":-1\r\n"),
            ((b"GET", b"replaced"), bulk(b"new")),
            ((b"TTL", b"replaced"), b":-1\r\n"),
            ((b"ZCARD", b"ttl-board"), b":0\r\n"),
            ((b"KEYS",), array(b"persists", b"replaced")),
            ((b"DEL", b"persists", b"replaced", b"ttl-board"), b":2\r\n"),
        ]
        other.sendall(b"".join(command(*args) for args, _ in exchanges))
        receive_exact(other, b"".join(reply for _, reply in exchanges))


def check_client_timer_messages():
    result = subprocess.run(
        [str(CLIENT)],
        input=('SET cli-timer value\nttl cli-timer\nexpire cli-timer 30\nTTL cli-timer\n'
               'persist cli-timer\nPERSIST cli-timer\nEXPIRE cli-timer invalid\n'
               'GET cli-timer\nDEL cli-timer\nTTL cli-timer\n'
               'EXPIRE cli-timer 30\nPERSIST cli-timer\nexit\n'),
        capture_output=True, text=True, timeout=5, check=True)
    assert not result.stderr, result.stderr
    assert "EXPIRE, TTL, PERSIST" in result.stdout
    assert "No timer for this key." in result.stdout
    assert "Timer set to 30 seconds." in result.stdout
    assert re.search(r"Time remaining: (29|30) seconds\.", result.stdout), result.stdout
    assert "Timer removed; this key has no expiration." in result.stdout
    assert "No timer to remove (key has no timer or does not exist)." in result.stdout
    assert "Timer expired or key does not exist." in result.stdout
    assert "Key does not exist or has expired; no timer was set." in result.stdout
    assert "(error) ERR value is not an integer or out of range" in result.stdout
    assert "(integer) 1" in result.stdout  # DEL keeps its normal integer display.

    # One-command invocations use the same presentation as interactive queries.
    for args, expected in (
        (["TTL", "cli-timer"], "Timer expired or key does not exist.\n"),
        (["EXPIRE", "cli-timer", "30"], "Key does not exist or has expired; no timer was set.\n"),
        (["PERSIST", "cli-timer"], "No timer to remove (key has no timer or does not exist).\n"),
    ):
        result = subprocess.run([str(CLIENT), *args], capture_output=True,
                                text=True, timeout=5, check=True)
        assert result.stdout == expected, result.stdout
    with connect() as sock:
        sock.sendall(command(b"SET", b"cli-timer", b"value"))
        receive_exact(sock, b"+OK\r\n")
    result = subprocess.run([str(CLIENT), "EXPIRE", "cli-timer", "0"],
                            capture_output=True, text=True, timeout=5, check=True)
    assert result.stdout == "Key deleted immediately (nonpositive TTL).\n"
    with connect() as sock:
        sock.sendall(command(b"GET", b"cli-timer"))
        receive_exact(sock, b"$-1\r\n")


def check_idle_timers():
    # The test server uses an 800 ms uniform timeout. Read activity counts even
    # before a complete command exists; ordinary PING replies count as writes.
    with connect() as idle, connect() as active, connect() as partial:
        partial.sendall(b"*2\r\n$4\r\nECHO\r\n$12\r\n")
        for _ in range(8):
            time.sleep(0.15)
            active.sendall(command(b"PING"))
            receive_exact(active, b"+PONG\r\n")
            partial.sendall(b"x")
        idle.settimeout(2)
        assert idle.recv(1) == b"", "inactive connection was not closed"
        partial.sendall(b"xxxx\r\n")
        receive_exact(partial, bulk(b"x" * 12))
        active.sendall(command(b"PING"))
        receive_exact(active, b"+PONG\r\n")
    # No other client traffic can wake poll: its deadline must do that itself.
    with connect() as idle:
        idle.settimeout(3)
        start = time.monotonic()
        assert idle.recv(1) == b"", "idle timer did not wake poll"
        assert time.monotonic() - start >= 0.6, "connection timed out too early"
    # A new connection may reuse an expired descriptor; old list links must be gone.
    for _ in range(20):
        with connect() as sock:
            sock.sendall(command(b"PING"))
            receive_exact(sock, b"+PONG\r\n")

    # Fill the output path while the peer never reads. EAGAIN and readiness
    # alone must not keep this connection alive indefinitely.
    with connect() as slow, connect() as active:
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        slow.settimeout(4)
        finished = threading.Event()
        errors = []

        def fill_output():
            try:
                frame = command(b"ECHO", b"x" * 65536)
                for _ in range(1024):
                    slow.sendall(frame)
            except OSError as error:
                errors.append(error)
            finally:
                finished.set()

        sender = threading.Thread(target=fill_output, daemon=True)
        sender.start()
        try:
            for _ in range(30):
                if finished.wait(0.1):
                    break
                active.sendall(command(b"PING"))
                receive_exact(active, b"+PONG\r\n")
            assert finished.is_set(), "stalled output connection did not time out"
            assert errors and isinstance(errors[0], (ConnectionResetError, BrokenPipeError)), errors
            active.sendall(command(b"PING"))
            receive_exact(active, b"+PONG\r\n")
        finally:
            slow.close()
            sender.join(timeout=5)
        assert not sender.is_alive(), "stalled writer test did not finish"


def check_protocol_errors():
    for invalid in (b"*0\r\n", b"*1\r\n$65537\r\n", b"*1\r\n$1\r\nxXX"):
        with connect() as sock:
            sock.sendall(command(b"PING") + invalid)
            receive_exact(sock, b"+PONG\r\n")
            with sock.makefile("rb") as stream:
                reply = stream.readline()
                assert reply.startswith(b"-ERR Protocol error: ") and reply.endswith(b"\r\n")
                assert stream.read(1) == b"", "protocol errors must close the connection"


def check_half_close_and_limits():
    with connect() as sock:
        payload = b"x" * (64 * 1024)
        sock.sendall(command(b"ECHO", payload) + command(b"PING"))
        sock.shutdown(socket.SHUT_WR)
        receive_exact(sock, bulk(payload) + b"+PONG\r\n")
        assert sock.recv(1) == b""
    with connect() as sock:
        sock.sendall(command(b"PING") + b"*2\r\n$4\r\nECHO\r\n$5\r\nab")
        sock.shutdown(socket.SHUT_WR)
        receive_exact(sock, b"+PONG\r\n")
        assert sock.recv(1) == b"", "truncated request must be closed"
    # This frame is below 1 MiB but well above the read budget. The command
    # handler rejects its argument count only after the whole frame is parsed.
    with connect() as sock:
        sock.sendall(command(b"PING", *([b"x" * 65536] * 14)))
        receive_exact(sock, b"-ERR wrong number of arguments for PING\r\n")


def check_multiple_clients(process, mode):
    with contextlib.ExitStack() as stack:
        idle = [stack.enter_context(connect()) for _ in range(32)]
        # Keep an incomplete frame open while unrelated clients make progress.
        idle[0].sendall(b"*2\r\n$4\r\nECHO\r\n$10\r\na")

        def exchange(index):
            with connect() as sock:
                value = str(index).encode()
                for _ in range(10):
                    sock.sendall(command(b"ECHO", value))
                    receive_exact(sock, bulk(value))

        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            list(pool.map(exchange, range(24)))
        if mode == "event-loop":
            assert len(os.listdir(f"/proc/{process.pid}/task")) == 1, "event loop created threads"
        idle[0].sendall(b"bcdefghij\r\n")
        receive_exact(idle[0], bulk(b"abcdefghij"))


def check_slow_reader():
    # Enough replies to fill kernel send buffers. The sender must run separately:
    # bounded server buffers deliberately pause reads while replies are pending.
    with connect() as slow:
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
        slow.settimeout(15)
        payload = bytes(range(256)) * 256
        count = 128
        frame = command(b"ECHO", payload)
        errors = []
        started = threading.Event()

        def send_pipeline():
            try:
                started.set()
                for _ in range(count):
                    slow.sendall(frame)
                slow.shutdown(socket.SHUT_WR)
            except BaseException as error:
                errors.append(error)

        sender = threading.Thread(target=send_pipeline, daemon=True)
        sender.start()
        started.wait(timeout=1)
        time.sleep(0.2)  # Let pending replies fill the server's socket buffer.
        try:
            with connect() as other:
                other.settimeout(2)
                other.sendall(command(b"PING"))
                receive_exact(other, b"+PONG\r\n")
            receive_exact(slow, bulk(payload) * count)
            assert slow.recv(1) == b""
        finally:
            slow.close()
            sender.join(timeout=5)
        assert not sender.is_alive(), "pipeline sender did not finish"
        assert not errors, errors


def check_disconnects():
    for _ in range(40):
        with connect() as sock:
            sock.sendall(command(b"ECHO", b"x" * 65536))
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    with connect() as sock:
        sock.sendall(command(b"PING"))
        receive_exact(sock, b"+PONG\r\n")


def main():
    result = subprocess.run([str(SERVER), "--help"], capture_output=True, text=True, check=True)
    assert "threaded|event-loop" in result.stdout
    assert "regular|offset" in result.stdout
    assert "--idle-timeout-ms" in result.stdout and "60000" in result.stdout
    for args in (
        ["--mode", "invalid"],
        ["--mode", "event-loop", "--buffer", "invalid"],
        ["--buffer", "regular"],
        ["--mode", "threaded", "--buffer", "offset"],
        ["--mode", "event-loop", "--buffer"],
        ["--mode", "event-loop", "--mode", "threaded"],
        ["--mode", "event-loop", "--buffer", "offset", "--buffer", "regular"],
        ["--unknown", "value"],
        ["--idle-timeout-ms", "100"],
        ["--mode", "threaded", "--idle-timeout-ms", "100"],
        ["--mode", "event-loop", "--idle-timeout-ms", "0"],
        ["--mode", "event-loop", "--idle-timeout-ms", "-1"],
        ["--mode", "event-loop", "--idle-timeout-ms", "abc"],
        ["--mode", "event-loop", "--idle-timeout-ms", "2147483648"],
        ["--mode", "event-loop", "--idle-timeout-ms", "100", "--idle-timeout-ms", "200"],
        ["--mode", "event-loop", "--idle-timeout-ms"],
    ):
        result = subprocess.run([str(SERVER), *args], capture_output=True, timeout=5)
        assert result.returncode != 0, args
    with running_server(None):
        check_commands()  # No arguments keeps the original launch command working.
    with running_server("event-loop", "regular", reverse_options=True):
        check_commands()
    for mode, buffer in (("threaded", None), ("event-loop", None),
                         ("event-loop", "regular"), ("event-loop", "offset")):
        logs = []
        with running_server(mode, buffer, log_capture=logs) as process:
            check_commands()
            check_storage()
            check_ttl_commands()
            check_client_timer_messages()
            check_fragmentation_and_pipeline()
            check_protocol_errors()
            check_half_close_and_limits()
            check_multiple_clients(process, mode)
            check_slow_reader()
            check_disconnects()
        check_connection_logs(logs[0], {"client disconnected", "protocol error",
                                       "client disconnected during incomplete request"})
        print(f"Integration tests passed ({mode}, buffer={buffer or 'default'})")
    for buffer in ("regular", "offset"):
        logs = []
        with running_server("event-loop", buffer, idle_timeout_ms=800, log_capture=logs):
            check_idle_timers()
        check_connection_logs(logs[0], {"idle timeout", "client disconnected"})
        print(f"Idle timer integration tests passed (buffer={buffer})")


if __name__ == "__main__":
    main()
