#!/usr/bin/env python3
"""Randomized, reproducible inference experiments against unmodified ime-core.

Each configuration runs in a separate process, with scheduling set before worker
creation. No settings of the installed input method or other processes change.
"""
import argparse
import hashlib
import json
import math
import platform
import random
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
READINGS = {
    "one": ["ㄋㄧˇ"],
    "two": ["ㄋㄧˇ", "ㄏㄠˇ"],
    "six": ["ㄐㄧㄣ ", "ㄊㄧㄢ ", "ㄊㄧㄢ ", "ㄑㄧˋ", "ㄏㄣˇ", "ㄏㄠˇ"],
    "long": ["ㄒㄧㄣ ", "ㄒㄧㄤ "],
}


def percentile(values, fraction):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)]


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")


def environment():
    commands = {
        "Darwin": [["sysctl", "-n", "hw.model", "hw.physicalcpu", "hw.logicalcpu", "hw.memsize"],
                   ["pmset", "-g", "batt"], ["pmset", "-g", "therm"]],
        "Linux": [["uname", "-srm"], ["lscpu"]],
    }.get(platform.system(), [])
    result = {}
    for command in commands:
        observed = subprocess.run(command, capture_output=True, text=True, timeout=10, check=False)
        result[" ".join(command)] = {"exit": observed.returncode, "output": observed.stdout + observed.stderr}
    return result


def specs(mode, samples, rounds, linux, thread_counts=None, priorities=None):
    items = []
    qos = ["default"] if linux else ["default", "initiated", "interactive"]
    if priorities:
        qos = priorities
    if mode == "sweep":
        for backend in (["cpu"] if linux else ["metal", "cpu"]):
            for threads in (thread_counts or [1, 2, 4, 6, 8]):
                for priority in qos:
                    items.append(dict(backend=backend, threads=threads, qos=priority))
    elif mode == "idle":
        for backend in (["cpu"] if linux else ["metal", "cpu"]):
            for idle in [600, 2000]:
                for warmup in ["none", "sync", "ahead"]:
                    for priority in (priorities or (["default"] if linux else ["default", "initiated"])):
                        items.append(dict(backend=backend, threads=4, qos=priority,
                                          workload="two", idle_ms=idle, warmup=warmup, lead_ms=100))
    elif mode == "cache":
        for backend in (["cpu"] if linux else ["metal", "cpu"]):
            for cache in ["reuse", "fresh"]:
                for threads in [2, 4, 8]:
                    items.append(dict(backend=backend, threads=threads,
                                      qos="default" if linux else "initiated", cache=cache))
    elif mode == "uclamp":
        for threads in [1, 2, 4, 8]:
            for minimum in [0, 512, 768, 1024]:
                items.append(dict(backend="cpu", threads=threads, qos="default", uclamp=minimum))
    elif mode == "offload":
        for layers in [0, 4, 8, 12, 999]:
            for threads in [2, 4, 8]:
                items.append(dict(backend="cpu" if layers == 0 else "metal", layers=layers,
                                  threads=threads, qos="initiated"))
    result = []
    for round_index in range(rounds):
        for item in items:
            result.append(dict(item, samples=samples, round=round_index))
    random.Random(20260930).shuffle(result)
    return result


