#!/usr/bin/env python3
"""Freeze and observe model/service capability cases; no quality tuning or training.

The token trace mirrors the read-only core tokenizer for this experiment only.
It must never become a production tokenizer or a semantic cache key.
"""

import argparse
import hashlib
import json
import statistics
import subprocess
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def save(path, value):
    # Exclusive creation preserves the pre-observation fixture and evidence.
    with path.open("x") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


class TokenAudit:
    def __init__(self, tables):
        self.tables = {
            name: json.loads((tables / f"{name}.json").read_text())
            for name in ("chars", "latin", "bpmf", "special_tokens")
        }

    @staticmethod
    def latin(character):
        return character.isascii() and (character.isalnum() or character in "-_+")

    def trace(self, query):
        chars = self.tables["chars"]
        latin = self.tables["latin"]
        special = self.tables["special_tokens"]
        context = query["context"]
        context_tokens = []
        labels = []
        index = 0
        while index < len(context):
            character = context[index]
            if character == " ":
                token, label = special["<SP>"], "<SP>"
            elif character in chars:
                token, label = chars[character], character
            elif self.latin(character):
                end = index + 1
                while end < len(context) and self.latin(context[end]):
                    end += 1
                word = context[index:end].lower()
                token = latin.get(word, special["<LATIN>"])
                label = f"latin:{word}" if word in latin else "<LATIN>"
                index = end - 1
            else:
                token, label = special["<UNK>"], "<UNK>"
            context_tokens.append(token)
            labels.append(label)
            index += 1
        while context_tokens and context_tokens[0] == special["<UNK>"]:
            context_tokens.pop(0)
            labels.pop(0)
        tokens = [special["<BOS>"]] + context_tokens
        labels.insert(0, "<BOS>")
        chosen_decode = []
        for entry in query["padding"]:
            if "chosen" in entry:
                character = entry["chosen"]
                token = chars.get(character, special["<UNK>"])
                labels.append(character if character in chars else "<UNK>")
                chosen_decode.append(chars.get(character))
            else:
                reading = f"<{entry['reading']}>"
                token = self.tables["bpmf"][reading]
                labels.append(reading)
                chosen_decode.append("predict")
            tokens.append(token)
        tokens.append(special["<SEP>"])
        labels.append("<SEP>")
        return {"prompt_tokens": tokens, "labels": labels, "chosen_decode": chosen_decode}


