#!/usr/bin/env python3
"""Download and verify one file from the current Hugging Face model revision."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import time
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


def model_file_metadata(manifest, filename):
    revision = manifest.get("sha", "")
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("Hugging Face returned an invalid model revision")

    sibling = next(
        (entry for entry in manifest.get("siblings", []) if entry.get("rfilename") == filename),
        None,
    )
    if sibling is None:
        raise ValueError(f"model revision does not contain {filename}")
    size = sibling.get("size")
    checksum = sibling.get("lfs", {}).get("sha256", "")
    if not isinstance(size, int) or size <= 0:
        raise ValueError("Hugging Face returned an invalid model file size")
    if not re.fullmatch(r"[0-9a-f]{64}", checksum):
        raise ValueError("Hugging Face model file has no valid LFS SHA-256")
    return revision, size, checksum


def request_bytes(url, attempts=3):
    request = Request(
        url,
        headers={"Cache-Control": "no-cache", "User-Agent": "llavon-ime-release"},
    )
    for attempt in range(1, attempts + 1):
        try:
            with urlopen(request, timeout=120) as response:
                return response.read()
        except (HTTPError, URLError, TimeoutError):
            if attempt == attempts:
                raise
            time.sleep(attempt)
    raise AssertionError("unreachable")


def download(repository, filename, output):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("invalid Hugging Face repository")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", filename):
        raise ValueError("invalid model filename")

    encoded_repository = quote(repository, safe="/")
    manifest_url = (
        f"https://huggingface.co/api/models/{encoded_repository}/revision/main?blobs=true"
    )
    manifest = json.loads(request_bytes(manifest_url))
    revision, expected_size, expected_checksum = model_file_metadata(manifest, filename)

    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = Path(f"{output}.partial")
    download_url = (
        f"https://huggingface.co/{encoded_repository}/resolve/{revision}/{quote(filename)}"
    )
    request = Request(download_url, headers={"User-Agent": "llavon-ime-release"})
    for attempt in range(1, 4):
        partial.unlink(missing_ok=True)
        digest = hashlib.sha256()
        size = 0
        try:
            with urlopen(request, timeout=120) as response, partial.open("wb") as stream:
                while chunk := response.read(1024 * 1024):
                    stream.write(chunk)
                    digest.update(chunk)
                    size += len(chunk)
            if size != expected_size:
                raise ValueError(
                    f"model size mismatch: expected {expected_size}, downloaded {size}"
                )
            if digest.hexdigest() != expected_checksum:
                raise ValueError("model SHA-256 does not match the Hugging Face manifest")
            partial.replace(output)
            break
        except (HTTPError, URLError, TimeoutError, OSError, ValueError):
            partial.unlink(missing_ok=True)
            if attempt == 3:
                raise
            time.sleep(attempt)

    print(f"revision={revision} sha256={expected_checksum} size={expected_size}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--filename", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    download(args.repository, args.filename, args.output)


if __name__ == "__main__":
    main()
