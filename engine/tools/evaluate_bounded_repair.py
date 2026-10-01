#!/usr/bin/env python3
"""Freeze a bounded explicit-repair policy and evaluate new synthetic negative controls.

The challenge uses related generators to earlier experiments: it is not an
independent human-language holdout. No model output is consulted while making it.
"""

import argparse
import json
import math
import random
import statistics
import string
import subprocess
from datetime import datetime, timezone
from pathlib import Path

from evaluate_pause_candidates import digest, load_cases, save


ROOT = Path(__file__).resolve().parents[2]
SEED = 2026093003


def challenge():
    rng = random.Random(SEED)
    families = {
        "technical": ["nuitka", "wasmtime", "openbao", "cerbos", "zellij", "komorebi", "sokol", "ivorysql"],
        "opaque_alpha": ["".join(rng.choices(string.ascii_lowercase, k=21)) for _ in range(8)],
        "opaque_hex": ["b" + "".join(rng.choices("0123456789abcdef", k=17)) for _ in range(8)],
    }
    prefixes = [("設定", "ㄕㄜˋ|ㄉㄧㄥˋ"), ("請看", "ㄑㄧㄥˇ|ㄎㄢˋ"), ("今天", "ㄐㄧㄣ|ㄊㄧㄢ")]
    articles = []
    for family, roots in families.items():
        for root in roots:
            for shape, literal in [("identifier", root + "_store84"), ("email", root + "@mail.invalid"),
                                   ("domain", root + ".docs.invalid")]:
                for prefix_index, (text, readings) in enumerate(prefixes):
                    # Opposing intentions share the same raw, context and root
                    # group. Only the explicit-refresh event and desired label
                    # differ; neither label is an input to the model policy.
                    for ascii_intent in (False, True):
                        articles.append({
                            "id": f"{family}:{root}:{shape}:{prefix_index}:{'ascii' if ascii_intent else 'mixed'}",
                            "root_group": f"{family}:{root}", "category": family,
                            "ascii_intent": ascii_intent, "explicit_request": not ascii_intent,
                            "clauses": [{"parts": [{"text": text, "readings": readings}, {"text": literal}],
                                         "punctuation": "。"}],
                        })
    return {"articles": articles}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("probe", "service", "model", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--development", type=Path, help="use already-observed articles instead of the new challenge")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    policy = {
        "id": "explicit-first-visible-one-run-v1", "max_predicts_per_event": 1, "retries": 0,
        "actor": "explicit refresh of first visible non-raw char_index=0 path with supported complete readings",
        "scope": "first contiguous supported Bopomofo run only; no target text available",
        "insertion": "one novel same-structure path immediately after source; preserve all original paths and preview",
        "acceptance": ["no formerly reachable full target lost", "no preview change", "raw remains reachable",
                       "commit matches selected document", "no model failure", "event budget <= 1", "no event => zero extra requests"],
    }
    # Freeze policy/binary/source hashes before generating challenge inputs.
    paths = [args.probe, args.service, args.model, Path(__file__),
             ROOT / "engine/tests/rawkey/typing_usability_probe.cpp",
             ROOT / "docs/bounded-explicit-repair-experiment.md"]
    save(args.output / "frozen-policy.json", {
        "frozen_at_utc": datetime.now(timezone.utc).isoformat(), "policy": policy,
        "sha256": {str(path.resolve()): digest(path) for path in paths},
    })
    sources = {}
    cases = load_cases(args.development, sources) if args.development else challenge()
    fixture = args.output / "frozen-cases.json"
    save(fixture, cases)
    save(args.output / "manifest.json", {
        "fixture_sha256": digest(fixture), "seed": None if args.development else SEED,
        "source_fixtures_sha256": sources,
        "data_role": "previously observed development/regression" if args.development else
                     "new frozen synthetic challenge; related generator; not independent natural-language holdout",
        "root_groups": len({article.get("root_group", article["id"]) for article in cases["articles"]}),
        "articles": len(cases["articles"]),
        "clauses": sum(len(article["clauses"]) for article in cases["articles"]),
    })
    observations = args.output / "observations.json"
    subprocess.run([str(path.resolve()) for path in (args.probe, args.service, args.model, fixture, observations)]
                   + ["0", "explicit-one"], check=True)
    rows = json.loads(observations.read_text())["clause_recovery"]
    if len(rows) != sum(len(article["clauses"]) for article in cases["articles"]) * 2:
        raise ValueError("incomplete observations")
    summary = {}
    groups = {"all": rows}
    groups.update({layout: [row for row in rows if row["layout"] == layout] for layout in ("standard", "hsu")})
    groups.update({"family:" + family: [row for row in rows if row["category"] == family]
                   for family in sorted({row["category"] for row in rows})})
    groups["no_explicit_request"] = [row for row in rows if not row["explicit_request"]]
    groups["explicit_request"] = [row for row in rows if row["explicit_request"]]
    for group, selected in groups.items():
        active = [row for row in selected if row["extra_requests"] != 0]
        waits = sorted(row["wait_us"] / 1000 for row in active)
        summary[group] = {
            "total": len(selected), "baseline_reachable": sum(row["baseline_target_in_homepage"] for row in selected),
            "enriched_reachable": sum(row["enriched_target_in_homepage"] for row in selected),
            "gains": sum(not row["baseline_target_in_homepage"] and row["enriched_target_in_homepage"] for row in selected),
            "losses": sum(row["baseline_target_in_homepage"] and not row["enriched_target_in_homepage"] for row in selected),
            "root_groups": len({row["root_group"] for row in selected}),
            "gain_root_groups": sorted({row["root_group"] for row in selected if
                                        not row["baseline_target_in_homepage"] and row["enriched_target_in_homepage"]}),
            "automatic_preview_exact": sum(row["automatic_preview_exact"] for row in selected),
            "extra_requests": sum(row["extra_requests"] for row in selected),
            "max_extra_requests": max((row["extra_requests"] for row in selected), default=0),
            "without_new_suggestion": sum(row["added_paths"] == 0 for row in selected),
            "added_paths": sum(row["added_paths"] for row in selected),
            "wait_median_ms": statistics.median(waits) if waits else None,
            "wait_p95_ms": waits[math.ceil(len(waits) * .95) - 1] if waits else None,
            "model_failures": sum(row["model_failures"] for row in selected),
            "preview_changes": sum(not row["preview_preserved"] for row in selected),
            "raw_missing": sum(not row["raw_in_homepage"] for row in selected),
            "commit_mismatches": sum(not row["commit_matches_selected"] for row in selected),
            "budget_violations": sum(row["extra_requests"] > 1 or
                                     (not row["explicit_request"] and row["extra_requests"] != 0) for row in selected),
        }
    totals = summary["all"]
    accepted = not any(totals[key] for key in ("losses", "model_failures", "preview_changes", "raw_missing", "commit_mismatches", "budget_violations"))
    # For new challenge data, explicitly check paired intentions did share raw
    # and context. The label must not affect the unmodified production preview.
    pair_failures = []
    if not args.development:
        paired = {}
        for row in rows:
            key = (row["layout"], row["id"].replace(":ascii:", ":intent:").replace(":mixed:", ":intent:"))
            paired.setdefault(key, []).append(row)
        for key, pair in paired.items():
            if len(pair) != 2 or any(pair[0][field] != pair[1][field] for field in ("raw", "context", "before")):
                pair_failures.append(key)
        accepted = accepted and not pair_failures
    save(args.output / "decision.json", {
        "acceptance_passed": accepted,
        "decision": "retain as research-only; evidence of bounded behavior, not production approval" if accepted else
                    "reject this frozen policy; do not tune on observed challenge",
        "paired_intent_mismatches": pair_failures, "summary": summary,
        "interpretation": "fixed simulated actor and settled candidate repair, not human usability or automatic conversion gains",
    })
    print(json.dumps({"acceptance_passed": accepted, "all": totals,
                      "no_explicit_request": summary["no_explicit_request"]}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
