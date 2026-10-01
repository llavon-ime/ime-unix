#!/usr/bin/env python3
"""Observe a frozen, target-independent candidate insertion policy on development data."""

import argparse
import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def save(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def load_cases(path, sources, depth=0):
    if depth >= 16:
        raise ValueError("cyclic or too-deep fixture inheritance")
    sources[str(path.resolve())] = digest(path)
    local = json.loads(path.read_text())
    if "extends" not in local:
        return local
    inherited = load_cases(path.parent / local.pop("extends"), sources, depth + 1)
    for key, value in local.items():
        if key in ("articles", "edit_workflows", "timing_cases"):
            inherited.setdefault(key, []).extend(value)
        else:
            inherited[key] = value
    return inherited


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("probe", "service", "model", "cases", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    sources = {}
    cases = load_cases(args.cases, sources)
    frozen = args.output / "frozen-cases.json"
    save(frozen, cases)
    save(args.output / "manifest.json", {
        "frozen_at_utc": datetime.now(timezone.utc).isoformat(),
        "policy": "shadow-pause-four-append-originals-v1",
        "data_role": "previously observed development/regression data, not holdout",
        "articles": len(cases["articles"]),
        "clauses": sum(len(article["clauses"]) for article in cases["articles"]),
        "fixture_sha256": digest(frozen), "source_fixtures_sha256": sources,
        "binary_sha256": {str(path.resolve()): digest(path) for path in (args.probe, args.service, args.model)},
        "acceptance": ["raw in homepage", "no preview change", "no formerly reachable target lost",
                       "commit equals selected text", "no transport or model failures"],
    })
    report_path = args.output / "observations.json"
    subprocess.run([str(path.resolve()) for path in (args.probe, args.service, args.model, frozen, report_path)]
                   + ["0", "pause-candidates"], check=True)
    report = json.loads(report_path.read_text())
    rows = report["clause_recovery"]
    if len(rows) != sum(len(article["clauses"]) for article in cases["articles"]) * 2:
        raise ValueError("missing clause observations")
    summary = {}
    for layout in ("standard", "hsu", "all"):
        selected = [row for row in rows if layout == "all" or row["layout"] == layout]
        old = lambda row: row["baseline_target_in_homepage"]
        new = lambda row: row["enriched_target_in_homepage"]
        summary[layout] = {
            "total": len(selected), "applicable": sum(row["applicable"] for row in selected),
            "baseline_reachable": sum(old(row) for row in selected),
            "enriched_reachable": sum(new(row) for row in selected),
            "gains": sum(not old(row) and new(row) for row in selected),
            "losses": sum(old(row) and not new(row) for row in selected),
            "extra_requests": sum(row["extra_requests"] for row in selected),
            "added_paths": sum(row["added_paths"] for row in selected),
            "model_failures": sum(row["model_failures"] for row in selected),
            "preview_changes": sum(not row["preview_preserved"] for row in selected),
            "raw_missing": sum(not row["raw_in_homepage"] for row in selected),
            "commit_mismatches": sum(not row["commit_matches_selected"] for row in selected),
            "loss_examples": [row["id"] for row in selected if old(row) and not new(row)],
            "gain_examples": [row["id"] for row in selected if not old(row) and new(row)],
        }
    all_rows = summary["all"]
    accepted = not any(all_rows[key] for key in ("losses", "model_failures", "preview_changes", "raw_missing", "commit_mismatches"))
    save(args.output / "decision.json", {
        "acceptance_passed": accepted,
        "decision": "eligible for a new independent validation, not production approval" if accepted else
                    "reject this insertion policy; do not tune on these observations",
        "summary": summary,
        "interpretation": "settled counterfactual homepage repair, not automatic accuracy or human selection success",
    })
    print(json.dumps({"acceptance_passed": accepted, "summary": summary}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
