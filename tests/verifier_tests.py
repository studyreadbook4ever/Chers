#!/usr/bin/env python3
"""Tamper and boundary tests for the independent persisted-run verifier."""

import copy
import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("verify_run", ROOT / "tools" / "verify_run.py")
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)
MS = 1_000_000
T = 11_020 * MS


def distribution(count=20, maximum=12.5, over16=0, over2=20):
    return {"count": count, "mean_ms": maximum, "p50_ms": maximum, "p99_ms": maximum,
            "max_ms": maximum, "over_16ms": over16, "over_2ms": over2}


class VerifierTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="chronolane-verifier-")
        self.directory = Path(self.temp.name)
        self.notes = [
            [0, 0, 1_000*MS, "calibration"],
            [1, 1, 5_999*MS, "calibration"],
            [2, 0, T, "scored"],
            [3, 0, T+40*MS, "scored"],
            [4, 1, T+50*MS, "scored"],
            [5, 0, 61_020*MS-1, "scored"],
        ]
        self.inputs = [
            [980*MS, 0, 0, "calibration", "good", -20*MS],
            [6_019*MS, 1, 1, "calibration", "good", 20*MS],
            [T, 0, 2, "scored", "perfect", 0],
            [T, 0, "", "scored", "empty", 0],
            [T+48*MS, 0, 3, "scored", "perfect", 8*MS],
            [61_040*MS-1, 0, 5, "scored", "good", 20*MS],
        ]
        self.manifest = {
            "benchmark": "ChronoLane", "version": "0.1.0", "completed": True, "valid": True,
            "invalid_reason": "", "lanes": 2, "density_attempts_per_second": 4,
            "entropy_source": "CPU RDSEED hardware entropy (no PRNG expansion)",
            "trial": {"notes": 4, "perfect": 2, "good": 1, "empty": 1, "missed": 1,
                      "raw_score": 0, "score": 0, "delaySum_ms": 128, "hit_delay_sum_ms": 28},
            "calibration": {"notes": 2, "perfect": 0, "good": 2, "empty": 0, "missed": 0,
                            "raw_score": 2, "score": 0.5, "delaySum_ms": 40, "hit_delay_sum_ms": 40},
            "generation": {"attempts": 5, "emitted": 4, "dropped": 1, "actual_notes_per_second": 0.08},
            "observation": {"width": 640, "height": 360, "format": "RGBA8", "target_fps": 80,
                            "target_max_gap_ms": 16, "timing_compliant": True,
                            "publication_gaps": distribution(),
                            "render_and_publish": distribution(maximum=0.1, over2=0)},
            "input_poll_gaps": distribution(maximum=1, over2=0),
            "timeline_ns": copy.deepcopy(verifier.TIMELINE),
        }
        self.timings = {"frame_gap": [12_500_000] * 20, "render": [100_000] * 20, "poll": [MS] * 20}

    def tearDown(self):
        self.temp.cleanup()

    def write(self):
        (self.directory / "result.json").write_text(json.dumps(self.manifest), encoding="utf-8")
        for name, fields, rows in (("notes.csv", verifier.NOTE_FIELDS, self.notes),
                                   ("inputs.csv", verifier.INPUT_FIELDS, self.inputs)):
            with (self.directory / name).open("w", newline="", encoding="utf-8") as stream:
                writer = csv.writer(stream)
                writer.writerow(fields)
                writer.writerows(rows)
        with (self.directory / "timing_samples.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(verifier.TIMING_FIELDS)
            for kind, samples in self.timings.items():
                writer.writerows([kind, index, duration] for index, duration in enumerate(samples))

    def audit(self, require_timing=False):
        self.write()
        return verifier.verify_run(self.directory, require_timing)

    def fails(self, text):
        result = self.audit()
        self.assertFalse(result["passed"], result)
        self.assertIn(text, "\n".join(result["errors"]), result)

    def test_valid_replay_and_tail_endpoints(self):
        result = self.audit(require_timing=True)
        self.assertTrue(result["passed"], result)
        self.assertTrue(result["artifacts_consistent"])
        self.assertEqual(result["replayed_inputs"], 6)
        self.assertEqual(result["recomputed"]["trial"]["delay_sum_ns"], 128*MS)

    def test_tampered_score(self):
        self.manifest["trial"]["score"] = 0.75
        self.fails("trial.score")

    def test_consumed_note_cannot_be_matched_twice(self):
        self.inputs[3][2] = 2
        self.inputs[3][4] = "perfect"
        self.fails("replay matched None")

    def test_missing_input_record(self):
        del self.inputs[3]
        self.fails("trial.empty")

    def test_removed_empty_delay_penalty(self):
        self.manifest["trial"]["delaySum_ms"] = 28
        self.fails("trial.delaySum_ms")

    def test_tampered_exact_delay(self):
        self.inputs[4][5] -= 1
        self.fails("claimed delay")

    def test_tampered_judgment(self):
        self.inputs[-1][4] = "perfect"
        self.fails("claimed judgment")

    def test_perfect_boundary_plus_one_ns(self):
        self.inputs[4][0] += 1
        self.inputs[4][5] += 1
        self.fails("replay requires good")

    def test_earliest_eligible_note_at_exact_tie(self):
        self.inputs[2] = [T+20*MS, 0, 3, "scored", "good", -20*MS]
        self.inputs[3][0] = T+20*MS
        self.fails("replay matched 2")

    def test_input_chronology(self):
        self.inputs[4][0] = T-1
        self.fails("timestamps went backwards")

    def test_target_lock_one_ns_violation(self):
        self.notes[3][2] -= 1
        self.fails("40ms lock")

    def test_wrong_stage(self):
        self.inputs[2][3] = "calibration"
        self.fails("claimed stage")

    def test_wait_input_cannot_appear_in_log(self):
        self.inputs.insert(2, [8_000*MS, 0, "", "scored", "empty", 0])
        self.fails("outside both scoring windows")

    def test_target_stage_end_is_excluded(self):
        self.notes[-1][2] += 1
        self.fails("target outside")

    def test_missing_note_record(self):
        del self.notes[4]
        self.fails("consecutive")

    def test_density_range(self):
        self.manifest["density_attempts_per_second"] = 49
        self.fails("outside [2*lanes,24*lanes]")

    def test_generation_counts(self):
        self.manifest["generation"]["dropped"] += 1
        self.fails("generation attempts/emitted/dropped")

    def test_timeline_tamper(self):
        self.manifest["timeline_ns"]["finish"] -= MS
        self.fails("timeline_ns.finish")

    def test_invalid_and_incomplete_are_nonpassing_with_consistent_replay(self):
        self.manifest["completed"] = False
        self.manifest["valid"] = False
        self.manifest["invalid_reason"] = "interrupted by signal"
        result = self.audit()
        self.assertTrue(result["artifacts_consistent"], result)
        self.assertFalse(result["passed"])
        self.assertFalse(result["completed"])
        self.assertIn("run did not complete", result["errors"])

    def test_timing_is_separate_from_score_validity(self):
        self.manifest["observation"]["timing_compliant"] = False
        self.manifest["observation"]["publication_gaps"] = distribution(maximum=17, over16=20)
        self.timings["frame_gap"] = [17 * MS] * 20
        result = self.audit()
        self.assertTrue(result["passed"], result)
        self.assertFalse(result["timing_compliant"])
        strict = verifier.verify_run(self.directory, require_timing=True)
        self.assertFalse(strict["passed"])
        self.assertTrue(strict["artifacts_consistent"])

    def test_falsely_claimed_timing_compliance(self):
        self.manifest["observation"]["timing_compliant"] = False
        self.fails("timing_compliant disagrees")

    def test_timing_summary_internal_conflict(self):
        self.manifest["observation"]["publication_gaps"]["over_16ms"] = 1
        self.fails("exceedance count conflicts")

    def test_raw_timing_tamper(self):
        self.timings["frame_gap"][0] = 17 * MS
        self.fails("raw samples require")

    def test_missing_timing_sample(self):
        del self.timings["poll"][0]
        self.fails("timing.poll.count")

    def test_missing_timing_file(self):
        self.write()
        (self.directory / "timing_samples.csv").unlink()
        result = verifier.verify_run(self.directory)
        self.assertFalse(result["passed"])
        self.assertIn("timing_samples.csv", "\n".join(result["errors"]))

    def test_timing_percentiles_replayed_from_nonuniform_samples(self):
        self.timings["frame_gap"] = [4*MS, MS, 3*MS, 2*MS]
        self.manifest["observation"]["publication_gaps"] = {
            "count": 4, "mean_ms": 2.5, "p50_ms": 2, "p99_ms": 4,
            "max_ms": 4, "over_16ms": 0, "over_2ms": 2,
        }
        result = self.audit()
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["recomputed_timing"]["frame_gap"]["p50_ms"], 2)

    def test_sixteen_ms_plus_one_ns_is_noncompliant(self):
        self.timings["frame_gap"] = [16*MS+1]
        self.manifest["observation"]["timing_compliant"] = False
        self.manifest["observation"]["publication_gaps"] = distribution(
            count=1, maximum=16.000001, over16=1, over2=1)
        result = self.audit()
        self.assertTrue(result["passed"], result)
        self.assertFalse(result["timing_compliant"])

    def test_nonfinite_json_rejected(self):
        self.manifest["trial"]["score"] = float("nan")
        self.fails("non-finite")

    def test_boolean_not_accepted_as_integer(self):
        self.manifest["trial"]["perfect"] = True
        self.fails("must be an integer")

    def test_duplicate_json_keys_rejected(self):
        self.write()
        path = self.directory / "result.json"
        path.write_text(path.read_text().replace('"lanes": 2', '"lanes": 2, "lanes": 2'))
        self.assertFalse(verifier.verify_run(self.directory)["passed"])

    def test_missing_file(self):
        self.write()
        (self.directory / "inputs.csv").unlink()
        result = verifier.verify_run(self.directory)
        self.assertFalse(result["passed"])
        self.assertIn("Malformed or missing evidence", result["errors"][0])

    def test_negative_score_preserved(self):
        self.inputs.insert(4, [T, 0, "", "scored", "empty", 0])
        self.manifest["trial"].update(empty=2, raw_score=-5, score=-0.625, delaySum_ms=228)
        result = self.audit()
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["recomputed"]["trial"]["score"], -0.625)

    def test_no_hits_and_zero_note_stage(self):
        self.notes = []
        self.inputs = []
        for stage in ("calibration", "trial"):
            self.manifest[stage] = {"notes": 0, "perfect": 0, "good": 0, "empty": 0,
                                    "missed": 0, "raw_score": 0, "score": None,
                                    "delaySum_ms": 0, "hit_delay_sum_ms": 0}
        self.manifest["generation"] = {"attempts": 0, "emitted": 0, "dropped": 0,
                                       "actual_notes_per_second": 0}
        result = self.audit()
        self.assertTrue(result["passed"], result)
        self.assertIsNone(result["recomputed"]["trial"]["score"])

    def test_cli_exit_status(self):
        self.write()
        command = [sys.executable, str(ROOT / "tools" / "verify_run.py"), str(self.directory)]
        valid = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)
        self.assertTrue(json.loads(valid.stdout)["passed"])
        self.manifest["trial"]["score"] = 1
        self.write()
        invalid = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(invalid.returncode, 1)
        self.assertFalse(json.loads(invalid.stdout)["passed"])


if __name__ == "__main__":
    unittest.main()
