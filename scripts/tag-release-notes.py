#!/usr/bin/env python3
"""Extract release notes from an annotated Git tag message."""

import argparse
from pathlib import Path
import re
import subprocess


def git_output(*arguments, cwd=None):
    return subprocess.run(
        ["git", *arguments],
        cwd=cwd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout


def extract(tag, cwd=None):
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
        raise ValueError("release tag must use v<major>.<minor>.<patch>")

    reference = f"refs/tags/{tag}"
    try:
        object_type = git_output("cat-file", "-t", reference, cwd=cwd).strip()
    except subprocess.CalledProcessError as error:
        raise ValueError(f"release tag does not exist: {tag}") from error
    if object_type != "tag":
        raise ValueError(f"release tag must be annotated: {tag}")

    notes = git_output(
        "for-each-ref",
        "--format=%(contents:subject)%0a%0a%(contents:body)",
        reference,
        cwd=cwd,
    ).strip()
    if not notes:
        raise ValueError(f"annotated tag has no release notes: {tag}")
    return notes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        notes = extract(args.tag)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    args.output.write_text(f"{notes}\n", encoding="utf-8")


if __name__ == "__main__":
    main()