def fixtures():
    readings = [{"reading": "ㄒㄧㄣ "}, {"reading": "ㄒㄧㄤ "}]
    groups = [
        ("opaque_identifier", "請檢查quasar_cache_73", "請檢查mistral_cache_91", None, True),
        ("opaque_email", "請寄到quasar@docs.invalid", "請寄到mistral@docs.invalid", None, True),
        ("opaque_numeric", "版本481762", "版本938541", None, True),
        ("case_fold", "請使用Python", "請使用pYtHoN", None, True),
        ("unknown_symbol", "我看到🚀", "我看到🧭", None, True),
        ("leading_unknown", "🚀系統", "🧭系統", None, True),
        ("opaque_hyphen", "請檢查quasar-cache", "請檢查mistral-cache", None, True),
        ("known_latin_control", "請使用python", "請使用java", None, False),
        ("chinese_context_control", "郵件的", "建築的", None, False),
        ("chosen_ascii", "這是", "這是", ("a", "z"), True),
        ("chosen_digit", "這是", "這是", ("1", "9"), True),
        ("chosen_hanzi_control", "這是", "這是", ("信", "心"), False),
        ("chosen_punctuation_control", "這是", "這是", (",", "."), False),
    ]
    queries = []
    pairs = []
    for group, left, right, chosen, equivalent in groups:
        for variant, context in enumerate((left, right)):
            padding = ([] if chosen is None else [{"chosen": chosen[variant]}]) + readings
            queries.append({"id": f"{group}:{variant}", "group": group, "context": context, "padding": padding})
        pairs.append({"group": group, "equivalent_expected": equivalent})
    return {"queries": queries, "pairs": pairs}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("service", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path, help="new output directory; never overwrite an observed run")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    tables = ROOT / "ime-core/table/tokens"
    audit = TokenAudit(tables)
    inputs = fixtures()
    traces = {query["id"]: audit.trace(query) for query in inputs["queries"]}
    for pair in inputs["pairs"]:
        group = pair["group"]
        left, right = (traces[f"{group}:{index}"] for index in (0, 1))
        same = left["prompt_tokens"] == right["prompt_tokens"] and left["chosen_decode"] == right["chosen_decode"]
        if same != pair["equivalent_expected"]:
            raise ValueError(f"predeclared token capability changed: {group}")
    inputs["acceptance"] = {
        "equal_tokens_equal_unresolved_candidate_lists": True,
        "chosen_singletons_and_same_reading_only": True,
        "request_history_must_not_change_candidate_lists": True,
        "different_token_controls_need_not_change_top1": True,
        "no_accuracy_or_population_estimate": True,
    }
    fixture_path = args.output / "frozen-queries.json"
    save(fixture_path, inputs)
    provenance_paths = [args.probe, args.service, args.model, Path(__file__),
                        ROOT / "engine/tools/model_mixed_probe/service_capability_probe.cpp",
                        ROOT / "ime-core/src/engine/tokenizer.hpp",
                        ROOT / "ime-core/src/engine/llama_engine.hpp"]
    provenance_paths += [tables / f"{name}.json" for name in audit.tables]
    save(args.output / "manifest.json", {
        "frozen_at_utc": datetime.now(timezone.utc).isoformat(),
        "fixture_sha256": digest(fixture_path),
        "core_commit": subprocess.check_output(["git", "-C", str(ROOT / "ime-core"), "rev-parse", "HEAD"], text=True).strip(),
        "sha256": {str(path): digest(path) for path in provenance_paths},
        "token_traces": traces,
        "chars": len(audit.tables["chars"]), "latin": len(audit.tables["latin"]),
        "ascii_chosen_supported": "".join(chr(value) for value in range(32, 127) if chr(value) in audit.tables["chars"]),
    })
    # Only after all queries, hypotheses, traces and hashes are on disk do we
    # load the model. The capability cases are not an unseen language holdout.
    report_path = args.output / "service-observations.json"
    subprocess.run([str(args.probe.resolve()), str(args.service.resolve()), str(args.model.resolve()),
                    str(fixture_path.resolve()), str(report_path.resolve())], check=True)
    report = json.loads(report_path.read_text())
    rows = {(row["phase"], row["id"]): row for row in report["rows"]}
    phases = ("reused_forward", "reused_reverse", "fresh_forward")
    if len(rows) != len(inputs["queries"]) * len(phases):
        raise ValueError("missing or duplicate observations")
    summary = []
    for pair in inputs["pairs"]:
        group = pair["group"]
        equal = []
        examples = []
        for phase in phases:
            left, right = (rows[phase, f"{group}:{index}"] for index in (0, 1))
            equal.append(left["unresolved_candidates"] == right["unresolved_candidates"])
            examples.append({"phase": phase, "left": left["text"], "right": right["text"],
                             "unresolved_lists_equal": equal[-1]})
        summary.append({**pair, "unresolved_lists_equal": equal, "examples": examples})
    stable = all(rows[phases[0], query["id"]]["candidates"] == rows[phase, query["id"]]["candidates"]
                 for query in inputs["queries"] for phase in phases[1:])
    equivalent = [row for row in summary if row["equivalent_expected"]]
    passed = stable and all(all(row["unresolved_lists_equal"]) for row in equivalent)
    save(args.output / "capability-report.json", {
        "acceptance_passed": passed, "query_count": len(inputs["queries"]),
        "prediction_observations": len(report["rows"]),
        "equivalent_pairs": len(equivalent),
        "equivalent_pair_observations_equal": sum(sum(row["unresolved_lists_equal"]) for row in equivalent),
        "request_history_stable": stable,
        "warm_prediction_median_us": statistics.median(row["latency_us"] for row in report["rows"]),
        "pairs": summary,
        "interpretation": "API/token capability evidence; not quality accuracy, confidence calibration or desktop latency",
    })
    if not passed:
        raise RuntimeError("capability acceptance failed; evidence retained for diagnosis")
    print(f"Capability invariants passed: {len(inputs['queries'])} queries, {len(equivalent)} equivalent pairs, three phases")


if __name__ == "__main__":
    main()
