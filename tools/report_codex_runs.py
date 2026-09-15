#!/usr/bin/env python3
"""Summarize completed Codex pilots without turning missing runs into zeroes."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path


def summarize_run(directory):
    run = json.loads((directory/"run.json").read_text())
    resolved = run.get("thread_settings", {})
    row = {"model":run["requested_model"], "effort":run["effort"],
           "status":run["status"], "directory":directory.name, "publishable":False,
           "lanes":run.get("lanes"), "density":run.get("density"),
           "started_utc":run.get("started_utc")}
    if (not run.get("publishable") or run.get("status") != "completed"
            or run.get("verification_passed") is not True):
        row["error"] = run.get("error", "run not eligible for numeric publication")
        return row
    result = json.loads((directory/run["benchmark_result"]).read_text())
    verification = json.loads((directory/"verification.json").read_text())
    if (not result.get("completed") or not result.get("valid") or not verification.get("passed")
            or resolved.get("model") != row["model"] or resolved.get("reasoningEffort") != row["effort"]):
        row["error"] = "completed/valid/replay/model/effort evidence mismatch"
        return row
    bridge = json.loads((directory/"agent/bridge.json").read_text())
    if bridge.get("input_mode") != "keyboard":
        row["error"] = "not the declared keyboard input route"
        return row
    trial = result["trial"]
    hits = trial["perfect"] + trial["good"]
    notes = trial["notes"]
    if notes <= 0:
        row["error"] = "no scored notes"
        return row
    row.update(publishable=True, score_percent=100*trial["score"], notes=notes,
               hits=hits, hit_rate=hits/notes, perfect=trial["perfect"], good=trial["good"],
               empty=trial["empty"], missed=trial["missed"], delaySum_ms=trial["delaySum_ms"],
               hit_mean_ms=trial["hit_delay_sum_ms"]/hits if hits else None,
               observation_to_tool_ms=run["observation_to_tool_ms"],
               tool_calls=len(run["model_decisions"]),
               actions_proposed=bridge["counts"].get("action_proposed",0),
               actions_sent=bridge["counts"].get("action_sent",0),
               actions_dropped=bridge["counts"].get("action_dropped",0),
               engine_timing_compliant=result["observation"]["timing_compliant"],
               engine_max_publication_gap_ms=result["observation"]["publication_gaps"]["max_ms"],
               receiver_lifetime_max_gap_ms=bridge["receipt_gap_max_ms"],
               receiver_lifetime_gaps_over_16ms=bridge["receipt_gaps_over_16ms"],
               service_tier=resolved.get("serviceTier"), token_usage=run.get("token_usage"))
    handoffs = [e["local_return_ns"] for e in bridge.get("events",[]) if e["type"] == "observation"]
    gaps = [(b-a)/1e6 for a,b in zip(handoffs,handoffs[1:])]
    row["tool_image_handoff_gaps_ms"] = {"count":len(gaps), "max":max(gaps) if gaps else None,
        "mean":sum(gaps)/len(gaps) if gaps else None, "over_16ms":sum(g > 16 for g in gaps)}
    action_latencies = [d["observation_to_tool_ms"] for d in run["model_decisions"]
                        if isinstance(d.get("arguments"),dict) and d["arguments"].get("actions")
                        and "observation_to_tool_ms" in d]
    row["action_batch_observation_to_tool_ms"] = {"count":len(action_latencies),
        "mean":sum(action_latencies)/len(action_latencies) if action_latencies else None}
    return row


def _start_time(row):
    try:
        value = datetime.fromisoformat(row["started_utc"].replace("Z", "+00:00"))
        return value.astimezone(timezone.utc) if value.tzinfo is not None else None
    except (AttributeError, KeyError, TypeError, ValueError):
        return None


def _configuration_key(row, catalog):
    models = catalog["models"]
    positions = {model["model"]: index for index, model in enumerate(models)}
    model = next((item for item in models if item["model"] == row["model"]), {})
    efforts = [item["reasoningEffort"] for item in model.get("supportedReasoningEfforts", [])]
    maximum = efforts[-1] if efforts else None
    order = 0 if row["effort"] == "low" else 1 if row["effort"] == maximum else 2
    return positions.get(row["model"], len(models)), row["model"], order, row["effort"]


def select_runs(rows, catalog):
    """Choose by start time and eligibility, never by achieved score or latency."""
    selected = {}
    chronology = sorted(rows, key=lambda row: (
        _start_time(row) or datetime.max.replace(tzinfo=timezone.utc), row["directory"]))
    attempts = []
    for row in chronology:
        key = row["model"], row["effort"]
        valid_time = _start_time(row) is not None
        eligible = row["publishable"] and valid_time
        chosen = eligible and key not in selected
        if chosen:
            selected[key] = row
        reason = ("first eligible run by started_utc" if chosen else
                  row.get("error", "not eligible for numeric publication") if not row["publishable"] else
                  "missing or invalid timezone-qualified started_utc" if not valid_time else
                  "a chronologically earlier eligible run was selected")
        attempts.append({"model":row["model"], "effort":row["effort"],
                         "directory":row["directory"], "started_utc":row.get("started_utc"),
                         "eligible":eligible, "selected":chosen, "reason":reason})
    chosen_rows = sorted(selected.values(), key=lambda row: _configuration_key(row, catalog))
    expected = []
    for model in catalog["models"]:
        efforts = [item["reasoningEffort"] for item in model.get("supportedReasoningEfforts", [])]
        requested = (["low"] if "low" in efforts else []) + (efforts[-1:] if efforts else [])
        for effort in dict.fromkeys(requested):
            if (model["model"], effort) not in selected:
                expected.append({"model":model["model"], "effort":effort})
    return {
        "criterion": "For each (model, effort), select the first numeric-eligible run by started_utc; "
                     "break equal timestamps by relative directory path. A valid timezone-qualified "
                     "timestamp is required. Scores and latency do not affect selection.",
        "display_order": "Catalog model order; low then the catalog's maximum supported effort, then other efforts.",
        "catalog_observed_utc": catalog.get("observed_utc"),
        "selected": chosen_rows, "attempts": attempts, "missing_configurations": expected,
    }


def markdown_table(rows, link_prefix=""):
    lines = ["| Model | Effort | Score | Hits / notes | delaySum (ms) | Hit mean (ms) | Obs→tool mean (s) |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        label = f"[{row['model']}]({link_prefix}{row['directory']}/run.json)"
        if not row["publishable"]:
            status = row['status'] if row['status'] != 'completed' else 'unverified'
            lines.append(f"| {label} | {row['effort']} | {status} | — | — | — | — |")
            continue
        mean = "N/A" if row["hit_mean_ms"] is None else f"{row['hit_mean_ms']:.3f}"
        latency = row["observation_to_tool_ms"]["mean"]
        latency_text = "N/A" if latency is None else f"{latency/1000:.3f}"
        lines.append(f"| {label} | {row['effort']} | {row['score_percent']:.3f}% | {row['hits']} / {row['notes']} | {row['delaySum_ms']:.3f} | {mean} | {latency_text} |")
    return "\n".join(lines)


def generate_report(directory):
    directory = Path(directory)
    rows = []
    for path in sorted(directory.rglob("run.json")):
        row = summarize_run(path.parent)
        row["directory"] = str(path.parent.relative_to(directory))
        rows.append(row)
    catalog = json.loads((directory/"model-catalog.json").read_text())
    rows.sort(key=lambda row: (_configuration_key(row, catalog),
                              _start_time(row) or datetime.max.replace(tzinfo=timezone.utc), row["directory"]))
    selection = select_runs(rows, catalog)
    record = {"catalog_observed_utc":catalog["observed_utc"], "client_version":catalog["client_version"],
              "scope":"models visible through the evaluated account's Codex catalog",
              "runs":rows}
    (directory/"summary.json").write_text(json.dumps(record,indent=2)+"\n")
    (directory/"selection.json").write_text(json.dumps(selection,indent=2)+"\n")
    workloads = sorted({(str(row['lanes']),str(row['density'])) for row in rows})
    workload_text = "; ".join(f"{lanes} lanes, {density} total attempts/s" for lanes,density in workloads)
    text = f"# CHERS native Codex vision pilots\n\nFresh-chart pilot attempts. Workloads: {workload_text}.\n\n"
    text += "## Selected results\n\n" + selection["criterion"] + " " + selection["display_order"] + "\n\n"
    text += "The full selection record is in [selection.json](selection.json).\n\n"
    text += markdown_table(selection["selected"])
    if selection["missing_configurations"]:
        text += "\n\nNo eligible result: " + "; ".join(
            f"{item['model']} ({item['effort']})" for item in selection["missing_configurations"]) + "."
    text += "\n\n## All attempted runs\n\nFailed attempts and later eligible runs remain listed below.\n\n"
    text += markdown_table(rows)
    errors = [row for row in rows if not row["publishable"]]
    if errors:
        text += "\n\n### Excluded attempts\n\n"
        for row in errors:
            error = " ".join(str(row.get("error", "not eligible")).splitlines())
            text += f"- [{row['directory']}]({row['directory']}/run.json): {error}\n"
    text += "\n\n## Interpretation\n\nZero delaySum with zero hits is not accurate timing; the hit mean is N/A.\n"
    text += ("Obs→tool measures local image receipt to the returned tool call and includes runtime, "
             "transport, model processing, and tool-loop overhead; it is not pure inference time. "
             "It covers answered observations only. Capture rate is not inference rate. "
             "The summary's tool_calls count includes observation-only and invalid calls, not only action decisions.\n")
    text += "See each run's agent/bridge.json, verification.json and benchmark result for actions and timing.\n"
    (directory/"REPORT.md").write_text(text)
    return record, selection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    _, selection = generate_report(args.directory)
    print(markdown_table(selection["selected"]))


if __name__ == "__main__":
    main()
