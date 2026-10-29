#!/usr/bin/env python3
"""Smoke-test a macOS prediction service with the installed GGUF model."""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def receive_frame(connection: socket.socket) -> bytes:
    header = connection.recv(4)
    if len(header) != 4:
        raise RuntimeError("prediction service returned a short frame header")
    size = struct.unpack("<I", header)[0]
    body = bytearray()
    while len(body) < size:
        chunk = connection.recv(size - len(body))
        if not chunk:
            raise RuntimeError("prediction service returned a short frame body")
        body.extend(chunk)
    return bytes(body)


def exchange(socket_path: Path, payload: bytes, timeout: float) -> bytes:
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(timeout)
        connection.connect(str(socket_path))
        connection.sendall(struct.pack("<I", len(payload)) + payload)
        return receive_frame(connection)


def wait_for_socket(process: subprocess.Popen[bytes], socket_path: Path, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if socket_path.exists():
            return
        return_code = process.poll()
        if return_code is not None:
            raise RuntimeError(f"prediction service exited during startup with status {return_code}")
        time.sleep(0.05)
    raise RuntimeError("prediction service did not create its socket")


def protocol_error(response: bytes) -> str:
    if len(response) < 38:
        return response.hex()
    message_size = struct.unpack_from("<I", response, 34)[0]
    return response[38 : 38 + message_size].decode("utf-8", errors="replace")


def verify(service: Path, model: Path, tables: Path) -> None:
    for path, description in ((service, "service"), (model, "model"), (tables, "tables")):
        if not path.exists():
            raise RuntimeError(f"{description} path does not exist: {path}")

    with tempfile.TemporaryDirectory(prefix="llavon-ime-service-smoke-") as directory:
        socket_path = Path(directory) / "ime.sock"
        log_path = Path(directory) / "service.log"
        command = [
            str(service),
            "--socket",
            str(socket_path),
            "--model",
            str(model),
            "--tables",
            str(tables),
            "--inference-provider",
            "auto",
            "--context-length",
            "324",
            "--threads",
            "2",
            "--gpu-layers",
            "0",
            "--idle-timeout",
            "60",
        ]
        with log_path.open("wb") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_for_socket(process, socket_path, 30)

                opened = exchange(socket_path, bytes.fromhex("0100"), 10)
                if len(opened) < 18 or opened[:2] != bytes.fromhex("0101"):
                    raise RuntimeError(f"OpenSession failed: {opened.hex()}")
                session_id = opened[2:18]

                reading = "ㄋㄧˇ".encode("utf-16le")
                predict = (
                    bytes([2])
                    + session_id
                    + struct.pack("<QQI", 1, 1, 0)
                    + struct.pack("<I", 1)
                    + bytes([0])
                    + struct.pack("<I", 3)
                    + reading
                )
                prediction = exchange(socket_path, predict, 180)
                if not prediction or prediction[0] != 3:
                    if prediction and prediction[0] == 7:
                        raise RuntimeError(f"Predict failed: {protocol_error(prediction)}")
                    raise RuntimeError(f"Predict returned an unexpected frame: {prediction.hex()}")

                status = exchange(socket_path, bytes.fromhex("050000"), 10)
                if len(status) < 29 or status[:2] != bytes.fromhex("0501"):
                    raise RuntimeError(f"Status failed: {status.hex()}")
                if not status[19]:
                    raise RuntimeError("Predict succeeded but the service reports model_loaded=false")

                closed = exchange(socket_path, bytes([4, 0]) + session_id, 10)
                if len(closed) < 19 or closed[0] != 4 or not closed[-1]:
                    raise RuntimeError(f"CloseSession failed: {closed.hex()}")
            except Exception:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log.flush()
                details = log_path.read_text(errors="replace").strip()
                if details:
                    print("prediction service log:", file=sys.stderr)
                    print(details, file=sys.stderr)
                raise
            else:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--tables", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        verify(arguments.service, arguments.model, arguments.tables)
    except Exception as error:
        print(f"macOS prediction service smoke test failed: {error}", file=sys.stderr)
        return 1
    print("macOS prediction service smoke test passed (Prediction, model_loaded=true).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