def summarize(directory):
    reports = [json.loads(path.read_text()) for path in sorted(directory.glob("result-*.json"))]
    groups = {}
    references = {}
    differences = []
    for report in reports:
        spec = report["spec"]
        for key, ranks in report["signatures"].items():
            identity = (spec["backend"], key)
            if identity not in references:
                references[identity] = ranks
            elif references[identity] != ranks:
                before = [position[0] for position in references[identity]]
                after = [position[0] for position in ranks]
                differences.append(dict(backend=spec["backend"], query=key,
                                        top1_changed=before != after, spec={k: v for k, v in spec.items() if k != "allowed"}))
        settings = {k: v for k, v in spec.items() if k not in ["allowed", "round", "samples"]}
        for row in report["records"]:
            key = json.dumps(dict(settings, workload=row["workload"]), sort_keys=True)
            group = groups.setdefault(key, dict(settings=settings, workload=row["workload"], rows=[]))
            group["rows"].append(row)
    summary = []
    for group in groups.values():
        rows = group.pop("rows")
        values = [row["predict_ms"] for row in rows]
        summary.append(dict(group, n=len(rows), p50_ms=statistics.median(values),
                            p95_ms=percentile(values, .95), p99_ms=percentile(values, .99),
                            mean_ms=statistics.mean(values),
                            ready_p50_ms=statistics.median(row["ready_ms"] for row in rows),
                            compute_p50_ms=statistics.median(row["compute_ms"] for row in rows),
                            create_p50_ms=statistics.median(row["create_ms"] for row in rows)))
    summary.sort(key=lambda row: (row["workload"], row["p50_ms"]))
    outcome = dict(configurations=len(reports), requests=sum(len(r["records"]) for r in reports),
                   ranking_differences=differences,
                   repeated_rank_changes=sum(len(r["repeat_rank_changes"]) for r in reports),
                   repeated_top1_changes=sum(change["top1_changed"] for r in reports for change in r["repeat_rank_changes"]),
                   summary=summary)
    save(directory / "summary.json", outcome)
    print(json.dumps({k: outcome[k] for k in ["configurations", "requests"]}))
    for row in summary:
        print(f"{row['workload']:4s} {str(row['settings']):105s} n={row['n']:4d} "
              f"p50={row['p50_ms']:8.3f} p95={row['p95_ms']:8.3f} ready={row['ready_p50_ms']:8.3f}")
    print(f"ranking differences: {len(differences)}; top1 changes: {sum(r['top1_changed'] for r in differences)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--tables", type=Path, default=ROOT / "ime-core/table")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=["sweep", "idle", "cache", "uclamp", "offload"], default="sweep")
    parser.add_argument("--samples", type=int, default=40)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--cooldown", type=float, default=1)
    parser.add_argument("--threads", help="comma-separated thread counts for the sweep")
    parser.add_argument("--priorities", help="comma-separated QoS labels")
    parser.add_argument("--summarize", action="store_true")
    args = parser.parse_args()
    if args.summarize:
        summarize(args.output)
        return
    if not args.binary or not args.model or args.samples < 1 or args.rounds < 1:
        parser.error("binary, model and positive samples/rounds required")
    args.output.mkdir(parents=True, exist_ok=False)
    table = json.loads((args.tables / "bopomofo_char.json").read_text())
    allowed = {name: [[ord(char) for char in table[reading]] for reading in readings]
               for name, readings in READINGS.items()}
    thread_counts = [int(value) for value in args.threads.split(",")] if args.threads else None
    priorities = args.priorities.split(",") if args.priorities else None
    if thread_counts and any(value < 1 or value > 1024 for value in thread_counts):
        parser.error("threads must be in [1, 1024]")
    if priorities and any(value not in ["default", "initiated", "interactive", "background"] for value in priorities):
        parser.error("unknown priority")
    plan = specs(args.mode, args.samples, args.rounds, platform.system() == "Linux", thread_counts, priorities)
    for spec in plan:
        spec["allowed"] = allowed
    checksum = hashlib.sha256(args.model.read_bytes()).hexdigest()
    # Persist protocol, seed, model hash and ordered configurations before loading.
    source_hashes = {name: hashlib.sha256((Path(__file__).parent / name).read_bytes()).hexdigest()
                     for name in ["bench.cpp", "run.py", "protocol.md", "CMakeLists.txt"]}
    save(args.output / "manifest.json", dict(platform=platform.platform(), model_sha256=checksum,
         binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(), source_sha256=source_hashes,
         environment=environment(),
         binary=str(args.binary.resolve()), seed=20260930, configurations=plan,
         measurement="p50/p95 synchronous Session::predict; warmup and session creation reported separately",
         experimental_scope="No production behavior changes; VM results are not native hardware frequency measurements."))
    for index, spec in enumerate(plan):
        spec_path = args.output / f"spec-{index:03d}.json"
        save(spec_path, spec)
        with (args.output / f"log-{index:03d}.txt").open("w") as log:
            subprocess.run([str(args.binary.resolve()), str(args.model.resolve()), str(args.tables.resolve()),
                            str(spec_path.resolve()), str((args.output / f"result-{index:03d}.json").resolve())],
                           stdout=log, stderr=log, check=True, timeout=300)
        print(f"{index + 1}/{len(plan)} { {k: v for k, v in spec.items() if k != 'allowed'} }", flush=True)
        time.sleep(args.cooldown)
    summarize(args.output)


if __name__ == "__main__":
    main()
