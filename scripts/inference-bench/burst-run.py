#!/usr/bin/env python3
"""Request-scoped caller/actual-worker QoS experiment with baseline controls."""
import argparse
import hashlib
import json
import math
import random
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")


def p95(values):
    return sorted(values)[math.ceil(len(values) * .95) - 1]


def summarize(output):
    groups = {}
    refs = {}
    total = 0
    for path in sorted(output.glob("result-*.json")):
        result = json.loads(path.read_text())
        spec = result["spec"]
        identity = (spec["backend"], spec["threads"], spec["idle_ms"], spec["mode"])
        group = groups.setdefault(identity, [])
        group.extend(result["records"])
        total += sum(len(r["predict_ms"]) for r in result["records"])
        for key, top in result["signatures"].items():
            query = (spec["backend"], key)
            if query in refs and refs[query] != top:
                raise ValueError(f"top1 differs across boost controls: {query}")
            refs[query] = top
    rows = []
    for (backend, threads, idle, mode), records in sorted(groups.items()):
        first = [r["first_including_setup_ms"] for r in records]
        following = [t for r in records for t in r["predict_ms"][1:]]
        accounting = [r["accounting"] for r in records if "cpu_ns" in r["accounting"]]
        rows.append(dict(backend=backend, threads=threads, idle_ms=idle, mode=mode, n=len(records),
            first_p50_ms=statistics.median(first), first_p95_ms=p95(first),
            following_p50_ms=statistics.median(following) if following else None,
            total_p50_ms=statistics.median(r["total_ms"] for r in records),
            cpu_p50_ms=statistics.median(r["process_cpu_ms"] for r in records),
            setup_p50_ms=statistics.median(r["setup_ms"] for r in records),
            restore_p50_ms=statistics.median(r["restore_ms"] for r in records),
            workers=sum(r["workers"]["created"] for r in records),
            workers_initiated=sum(r["workers"]["initiated"] for r in records),
            workers_interactive=sum(r["workers"]["interactive"] for r in records),
            workers_restored=sum(r["workers"]["restored"] for r in records),
            worker_errors=sum(r["workers"]["errors"] for r in records),
            performance_cpu_fraction=statistics.median(r["performance_cpu_ns"]/r["cpu_ns"] for r in accounting if r["cpu_ns"]>0) if accounting else None,
            energy_nj_p50=statistics.median(r["energy_nj"] for r in accounting) if accounting else None))
    summary = dict(configurations=len(list(output.glob("result-*.json"))), requests=total,
                   top1_matches=True, summary=rows)
    save(output / "summary.json", summary)
    for row in rows:
        print(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--idle", default="0,600,2000")
    parser.add_argument("--samples", type=int, default=12)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--modes", default="none,caller,workers,static")
    parser.add_argument("--targets", default="cpu:4,cpu:8,metal:4")
    parser.add_argument("--summarize", action="store_true")
    args = parser.parse_args()
    if args.summarize:
        summarize(args.output)
        return
    if args.samples < 1 or args.rounds < 1:
        parser.error("samples/rounds must be positive")
    args.output.mkdir(parents=True, exist_ok=False)
    table = json.loads((ROOT / "ime-core/table/bopomofo_char.json").read_text())
    allowed = [[ord(c) for c in table[reading]] for reading in ["ㄋㄧˇ", "ㄏㄠˇ"]]
    modes = args.modes.split(",")
    idle = [int(value) for value in args.idle.split(",")]
    targets = [(value.split(":")[0], int(value.split(":")[1])) for value in args.targets.split(",")]
    plan = [dict(backend=backend, threads=threads, idle_ms=delay, mode=mode,
                 samples=args.samples, round=repeat, burst_length=4, allowed=allowed)
            for backend, threads in targets for delay in idle for mode in modes for repeat in range(args.rounds)]
    random.Random(20260930).shuffle(plan)
    sources = [Path(__file__), Path(__file__).with_name("burst.cpp")]
    save(args.output / "manifest.json", dict(seed=20260930, plan=plan,
        model_sha256=hashlib.sha256(args.model.read_bytes()).hexdigest(),
        binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        source_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        scope="Native macOS request-scoped QoS hints, no explicit physical frequency control",
        energy="proc_pid_rusage v6 energy_nj is kernel process accounting if available, not measured system rail power; powermetrics needs unavailable sudo"))
    for index, spec in enumerate(plan):
        path = args.output / f"spec-{index:03d}.json"
        save(path, spec)
        with (args.output / f"log-{index:03d}.txt").open("w") as log:
            subprocess.run([str(args.binary.resolve()), str(args.model.resolve()), str(ROOT / "ime-core/table"),
                            str(path.resolve()), str((args.output / f"result-{index:03d}.json").resolve())],
                           stdout=log, stderr=log, check=True, timeout=300)
        print(f"{index+1}/{len(plan)} { {k:v for k,v in spec.items() if k!='allowed'} }", flush=True)
    summarize(args.output)


if __name__ == "__main__":
    main()
