#!/usr/bin/env python3
"""Exercise the real listener without loading a model: reconnects must reap readers."""

import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def receive(client, size):
    data = bytearray()
    while len(data) < size:
        chunk = client.recv(size - len(data))
        if not chunk:
            raise AssertionError("service disconnected before its response")
        data.extend(chunk)
    return bytes(data)


def status(endpoint):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(3)
        connect(client, endpoint)
        client.sendall(bytes.fromhex("03000000050000"))
        length, = struct.unpack("<I", receive(client, 4))
        assert length < 1024
        response = receive(client, length)
        assert response[:2] == b"\x05\x01"
        return response[2:18]


def connect(client, endpoint):
    # Nonblocking AF_UNIX connect returns EAGAIN when the accept backlog is
    # full, rather than the EINPROGRESS used by TCP. Retry within a fixed bound.
    deadline = time.monotonic() + 3
    while True:
        try:
            client.connect(str(endpoint))
            return
        except BlockingIOError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.002)


def resources(pid):
    root = Path(f"/proc/{pid}")
    return len(list((root / "fd").iterdir())), len(list((root / "task").iterdir()))


def main():
    with tempfile.TemporaryDirectory(prefix="llavon-listener-") as temporary:
        root = Path(temporary)
        endpoint = root / "ime.sock"
        model = root / "not-loaded.gguf"
        model.touch()
        environment = dict(os.environ, XDG_STATE_HOME=str(root / "state"))
        with (root / "service.log").open("w+") as log:
            process = subprocess.Popen(
                [sys.argv[1], "--socket", str(endpoint), "--model", str(model),
                 "--tables", sys.argv[2], "--idle-timeout", "60"],
                stdout=log, stderr=log, env=environment,
            )
            try:
                deadline = time.monotonic() + 5
                while not endpoint.exists() and process.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.01)
                epoch = status(endpoint)
                time.sleep(0.3)
                baseline = resources(process.pid) if sys.platform.startswith("linux") else None
                for index in range(2000):
                    assert status(endpoint) == epoch
                    if index % 10 == 0:
                        # Abrupt disconnect in a frame header must also release the reader.
                        with socket.socket(socket.AF_UNIX) as client:
                            connect(client, endpoint)
                            client.sendall(b"\x10\x00")
                time.sleep(0.5)
                if baseline:
                    final = resources(process.pid)
                    assert final[0] <= baseline[0] + 2, ("descriptor accumulation", baseline, final)
                    assert final[1] <= baseline[1] + 1, ("reader accumulation", baseline, final)
                # Admission is bounded even for peers that keep idle streams
                # alive. Closing them must promptly restore usable capacity.
                held = []
                try:
                    for _ in range(128):
                        client = socket.socket(socket.AF_UNIX)
                        client.settimeout(3)
                        connect(client, endpoint)
                        held.append(client)
                    time.sleep(0.2)
                    with socket.socket(socket.AF_UNIX) as extra:
                        extra.settimeout(3)
                        connect(extra, endpoint)
                        try:
                            assert extra.recv(1) == b""
                        except ConnectionResetError:
                            pass
                finally:
                    for client in held:
                        client.close()
                time.sleep(0.3)
                assert status(endpoint) == epoch
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(3)
                    connect(client, endpoint)
                    client.sendall(bytes.fromhex("020000000600"))
                    assert receive(client, 7) == bytes.fromhex("03000000060101")
                assert process.wait(timeout=5) == 0
                assert not endpoint.exists()
            except Exception:
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
    print("2,000 real-service reconnects and fragmented disconnects passed")


if __name__ == "__main__":
    main()
