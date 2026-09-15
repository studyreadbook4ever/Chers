#!/usr/bin/env python3
"""Offline publication checks for native Codex benchmark reports."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from report_codex_runs import generate_report, markdown_table, select_runs, summarize_run


def fixtures():
    run = {
        "requested_model": "test-native-model", "effort": "low", "status": "completed",
        "started_utc": "2026-09-15T23:00:00Z",
        "publishable": True, "verification_passed": True,
        "thread_settings": {"model": "test-native-model", "reasoningEffort": "low",
                            "modelProvider": "openai", "serviceTier": "priority"},
        "benchmark_result": "benchmark/run/result.json",
        "observation_to_tool_ms": {"count": 2, "mean": 1250, "median": 1250, "max": 1600},
        "model_decisions": [{}, {}],
    }
    result = {
        "completed": True, "valid": True,
        "trial": {"notes": 100, "perfect": 0, "good": 0, "empty": 0, "missed": 100,
                  "raw_score": 0, "score": 0, "delaySum_ms": 0, "hit_delay_sum_ms": 0},
        "observation": {"timing_compliant": True, "publication_gaps": {"max_ms": 15.5}},
    }
    bridge = {
        "input_mode": "keyboard", "error": None,
        "counts": {"action_proposed": 4, "action_sent": 0, "action_dropped": 4},
        "receipt_gap_max_ms": 18.75, "receipt_gaps_over_16ms": 1,
    }
    verification = {"passed": True, "completed": True, "run_valid": True}
    return run, result, bridge, verification


class ReportPublicationTests(unittest.TestCase):
    def summarize(self, mutate=None):
        run, result, bridge, verification = fixtures()
        if mutate:
            mutate(run, result, bridge, verification)
        with tempfile.TemporaryDirectory() as directory:
            trial = Path(directory) / "test-native-model-low-1"
            artifacts = {
                "run.json": run, "benchmark/run/result.json": result,
                "agent/bridge.json": bridge, "verification.json": verification,
            }
            for relative, data in artifacts.items():
                path = trial / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(data))
            return summarize_run(trial)

    def assert_not_numeric(self, row):
        self.assertFalse(row["publishable"])
        self.assertNotIn("score_percent", row)
        self.assertNotIn("hit_mean_ms", row)
        line = markdown_table([row]).splitlines()[-1]
        self.assertNotIn("%", line)
        self.assertIn("| — | — | — | — |", line)

    def test_valid_zero_score_has_no_successful_hit_mean(self):
        row = self.summarize()
        self.assertTrue(row["publishable"])
        self.assertEqual(row["score_percent"], 0)
        self.assertEqual(row["hits"], 0)
        self.assertEqual(row["notes"], 100)
        self.assertEqual(row["delaySum_ms"], 0)
        self.assertIsNone(row["hit_mean_ms"])
        self.assertEqual(row["tool_calls"], 2)
        self.assertNotIn("model_decisions", row)
        line = markdown_table([row]).splitlines()[-1]
        self.assertIn("| 0.000% | 0 / 100 | 0.000 | N/A | 1.250 |", line)

    def test_successful_hit_average_excludes_empty_tap_penalty(self):
        def mixed_hits(run, result, bridge, verification):
            result["trial"].update(notes=10, perfect=2, good=1, empty=3, missed=7,
                raw_score=-10, score=-0.5, delaySum_ms=309.87654, hit_delay_sum_ms=9.87654)

        row = self.summarize(mixed_hits)
        self.assertTrue(row["publishable"])
        self.assertEqual(row["score_percent"], -50)
        self.assertEqual(row["hits"], 3)
        self.assertAlmostEqual(row["hit_rate"], 0.3)
        self.assertAlmostEqual(row["delaySum_ms"], 309.87654)
        self.assertAlmostEqual(row["hit_mean_ms"], 3.29218)
        line = markdown_table([row]).splitlines()[-1]
        self.assertIn("| -50.000% | 3 / 10 | 309.877 | 3.292 |", line)

    def test_failed_replay_refuses_numeric_result(self):
        self.assert_not_numeric(self.summarize(
            lambda run, result, bridge, verification: verification.update(passed=False)))

    def test_run_replay_flag_must_agree_with_verification_artifact(self):
        self.assert_not_numeric(self.summarize(
            lambda run, result, bridge, verification: run.update(verification_passed=False)))

    def test_model_or_effort_mismatch_refuses_numeric_result(self):
        for key, replacement in (("model", "substituted-model"), ("reasoningEffort", "high")):
            with self.subTest(key=key):
                self.assert_not_numeric(self.summarize(
                    lambda run, result, bridge, verification: run["thread_settings"].update({key: replacement})))

    def test_nonkeyboard_input_route_refuses_numeric_result(self):
        for route in ("transport-test", "injected-test", None):
            with self.subTest(route=route):
                self.assert_not_numeric(self.summarize(
                    lambda run, result, bridge, verification: bridge.update(input_mode=route)))

    def test_nonpositive_note_count_refuses_numeric_result(self):
        for notes in (0, -1):
            with self.subTest(notes=notes):
                self.assert_not_numeric(self.summarize(
                    lambda run, result, bridge, verification: result["trial"].update(notes=notes)))

    def test_incomplete_or_invalid_engine_result_refuses_numeric_result(self):
        for field in ("completed", "valid"):
            with self.subTest(field=field):
                self.assert_not_numeric(self.summarize(
                    lambda run, result, bridge, verification: result.update({field: False})))

    def test_failed_run_does_not_require_benchmark_files(self):
        with tempfile.TemporaryDirectory() as directory:
            trial = Path(directory)
            (trial / "run.json").write_text(json.dumps({
                "requested_model": "test-native-model", "effort": "low", "status": "failed",
                "publishable": False, "error": "setup failed before START",
            }))
            row = summarize_run(trial)
        self.assert_not_numeric(row)
        self.assertEqual(row["error"], "setup failed before START")

    def test_failed_status_overrides_stale_publishable_flag(self):
        self.assert_not_numeric(self.summarize(
            lambda run, result, bridge, verification: run.update(status="failed")))

    def test_missing_latency_is_na_and_timing_scopes_are_distinct(self):
        def missing_latency(run, result, bridge, verification):
            run["observation_to_tool_ms"] = {"count": 0, "mean": None, "median": None, "max": None}

        row = self.summarize(missing_latency)
        self.assertTrue(row["engine_timing_compliant"])
        self.assertEqual(row["engine_max_publication_gap_ms"], 15.5)
        self.assertEqual(row["receiver_lifetime_max_gap_ms"], 18.75)
        self.assertEqual(row["receiver_lifetime_gaps_over_16ms"], 1)
        self.assertTrue(markdown_table([row]).splitlines()[-1].endswith("| N/A | N/A |"))


class SelectionTests(unittest.TestCase):
    @staticmethod
    def catalog():
        return {"observed_utc": "2026-09-15T22:00:00Z", "client_version": "test-version", "models": [
            {"model": "model-b", "supportedReasoningEfforts": [
                {"reasoningEffort": "low"}, {"reasoningEffort": "ultra"}]},
            {"model": "model-a", "supportedReasoningEfforts": [
                {"reasoningEffort": "low"}, {"reasoningEffort": "max"}]},
        ]}

    @staticmethod
    def row(directory, time="2026-09-15T23:00:00Z", model="model-b", effort="low", eligible=True, score=0):
        return {"model": model, "effort": effort, "directory": directory, "started_utc": time,
                "publishable": eligible, "score_percent": score,
                **({"error": "transport lost"} if not eligible else {})}

    def test_first_eligible_is_selected_by_time_not_score_or_directory(self):
        rows = [self.row("a-later-best", "2026-09-15T23:02:00Z", score=100),
                self.row("z-first-valid", "2026-09-15T23:01:00Z", score=0),
                self.row("first-failed", "2026-09-15T23:00:00Z", eligible=False)]
        selection = select_runs(rows, self.catalog())
        self.assertEqual([row["directory"] for row in selection["selected"]], ["z-first-valid"])
        attempts = {row["directory"]: row for row in selection["attempts"]}
        self.assertFalse(attempts["first-failed"]["eligible"])
        self.assertEqual(attempts["first-failed"]["reason"], "transport lost")
        self.assertTrue(attempts["z-first-valid"]["selected"])
        self.assertFalse(attempts["a-later-best"]["selected"])
        self.assertIn("earlier", attempts["a-later-best"]["reason"])

    def test_display_order_is_catalog_then_low_then_maximum(self):
        rows = [self.row("a-max", model="model-a", effort="max"),
                self.row("b-ultra", effort="ultra"),
                self.row("a-low", model="model-a"), self.row("b-low")]
        selection = select_runs(rows, self.catalog())
        self.assertEqual([row["directory"] for row in selection["selected"]],
                         ["b-low", "b-ultra", "a-low", "a-max"])
        self.assertEqual(selection["missing_configurations"], [])

    def test_timezones_are_normalized_and_equal_times_use_path_tiebreak(self):
        rows = [self.row("z-equal", "2026-09-16T08:00:00+09:00"),
                self.row("a-equal", "2026-09-15T23:00:00Z"),
                self.row("0-later", "2026-09-15T23:01:00Z")]
        self.assertEqual(select_runs(rows, self.catalog())["selected"][0]["directory"], "a-equal")

    def test_missing_or_unqualified_timestamp_cannot_be_selected(self):
        for timestamp in (None, "invalid", "2026-09-15T23:00:00"):
            with self.subTest(timestamp=timestamp):
                selection = select_runs([self.row("missing-time", timestamp)], self.catalog())
                self.assertEqual(selection["selected"], [])
                self.assertFalse(selection["attempts"][0]["eligible"])
                self.assertIn("started_utc", selection["attempts"][0]["reason"])
                self.assertIn({"model": "model-b", "effort": "low"}, selection["missing_configurations"])

    def test_generator_keeps_failed_attempts_and_writes_reproducible_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = {"observed_utc": "2026-09-15T22:00:00Z", "client_version": "test-version", "models": [
                {"model": "test-native-model", "supportedReasoningEfforts": [{"reasoningEffort": "low"}]}]}
            (root / "model-catalog.json").write_text(json.dumps(catalog))
            for name, started, eligible in (("first-failed", "2026-09-15T22:00:00Z", False),
                                            ("nested/first-valid", "2026-09-15T23:00:00Z", True)):
                run, result, bridge, verification = fixtures()
                run.update(started_utc=started, lanes=8, density=20)
                if not eligible:
                    run.update(publishable=False, status="failed", error="recorded transport failure")
                artifacts = {"run.json": run, "benchmark/run/result.json": result,
                             "agent/bridge.json": bridge, "verification.json": verification}
                for path, value in artifacts.items():
                    target = root / name / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(json.dumps(value))
            record, selection = generate_report(root)
            self.assertEqual(len(record["runs"]), 2)
            self.assertEqual(selection["selected"][0]["directory"], "nested/first-valid")
            self.assertEqual(json.loads((root / "selection.json").read_text()), selection)
            report = (root / "REPORT.md").read_text()
            self.assertIn("## Selected results", report)
            self.assertIn("## All attempted runs", report)
            self.assertIn("recorded transport failure", report)
            self.assertIn("runtime, transport", report)
            self.assertIn("not pure inference time", report)
            self.assertIn("hit mean is N/A", report)


if __name__ == "__main__":
    unittest.main()
