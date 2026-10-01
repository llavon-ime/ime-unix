#!/usr/bin/env python3
"""Compare two mixed_input_probe binaries on generated structured boundaries.

No expected text or corpus data is passed into the decoder. This is a small,
deterministic diagnostic sweep, not a natural-user accuracy benchmark.
"""

import argparse
import json
import subprocess
from pathlib import Path


STEMS = (
    "user dev admin hello example report output test server data github localhost "
    "python florpington qwerty xylophonic satoshi zhang li wei abc xyz foo bar mail "
    "nefarious pandas end-to-end snake fooBar"
).split()
CONTROLS = [stem + tail for stem in STEMS for tail in ("_name42", "@example.com", ".org")] + [
    "https://example.org/docs?v=2", "http://localhost:8080/test", "ftp://server.local/data",
    "custom+v2://host/path", "abc123_foo", "x86_64", "mp3_file", "user_2026_name",
    "a@example.com", "z@example.net", "a_b", "v2.1.0", "_private_name",
]
PREFIXES = {
    "standard": [("su3", "你"), ("cl3", "好"), ("ji3", "我"), ("su3cl3", "你好")],
    "hsu": [("nef", "你"), ("hwf", "好"), ("xhf", "我"), ("nefhwf", "你好")],
}


def preview(binary, layout, raw):
    output = subprocess.check_output([str(binary), layout, raw], text=True, timeout=10)
    return next(line.split(" ", 2)[2] for line in output.splitlines() if line.startswith("*"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    rows = []
    for layout, prefixes in PREFIXES.items():
        for raw_prefix, text_prefix in [("", "")] + prefixes:
            for literal in CONTROLS:
                raw = raw_prefix + literal
                rows.append({
                    "layout": layout, "raw": raw, "literal": literal, "prefix": text_prefix,
                    "expected": text_prefix + literal,
                    "baseline": preview(args.baseline, layout, raw),
                    "candidate": preview(args.candidate, layout, raw),
                })
    args.report.write_text(json.dumps(rows, ensure_ascii=False, indent=2) + "\n")
    for layout in PREFIXES:
        for mixed in (False, True):
            group = [row for row in rows if row["layout"] == layout and bool(row["prefix"]) == mixed]
            counts = {mode: sum(row[mode] == row["expected"] for row in group)
                      for mode in ("baseline", "candidate")}
            regressions = sum(row["baseline"] == row["expected"] != row["candidate"] for row in group)
            print(layout, "mixed" if mixed else "literal", f"n={len(group)}", counts,
                  f"regressions={regressions}")


if __name__ == "__main__":
    main()
