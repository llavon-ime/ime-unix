#!/usr/bin/env python3
"""Pinned trainer discovery must work without the public GitHub API quota."""

import json
import hashlib
import os
import platform
from pathlib import Path
import subprocess
import sys
import tempfile
import tarfile


def main():
    executable, commit = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="llavon-trainer-discovery-") as directory:
        root = Path(directory)
        fixture = root / "fixture"
        fixture.mkdir()
        (fixture / "llavon-lora").write_text('#!/bin/sh\nprintf \'{"trainerApi":2}\\n\'\n')
        (fixture / "libtrainer.so").write_bytes(b"test-only native library marker")
        archive = root / "fixture.tar.gz"
        with tarfile.open(archive, "w:gz") as package:
            for file in fixture.iterdir():
                package.add(file, arcname=file.name)
        version = "2000.1"
        manifest = {"schema": 1, "trainerApi": 1, "version": version, "commit": commit, "assets": {}}
        for target in ["linux-x64-cpu", "osx-arm64-cpu"]:
            name = f"llavon-lora-{version}-{target}.tar.gz"
            manifest["assets"][target] = {
                "name": name,
                "url": f"https://github.com/llavon-ime/lora-trainer/releases/download/v{version}/{name}",
                "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(), "size": archive.stat().st_size,
            }
        manifest_path = root / "manifest.json"
        manifest_path.write_text(json.dumps(manifest))
        curl = root / "curl"
        curl.write_text(f"#!{sys.executable}\n" + r'''
import json, os, pathlib, sys
args = sys.argv[1:]
url = args[-1]
with open(os.environ["TEST_CURL_LOG"], "a") as log:
    log.write(url + "\n")
mode = os.environ["TEST_CURL_MODE"]
commit = os.environ["TEST_TRAINER_COMMIT"]
if "/releases/download/commit-" in url:
    if mode == "missing":
        sys.exit(22)
    manifest = json.loads(pathlib.Path(os.environ["TEST_MANIFEST"]).read_text())
    if mode == "mismatch":
        manifest["commit"] = "0" * 40
elif "/releases/download/latest/" in url:
    manifest = {"schema": 1, "trainerApi": 1, "version": "2000.2", "commit": commit}
elif "/releases/download/v2000.1/latest.json" in url:
    manifest = json.loads(pathlib.Path(os.environ["TEST_MANIFEST"]).read_text())
    if mode == "wrong-pinned":
        manifest["commit"] = "0" * 40
elif url.endswith(".tar.gz"):
    data = bytearray(pathlib.Path(os.environ["TEST_ARCHIVE"]).read_bytes())
    if mode == "corrupt":
        data[-1] ^= 1
    pathlib.Path(args[args.index("--output") + 1]).write_bytes(data)
    sys.exit(0)
else:
    # Simulate exhausted anonymous API quota. This must never be needed when
    # the commit manifest (or the legacy rolling fallback) is available.
    sys.exit(22)
pathlib.Path(args[args.index("--output") + 1]).write_text(json.dumps(manifest))
''')
        curl.chmod(0o755)
        for mode in ["pinned", "missing", "mismatch"]:
            log = root / f"{mode}.log"
            output = root / mode
            environment = dict(os.environ, PATH=f"{root}{os.pathsep}{os.environ['PATH']}",
                               TEST_CURL_LOG=str(log), TEST_CURL_MODE=mode, TEST_TRAINER_COMMIT=commit,
                               TEST_MANIFEST=str(manifest_path), TEST_ARCHIVE=str(archive))
            result = subprocess.run([executable, "check-trainer", "--output-dir", str(output)],
                                    env=environment, text=True, capture_output=True, timeout=10)
            assert result.returncode == 0, result.stderr
            expected_release = "2000.1" if mode == "pinned" else "2000.2"
            assert f"release={expected_release}" in result.stdout, result.stdout
            requests = log.read_text().splitlines()
            assert requests[0].endswith(f"/commit-{commit}/latest.json"), requests
            assert len(requests) == (1 if mode == "pinned" else 2), requests
            assert not any("api.github.com" in url for url in requests), requests
            assert not list(output.glob("*.partial"))
        supported = ((sys.platform.startswith("linux") and platform.machine() == "x86_64") or
                     (sys.platform == "darwin" and platform.machine() == "arm64"))
        if supported:
            for mode in ["install", "corrupt", "wrong-pinned"]:
                log = root / f"{mode}.log"
                output = root / mode
                environment = dict(os.environ, PATH=f"{root}{os.pathsep}{os.environ['PATH']}",
                                   TEST_CURL_LOG=str(log), TEST_CURL_MODE=mode, TEST_TRAINER_COMMIT=commit,
                                   TEST_MANIFEST=str(manifest_path), TEST_ARCHIVE=str(archive),
                                   LLAVON_IME_LORA_TRAINER_CACHE=str(root / f"{mode}-cache"),
                                   LLAVON_IME_LORA_RELEASE_ATTEMPTS="1")
                result = subprocess.run([executable, "install-trainer", "--output-dir", str(output)],
                                        env=environment, text=True, capture_output=True, timeout=10)
                requests = log.read_text().splitlines()
                assert not any("api.github.com" in url for url in requests), requests
                if mode == "install":
                    assert result.returncode == 0, result.stderr
                    stamp = json.loads((output / "trainer-release.json").read_text())
                    assert stamp["commit"] == commit and stamp["version"] == version
                    assert (output / "llavon-lora").is_file()
                else:
                    assert result.returncode != 0
                    expected = "checksum verification" if mode == "corrupt" else "differs from pinned submodule"
                    assert expected in result.stderr, result.stderr
                    assert not (output / "llavon-lora").exists()
                assert not list(output.glob("*.partial"))
    print("Pinned manifest discovery and legacy fallbacks passed without API quota")


if __name__ == "__main__":
    main()
