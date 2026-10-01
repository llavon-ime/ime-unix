#!/usr/bin/env python3
"""Replay a frozen stress set and measure actual first-page reachability.

Candidate presence is a repair affordance, not automatic accuracy. An inspected
fixture stays development/validation data; this tool never labels it held out.
"""

import argparse
import hashlib
import json
import subprocess
from collections import defaultdict
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    fixture = json.loads(args.fixture.read_text())
    rows = []
    for row in fixture["cases"]:
        result = json.loads(subprocess.check_output(
            [str(args.probe), row["layout"], row["raw"], "--json"], text=True, timeout=10))
        rows.append({**row, **result})
    groups = defaultdict(list)
    for row in rows:
        groups[(row["layout"], row["intent"])].append(row)
    summaries = []
    for (layout, intent), group in sorted(groups.items()):
        summary = {"layout": layout, "intent": intent, "count": len(group),
                   "automatic_exact": sum(r["best"] == r["expected"] for r in group),
                   "first_page": sum(r["expected"] in r["first_page"] for r in group),
                   "retained": sum(r["expected"] in r["paths"] for r in group)}
        summaries.append(summary)
        print(json.dumps(summary, ensure_ascii=False))
    report = {"fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest(),
              "binary_sha256": hashlib.sha256(args.probe.read_bytes()).hexdigest(),
              "policy": "conservative-structured-score-explicit-selection", "summaries": summaries, "rows": rows}
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
