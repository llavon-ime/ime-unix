#!/usr/bin/env python3
"""Frozen, independent stress set for structured-token spelling penalties.

Generate and hash cases before running any decoder. Never tune from this set;
after inspecting results it becomes validation/development data, not a holdout.
Reports names and families separately rather than pooling random identifiers
with ordinary prose. No model accuracy or natural-traffic estimate is implied.
"""

import argparse
import hashlib
import json
import random
import subprocess
from collections import defaultdict
from pathlib import Path


SEED = 2026093002


def cases():
    rng = random.Random(SEED)
    families = {
        "human-names": ["nguyen", "przemyslaw", "srinivasan", "tsvetlana", "bjornsson",
                        "hryhoriy", "chaitanya", "kristjansson", "qianwen", "zhenghao",
                        "mbaye", "dijkstra", "szymanski", "wroblewski", "schwarzenegger",
                        "kaczynski", "thirunavukkarasu", "wladyslaw", "khachaturian", "yoshihiro"],
        "technical-names": ["protobuf", "libstdcxx", "grpc", "ioctl", "epoll", "kqueue", "zstd",
                            "xxhash", "sha256", "blake3", "qdrant", "kvstore", "etcd", "vcpkg",
                            "bpftrace", "nixpkgs", "pthread", "sysctl", "nginx", "xgboost"],
        "random-letters": ["".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(n))
                           for n in (4, 8, 16, 24, 32) for _ in range(4)],
        "hex-identifiers": ["".join(rng.choice("0123456789abcdef") for _ in range(n))
                            for n in (8, 16, 24, 32) for _ in range(5)],
    }
    # Every group has 20 roots. Some begin with digits: these are valid email
    # local parts/domain labels even where the existing lexer has incomplete
    # grammar coverage. Always distinguish old failures from new regressions.
    patterns = ["{s}_cache", "{s}@mail.invalid", "{s}.invalid", "{s}://host.invalid/path"]
    prefixes = {
        "standard": [("su3", "你"), ("ji3", "我"), ("su3cl3", "你好")],
        "hsu": [("nef", "你"), ("xhf", "我"), ("nefhwf", "你好")],
    }
    result = []
    for family, stems in families.items():
        for stem in stems:
            for shape, pattern in enumerate(patterns):
                literal = pattern.format(s=stem)
                # A URI scheme starts with a letter; numeric-leading schemes
                # are excluded instead of falsely calling them valid URLs.
                if shape == 3 and not stem[0].isalpha():
                    continue
                for layout, mixed_prefixes in prefixes.items():
                    for raw_prefix, text_prefix in [("", "")] + mixed_prefixes:
                        result.append({"family": family, "root": stem, "shape": shape,
                                       "layout": layout, "intent": "mixed" if raw_prefix else "literal",
                                       "raw": raw_prefix + literal, "expected": text_prefix + literal})
    return result


def run(binary, row):
    output = subprocess.check_output([str(binary), row["layout"], row["raw"]], text=True, timeout=10)
    lines = output.splitlines()
    return {"text": next(line.split(" ", 2)[2] for line in lines if line.startswith("*")),
            "paths": [line[2:].split(" ", 1)[1] for line in lines if line.startswith("*") or line.startswith(" ")]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    fixture = {"seed": SEED, "policy": "freeze-before-inspection-no-tuning", "cases": cases()}
    encoded = (json.dumps(fixture, ensure_ascii=False, indent=2) + "\n").encode()
    path = args.directory / "frozen-holdout-cases.json"
    path.write_bytes(encoded)
    digest = hashlib.sha256(encoded).hexdigest()
    print("Frozen cases:", len(fixture["cases"]), "sha256:", digest, flush=True)
    rows = []
    for row in fixture["cases"]:
        rows.append({**row, "baseline": run(args.baseline, row), "candidate": run(args.candidate, row)})
    groups = defaultdict(list)
    for row in rows:
        groups[(row["family"], row["layout"], row["intent"])].append(row)
    summaries = []
    for (family, layout, intent), group in sorted(groups.items()):
        summary = {"family": family, "layout": layout, "intent": intent, "count": len(group),
                   "baseline_exact": sum(r["baseline"]["text"] == r["expected"] for r in group),
                   "candidate_exact": sum(r["candidate"]["text"] == r["expected"] for r in group),
                   "regressions": sum(r["baseline"]["text"] == r["expected"] != r["candidate"]["text"] for r in group)}
        summaries.append(summary)
        print(json.dumps(summary, ensure_ascii=False), flush=True)
    report = {"fixture_sha256": digest, "summaries": summaries, "rows": rows,
              "binary_sha256": {name: hashlib.sha256(binary.read_bytes()).hexdigest()
                                for name, binary in [("baseline", args.baseline), ("candidate", args.candidate)]}}
    (args.directory / "holdout-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
