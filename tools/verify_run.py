#!/usr/bin/env python3
"""Independently replay CHERS's persisted benchmark evidence.

No engine code is imported. Inputs are matched to the earliest still-available
note in their lane; their recorded note IDs, judgments, and delays are claims to
check, never replay instructions. A timing failure is separate from an invalid
score, unless --require-timing is requested.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import re
import sys
from typing import Any

MS = 1_000_000
SECOND = 1_000_000_000
PERFECT = 8 * MS
GOOD = 20 * MS
LOCK = 40 * MS
EMPTY_DELAY = 100 * MS
TIMELINE = {
    "calibration_start": SECOND,
    "calibration_end": 6 * SECOND,
    "scored_start": 11_020 * MS,
    "scored_end": 61_020 * MS,
    "finish": 61_040 * MS,
}
TARGET_WINDOWS = {
    "calibration": (TIMELINE["calibration_start"], TIMELINE["calibration_end"]),
    "scored": (TIMELINE["scored_start"], TIMELINE["scored_end"]),
}
NOTE_FIELDS = ["id", "lane", "target_ns", "stage"]
INPUT_FIELDS = ["event_ns", "lane", "note_id", "stage", "judgment", "delay_ns"]
TIMING_FIELDS = ["kind", "index", "duration_ns"]
INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)\Z")


class EvidenceError(ValueError):
    """Malformed evidence prevents further safe replay."""


def _object_no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError(f"result.json repeats key {key!r}")
        result[key] = value
    return result


def _nonfinite_json(value: str) -> None:
    raise EvidenceError(f"result.json contains non-finite number {value}")


def _integer(value: Any, label: str, minimum: int = 0, maximum: int = 2**63 - 1) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise EvidenceError(f"{label} must be an integer in [{minimum}, {maximum}]")
    return value


def _csv_integer(value: Any, label: str, minimum: int = 0, maximum: int = 2**63 - 1) -> int:
    if not isinstance(value, str) or INTEGER.fullmatch(value) is None:
        raise EvidenceError(f"{label} is not a canonical integer")
    return _integer(int(value), label, minimum, maximum)


def _number(value: Any, label: str) -> float:
    if type(value) not in (int, float):
        raise EvidenceError(f"{label} must be a finite number")
    try:
        converted = float(value)
    except OverflowError as error:
        raise EvidenceError(f"{label} is out of numeric range") from error
    if not math.isfinite(converted):
        raise EvidenceError(f"{label} must be a finite number")
    return converted


def _boolean(value: Any, label: str) -> bool:
    if type(value) is not bool:
        raise EvidenceError(f"{label} must be a JSON boolean")
    return value


def _mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise EvidenceError(f"{label} must be an object")
    return value


def _rows(path: Path, fields: list[str]) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != fields:
            raise EvidenceError(f"{path.name} header must be {','.join(fields)}")
        rows = list(reader)
        if any(None in row or any(value is None for value in row.values()) for row in rows):
            raise EvidenceError(f"{path.name} has a row with the wrong field count")
        return rows


def _stage_at(event_ns: int) -> str | None:
    for stage, (start, end) in TARGET_WINDOWS.items():
        if start - GOOD <= event_ns < end + GOOD:
            return stage
    return None


def _close(claimed: Any, expected: float, label: str, errors: list[str]) -> None:
    value = _number(claimed, label)
    # save_results writes 12 significant digits; CSV timing remains exact ns.
    if not math.isclose(value, expected, rel_tol=5e-12, abs_tol=5e-10):
        errors.append(f"{label}: claimed {claimed}, replay requires {expected}")


def _distribution(value: Any, label: str, errors: list[str]) -> dict[str, Any]:
    data = _mapping(value, label)
    count = _integer(data["count"], f"{label}.count")
    over16 = _integer(data["over_16ms"], f"{label}.over_16ms", maximum=count)
    over2 = _integer(data["over_2ms"], f"{label}.over_2ms", maximum=count)
    numbers = {key: _number(data[key], f"{label}.{key}")
               for key in ("mean_ms", "p50_ms", "p99_ms", "max_ms")}
    if any(number < 0 for number in numbers.values()):
        errors.append(f"{label}: timing durations cannot be negative")
    if not 0 <= over16 <= over2 <= count:
        errors.append(f"{label}: threshold counts are inconsistent")
    if not numbers["p50_ms"] <= numbers["p99_ms"] <= numbers["max_ms"]:
        errors.append(f"{label}: percentile ordering is inconsistent")
    if numbers["mean_ms"] > numbers["max_ms"]:
        errors.append(f"{label}: mean exceeds maximum")
    if count == 0 and any(numbers.values()):
        errors.append(f"{label}: empty timing series must have zero summaries")
    for threshold, exceedances in ((16, over16), (2, over2)):
        if numbers["max_ms"] > threshold + 1e-8 and exceedances == 0:
            errors.append(f"{label}: maximum exceeds {threshold}ms but count is zero")
        if numbers["max_ms"] < threshold - 1e-8 and exceedances > 0:
            errors.append(f"{label}: exceedance count conflicts with maximum")
    return data


def _summary(note_count: int) -> dict[str, Any]:
    return {
        "notes": note_count, "perfect": 0, "good": 0, "empty": 0,
        "missed": note_count, "raw_score": 0,
        "delay_sum_ns": 0, "hit_delay_sum_ns": 0,
    }


def _replay_timing(directory: Path, claimed: dict[str, Any], errors: list[str]) -> dict[str, Any]:
    samples: dict[str, list[int]] = {"frame_gap": [], "render": [], "poll": []}
    for index, row in enumerate(_rows(directory / "timing_samples.csv", TIMING_FIELDS)):
        label = f"timing_samples.csv:{index + 2}"
        kind = row["kind"]
        if kind not in samples:
            raise EvidenceError(f"{label}: unknown timing sample kind {kind!r}")
        if _csv_integer(row["index"], f"{label}.index") != len(samples[kind]):
            errors.append(f"{label}: timing indices must be consecutive within each kind")
        samples[kind].append(_csv_integer(row["duration_ns"], f"{label}.duration_ns"))
    recomputed = {}
    for kind, values in samples.items():
        count = len(values)
        ordered = sorted(values)
        summary = {
            "count": count,
            "mean_ms": sum(values) / count / MS if count else 0,
            "p50_ms": ordered[(count - 1) // 2] / MS if count else 0,
            "p99_ms": ordered[(count * 99 + 99) // 100 - 1] / MS if count else 0,
            "max_ms": ordered[-1] / MS if count else 0,
            "over_16ms": sum(value > 16 * MS for value in values),
            "over_2ms": sum(value > 2 * MS for value in values),
        }
        for field, expected in summary.items():
            value = claimed[kind][field]
            if field in ("count", "over_16ms", "over_2ms"):
                if value != expected:
                    errors.append(f"timing.{kind}.{field}: claimed {value}, raw samples require {expected}")
            elif not math.isclose(value, expected, rel_tol=5e-12, abs_tol=1e-6):
                errors.append(f"timing.{kind}.{field}: claimed {value}, raw samples require {expected}")
        recomputed[kind] = summary
    return recomputed


def _verify(directory: Path, report: dict[str, Any]) -> None:
    errors = report["errors"]
    with (directory / "result.json").open(encoding="utf-8") as stream:
        manifest = _mapping(json.load(stream, object_pairs_hook=_object_no_duplicates,
                                      parse_constant=_nonfinite_json), "result.json")
    if (manifest["benchmark"], manifest["version"]) not in (("CHERS", "0.2.0"), ("ChronoLane", "0.1.0")):
        raise EvidenceError("unsupported benchmark/version; expected CHERS 0.2.0 or legacy ChronoLane 0.1.0")
    report["completed"] = _boolean(manifest["completed"], "completed")
    report["run_valid"] = _boolean(manifest["valid"], "valid")
    reason = manifest["invalid_reason"]
    if not isinstance(reason, str):
        raise EvidenceError("invalid_reason must be a string")
    report["invalid_reason"] = reason
    if report["run_valid"] and (not report["completed"] or reason):
        errors.append("valid=true conflicts with incomplete run or invalid_reason")
    lanes = _integer(manifest["lanes"], "lanes", 2, 16)
    density = _number(manifest["density_attempts_per_second"], "density_attempts_per_second")
    if not 2 * lanes <= density <= 24 * lanes:
        errors.append("density_attempts_per_second is outside [2*lanes,24*lanes]")
    if manifest["entropy_source"] != "CPU RDSEED hardware entropy (no PRNG expansion)":
        errors.append("entropy_source is not the production RDSEED source")
    timeline = _mapping(manifest["timeline_ns"], "timeline_ns")
    for name, expected in TIMELINE.items():
        if _integer(timeline[name], f"timeline_ns.{name}") != expected:
            errors.append(f"timeline_ns.{name} must be {expected}")

    note_rows = _rows(directory / "notes.csv", NOTE_FIELDS)
    notes: list[dict[str, Any]] = []
    lanes_notes: list[list[dict[str, Any]]] = [[] for _ in range(lanes)]
    counts = {stage: 0 for stage in TARGET_WINDOWS}
    previous_target = -1
    for index, row in enumerate(note_rows):
        label = f"notes.csv:{index + 2}"
        note_id = _csv_integer(row["id"], f"{label}.id", maximum=2**64 - 2)
        lane = _csv_integer(row["lane"], f"{label}.lane", maximum=lanes - 1)
        target = _csv_integer(row["target_ns"], f"{label}.target_ns")
        stage = row["stage"]
        if note_id != index:
            errors.append(f"{label}: generated note IDs must be consecutive from zero")
        if stage not in TARGET_WINDOWS:
            raise EvidenceError(f"{label}: unknown note stage {stage!r}")
        start, end = TARGET_WINDOWS[stage]
        if not start <= target < end:
            errors.append(f"{label}: target outside its stage's half-open interval")
        if target < previous_target:
            raise EvidenceError(f"{label}: notes must be sorted by target time")
        if lanes_notes[lane] and target - lanes_notes[lane][-1]["target_ns"] < LOCK:
            errors.append(f"{label}: same-lane notes violate the 40ms lock")
        note = {"id": note_id, "lane": lane, "target_ns": target, "stage": stage}
        notes.append(note)
        lanes_notes[lane].append(note)
        counts[stage] += 1
        previous_target = target

    summaries = {stage: _summary(count) for stage, count in counts.items()}
    input_rows = _rows(directory / "inputs.csv", INPUT_FIELDS)
    cursors = [0] * lanes
    previous_event = -1
    for index, row in enumerate(input_rows):
        label = f"inputs.csv:{index + 2}"
        event_ns = _csv_integer(row["event_ns"], f"{label}.event_ns")
        lane = _csv_integer(row["lane"], f"{label}.lane", maximum=lanes - 1)
        claimed_delay = _csv_integer(row["delay_ns"], f"{label}.delay_ns", -(2**63))
        claimed_note = None if row["note_id"] == "" else _csv_integer(
            row["note_id"], f"{label}.note_id", maximum=2**64 - 2)
        stage = _stage_at(event_ns)
        if event_ns < previous_event:
            raise EvidenceError(f"{label}: input timestamps went backwards")
        previous_event = event_ns
        if stage is None:
            raise EvidenceError(f"{label}: recorded input lies outside both scoring windows")
        if row["stage"] != stage:
            errors.append(f"{label}: claimed stage {row['stage']!r}, expected {stage}")
        lane_chart = lanes_notes[lane]
        while cursors[lane] < len(lane_chart) and lane_chart[cursors[lane]]["target_ns"] < event_ns - GOOD:
            cursors[lane] += 1
        matched = None
        if cursors[lane] < len(lane_chart):
            candidate = lane_chart[cursors[lane]]
            if candidate["stage"] == stage and candidate["target_ns"] <= event_ns + GOOD:
                matched = candidate
                cursors[lane] += 1  # Immediate consumption; no debounce or input lock.
        score = summaries[stage]
        if matched is None:
            expected_note, expected_delay, judgment = None, 0, "empty"
            score["empty"] += 1
            score["raw_score"] -= 5
            score["delay_sum_ns"] += EMPTY_DELAY
        else:
            expected_note = matched["id"]
            expected_delay = event_ns - matched["target_ns"]
            judgment = "perfect" if abs(expected_delay) <= PERFECT else "good"
            score[judgment] += 1
            score["missed"] -= 1
            score["raw_score"] += 2 if judgment == "perfect" else 1
            score["delay_sum_ns"] += abs(expected_delay)
            score["hit_delay_sum_ns"] += abs(expected_delay)
        if claimed_note != expected_note:
            errors.append(f"{label}: claimed note {claimed_note}, replay matched {expected_note}")
        if claimed_delay != expected_delay:
            errors.append(f"{label}: claimed delay {claimed_delay}ns, replay requires {expected_delay}ns")
        if row["judgment"] != judgment:
            errors.append(f"{label}: claimed judgment {row['judgment']!r}, replay requires {judgment}")

    for stage, score in summaries.items():
        key = "trial" if stage == "scored" else "calibration"
        claimed = _mapping(manifest[key], key)
        for name in ("notes", "perfect", "good", "empty", "missed", "raw_score"):
            value = _integer(claimed[name], f"{key}.{name}", -(2**63) if name == "raw_score" else 0)
            if value != score[name]:
                errors.append(f"{key}.{name}: claimed {value}, replay requires {score[name]}")
        score["score"] = score["raw_score"] / (2 * score["notes"]) if score["notes"] else None
        if score["score"] is None:
            if claimed["score"] is not None:
                errors.append(f"{key}.score must be null when no notes were emitted")
        else:
            _close(claimed["score"], score["score"], f"{key}.score", errors)
        score["delaySum_ms"] = score["delay_sum_ns"] / MS
        score["hit_delay_sum_ms"] = score["hit_delay_sum_ns"] / MS
        _close(claimed["delaySum_ms"], score["delaySum_ms"], f"{key}.delaySum_ms", errors)
        _close(claimed["hit_delay_sum_ms"], score["hit_delay_sum_ms"], f"{key}.hit_delay_sum_ms", errors)
    report["recomputed"] = {"trial": summaries["scored"], "calibration": summaries["calibration"]}
    report["replayed_notes"] = len(notes)
    report["replayed_inputs"] = len(input_rows)

    generation = _mapping(manifest["generation"], "generation")
    attempted = _integer(generation["attempts"], "generation.attempts")
    emitted = _integer(generation["emitted"], "generation.emitted")
    dropped = _integer(generation["dropped"], "generation.dropped")
    if attempted != emitted + dropped or emitted != counts["scored"]:
        errors.append("generation attempts/emitted/dropped disagree with accounting or scored chart")
    _close(generation["actual_notes_per_second"], counts["scored"] / 50,
           "generation.actual_notes_per_second", errors)

    observation = _mapping(manifest["observation"], "observation")
    for name, expected in (("width", 640), ("height", 360), ("target_fps", 80), ("target_max_gap_ms", 16)):
        if _number(observation[name], f"observation.{name}") != expected:
            errors.append(f"observation.{name} must be {expected}")
    if observation["format"] != "RGBA8":
        errors.append("observation.format must be RGBA8")
    frames = _distribution(observation["publication_gaps"], "observation.publication_gaps", errors)
    render = _distribution(observation["render_and_publish"], "observation.render_and_publish", errors)
    polling = _distribution(manifest["input_poll_gaps"], "input_poll_gaps", errors)
    timings = _replay_timing(directory, {"frame_gap": frames, "render": render, "poll": polling}, errors)
    report["recomputed_timing"] = timings
    report["timing_compliant"] = timings["frame_gap"]["count"] > 0 and timings["frame_gap"]["over_16ms"] == 0
    if _boolean(observation["timing_compliant"], "observation.timing_compliant") != report["timing_compliant"]:
        errors.append("observation.timing_compliant disagrees with publication gap summary")
    if report["completed"] and (frames["count"] == 0 or polling["count"] == 0):
        errors.append("completed run is missing observation or input-poll timing measurements")


def verify_run(directory: str | Path, require_timing: bool = False) -> dict[str, Any]:
    """Return a serializable audit; malformed evidence is an ordinary failure."""
    directory = Path(directory)
    report: dict[str, Any] = {
        "directory": str(directory), "passed": False, "artifacts_consistent": False,
        "completed": False, "run_valid": False, "timing_compliant": None,
        "require_timing": require_timing, "errors": [],
        "limits": [
            "Timing summaries are recomputed from recorded duration samples; these are producer measurements, not an independent wall-clock attestation.",
            "Discarded attempts and hardware entropy provenance are declared evidence; the emitted chart cannot independently prove them.",
            "Replay detects inconsistent records, not coordinated replacement of all artifacts; it is not a cryptographic attestation.",
        ],
    }
    try:
        _verify(directory, report)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, csv.Error) as error:
        report["errors"].append(f"Malformed or missing evidence: {error}")
    report["artifacts_consistent"] = not report["errors"]
    report["passed"] = (report["artifacts_consistent"] and report["completed"] and report["run_valid"]
                        and (not require_timing or report["timing_compliant"] is True))
    if not report["completed"]:
        report["errors"].append("run did not complete")
    if not report["run_valid"]:
        report["errors"].append("run is not marked valid" + (f": {report['invalid_reason']}" if report.get("invalid_reason") else ""))
    if require_timing and report["timing_compliant"] is not True:
        report["errors"].append("required 16ms maximum publication-gap compliance was not met")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="directory containing result.json, notes.csv, inputs.csv, timing_samples.csv")
    parser.add_argument("--require-timing", action="store_true", help="also fail if any reported model-frame gap exceeded 16ms")
    arguments = parser.parse_args(argv)
    report = verify_run(arguments.directory, arguments.require_timing)
    print(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
