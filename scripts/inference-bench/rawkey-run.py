#!/usr/bin/env python3
"""Raw-key latency/commit confirmation with a separate real service per spec."""
import argparse
import hashlib
import json
import math
import random
import statistics
import subprocess
import time
from pathlib import Path


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--service", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=32)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--engine-source", type=Path, help="source snapshot used to compile the probe")
    args = parser.parse_args()
    if args.samples < 1 or args.rounds < 1:
        parser.error("samples and rounds must be positive")
    args.output.mkdir(parents=True, exist_ok=False)
    plan = [dict(threads=threads, layers=layers, paced=paced, samples=args.samples, round=repeat)
            for threads, layers in [(4, 0), (6, 0), (8, 0), (2, 999), (8, 999)]
            for paced in [False, True] for repeat in range(args.rounds)]
    random.Random(20260930).shuffle(plan)
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in [args.binary, args.service, args.model]}
    source_hashes = {}
    if args.engine_source:
        source_hashes = {str(path.relative_to(args.engine_source)): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in sorted(args.engine_source.rglob("*")) if path.suffix in [".cpp", ".hpp", ".h"]}
    save(args.output / "manifest.json", dict(configurations=plan, seed=20260930, sha256=hashes,
         engine_source_sha256=source_hashes,
         measurement="final tone key to settled Engine render including Unix IPC; not physical app display latency"))
    groups = {}
    previews = {}
    for index, spec in enumerate(plan):
        spec_path = args.output / f"spec-{index:03d}.json"
        result_path = args.output / f"result-{index:03d}.json"
        save(spec_path, spec)
        with (args.output / f"log-{index:03d}.txt").open("w") as log:
            subprocess.run([str(args.binary.resolve()), str(args.service.resolve()), str(args.model.resolve()),
                            str(spec_path.resolve()), str(result_path.resolve())], stdout=log, stderr=log,
                           check=True, timeout=300)
        result = json.loads(result_path.read_text())
        for key, preview in result["preedits"].items():
            if key in previews and previews[key] != preview:
                raise ValueError(f"configuration changed raw-key preview/commit: {key}")
            previews[key] = preview
        for row in result["records"]:
            key = (spec["threads"], spec["layers"], spec["paced"], row["workload"])
            groups.setdefault(key, []).append(row["tone_to_settled_render_ms"])
        print(f"{index+1}/{len(plan)} {spec}", flush=True)
        time.sleep(1)
    summary = []
    for (threads, layers, paced, workload), values in sorted(groups.items()):
        ordered = sorted(values)
        summary.append(dict(threads=threads, layers=layers, paced=paced, characters=[1, 2, 6][workload],
                            n=len(values), p50_ms=statistics.median(values),
                            p95_ms=ordered[math.ceil(len(values)*.95)-1]))
    save(args.output / "summary.json", dict(configurations=len(plan), requests=sum(len(v) for v in groups.values()),
                                           matching_previews=previews, summary=summary))
    for row in summary:
        print(row)


if __name__ == "__main__":
    main()
