#!/usr/bin/env python3
"""Render request-scoped boost evidence, including independent round medians.

Per-round medians are reported rather than an IID confidence interval that
would ignore shared process/temperature effects between neighboring bursts.
"""
import argparse
import json
import math
import statistics
from pathlib import Path


def p95(values):
    return sorted(values)[math.ceil(len(values) * .95) - 1]


def analyze(directory):
    by_round = {}
    reports = [json.loads(p.read_text()) for p in sorted(directory.glob("result-*.json"))]
    for report in reports:
        spec = report["spec"]
        key = (spec["backend"], spec["threads"], spec["idle_ms"], spec["mode"], spec["round"])
        records = report["records"]
        for record in records:
            assert record["workers"]["errors"] == 0
            if spec["backend"] == "cpu":
                assert record["workers"]["created"] > 0
            if spec["mode"] in ["workers", "static", "interactive"]:
                observed = "interactive" if spec["mode"] == "interactive" else "initiated"
                assert record["workers"][observed] == record["workers"]["created"]
                assert record["workers"]["restored"] == record["workers"]["created"]
            expected_idle = 25 if spec["mode"] == "static" else 21
            assert record["idle_qos_after"] == expected_idle
        first = [r["first_including_setup_ms"] for r in records]
        by_round[key] = dict(n=len(first), first_p50_ms=statistics.median(first),
                             first_p95_ms=p95(first), total_p50_ms=statistics.median(r["total_ms"] for r in records))
    summary = json.loads((directory / "summary.json").read_text())
    baseline = {(r["backend"], r["threads"], r["idle_ms"]):r for r in summary["summary"] if r["mode"] == "none"}
    rows = []
    for row in summary["summary"]:
        key = (row["backend"], row["threads"], row["idle_ms"])
        reference = baseline.get(key)
        repeats = [{"round":identity[4], **values} for identity, values in sorted(by_round.items()) if identity[:4] == (*key, row["mode"])]
        comparisons = []
        for repeat in repeats:
            matched = by_round.get((*key, "none", repeat["round"]))
            if matched:
                comparisons.append(dict(round=repeat["round"],
                    first_p50_improvement_pct=100*(1-repeat["first_p50_ms"]/matched["first_p50_ms"])))
        rows.append(dict(row, rounds=repeats, matched_round_comparisons=comparisons,
            first_p50_improvement_pct=100*(1-row["first_p50_ms"]/reference["first_p50_ms"]) if reference else None,
            energy_change_pct=100*(row["energy_nj_p50"]/reference["energy_nj_p50"]-1) if reference and reference["energy_nj_p50"] else None))
    result = dict(configurations=len(reports), bursts=sum(len(r["records"]) for r in reports),
                  predictions=summary["requests"], worker_restoration_verified=True, summary=rows)
    (directory / "analysis.json").write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n")
    lines = [f"# Scoped burst QoS: {directory.name}", "",
             f"{result['configurations']} configurations, {result['bursts']} bursts, {result['predictions']} predictions.", "",
             "Positive improvement means faster than the same-backend/thread/idle baseline.", "",
             "Energy is OS process-attributed accounting, not GPU or whole-system rail measurement.", "",
             "| Backend / threads | Idle ms | Mode | n | First p50 / p95 ms | Following p50 ms | Total p50 ms | Setup / restore us | P-core CPU % | Process energy mJ | First improvement % |",
             "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        performance = row["performance_cpu_fraction"]
        energy = row["energy_nj_p50"]
        lines.append(f"| {row['backend']} / {row['threads']} | {row['idle_ms']} | {row['mode']} | {row['n']} | "
                     f"{row['first_p50_ms']:.3f} / {row['first_p95_ms']:.3f} | {row['following_p50_ms']:.3f} | {row['total_p50_ms']:.3f} | "
                     f"{row['setup_p50_ms']*1000:.1f} / {row['restore_p50_ms']*1000:.1f} | "
                     f"{performance*100 if performance is not None else float('nan'):.1f} | "
                     f"{energy/1e6 if energy is not None else float('nan'):.1f} | {row['first_p50_improvement_pct'] or 0:.1f} |")
    lines.extend(["", "## Round comparisons", "", "Positive values mean first-request median improvement relative to that round's control.", ""])
    for row in rows:
        if row["mode"] == "none":
            continue
        comparisons = row["matched_round_comparisons"]
        lines.append(f"- {row['backend']}/{row['threads']} idle={row['idle_ms']} {row['mode']}: " +
                     ", ".join(f"round {r['round']}: {r['first_p50_improvement_pct']:+.1f}%" for r in comparisons))
    (directory / "analysis.md").write_text("\n".join(lines)+"\n")
    print(result["configurations"], "configurations;", result["bursts"], "bursts;", result["predictions"], "predictions")
    print("QoS transition, compute worker targeting and restoration checks verified for every observation.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    for directory in parser.parse_args().directories:
        analyze(directory)
