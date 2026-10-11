"""Correctness checks for both connection models; this is not a benchmark."""

import concurrent.futures
import contextlib
import os
from pathlib import Path
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


@contextlib.contextmanager
def running_server(mode, buffer=None, reverse_options=False):
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
        process = subprocess.Popen(args, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 5
            while True:
                if process.poll() is not None:
                    raise RuntimeError("server exited during startup")
                try:
                    with connect():
                        break
                except ConnectionRefusedError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("server did not start")
                    time.sleep(0.02)
            log.seek(0)
            banner = log.readline().decode()
            if mode == "event-loop":
                assert f"buffer={buffer or 'offset'}" in banner, banner
            yield process
            assert process.poll() is None, "server unexpectedly exited"
        except BaseException:
            log.seek(0)
            print(log.read().decode(errors="replace"))
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


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
    for args in (
        ["--mode", "invalid"],
        ["--mode", "event-loop", "--buffer", "invalid"],
        ["--buffer", "regular"],
        ["--mode", "threaded", "--buffer", "offset"],
        ["--mode", "event-loop", "--buffer"],
        ["--mode", "event-loop", "--mode", "threaded"],
        ["--mode", "event-loop", "--buffer", "offset", "--buffer", "regular"],
        ["--unknown", "value"],
    ):
        result = subprocess.run([str(SERVER), *args], capture_output=True, timeout=5)
        assert result.returncode != 0, args
    with running_server(None):
        check_commands()  # No arguments keeps the original launch command working.
    with running_server("event-loop", "regular", reverse_options=True):
        check_commands()
    for mode, buffer in (("threaded", None), ("event-loop", None),
                         ("event-loop", "regular"), ("event-loop", "offset")):
        with running_server(mode, buffer) as process:
            check_commands()
            check_storage()
            check_fragmentation_and_pipeline()
            check_protocol_errors()
            check_half_close_and_limits()
            check_multiple_clients(process, mode)
            check_slow_reader()
            check_disconnects()
        print(f"Integration tests passed ({mode}, buffer={buffer or 'default'})")


if __name__ == "__main__":
    main()
