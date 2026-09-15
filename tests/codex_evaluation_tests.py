#!/usr/bin/env python3
"""Offline checks for evaluation identity, image routing, and failure cleanup."""

from contextlib import ExitStack, redirect_stdout
import base64
import io
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))
sys.path.insert(0, str(ROOT / "tools"))
from codex_appserver_client import AppServer
import run_codex_evaluations as evaluation


MODEL = "test-native-model"
THREAD = "test-thread"


def tool_call(arguments, number=1, tool="chers_step"):
    return {"id": number, "method": "item/tool/call",
            "params": {"threadId": THREAD, "tool": tool, "arguments": arguments}}


class FakeServer:
    def __init__(self, messages=()):
        self.messages = list(messages)
        self.calls = []
        self.responses = []
        self.thread_requests = []

    def start_thread(self, model, effort, instructions, tools, cwd):
        self.thread_requests.append((model, effort, instructions, tools, cwd))
        return {"thread": {"id": THREAD}, "model": model,
                "modelProvider": "openai", "reasoningEffort": effort}

    def call(self, method, params, **kwargs):
        self.calls.append((method, params, kwargs))
        if method == "turn/start":
            return {"turn": {"id": "test-turn"}}
        return {}

    def receive(self, timeout=1):
        message = self.messages.pop(0) if self.messages else {"transport_closed": True}
        return time.monotonic_ns(), message

    def respond(self, number, result):
        self.responses.append((number, result))


class FakeGame:
    pid = 31337

    def __init__(self, returncode=None):
        self.returncode = returncode
        self.terminated = False
        self.killed = False
        self.waited = False

    def poll(self):
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = -15

    def kill(self):
        self.killed = True
        self.returncode = -9

    def wait(self, timeout=None):
        self.waited = True
        return self.returncode


class FakeBridge:
    def __init__(self, error=None, close_error=None):
        self.error = error
        self.close_error = close_error
        self.closed = False
        self.exported = False
        self.starts = 0
        self.events = []
        self.submitted = []

    def snapshot(self):
        return {"error": self.error, "events": list(self.events)}

    def start(self):
        self.starts += 1

    def observe(self):
        identifier = f"obs-{len(self.events) + 1}"
        self.events.append({"type": "observation", "observation_id": identifier,
                            "local_receipt_ns": time.monotonic_ns()})
        return {"observation_id": identifier, "image_url": "data:image/png;base64,ZmFrZQ=="}

    def submit(self, observation_id, actions):
        self.submitted.append((observation_id, actions))
        return {"accepted": len(actions), "scheduled": len(actions), "dropped_late": 0}

    def close(self):
        self.closed = True
        if self.close_error:
            raise RuntimeError(self.close_error)

    def export(self, directory):
        self.exported = True
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "bridge.json").write_text(json.dumps(self.snapshot()))


class ThreadIdentityTests(unittest.TestCase):
    def make_server(self, result):
        # Bypass process startup: only exercise the real request/identity checks.
        server = object.__new__(AppServer)
        server.call = Mock(return_value=result)
        return server

    def test_identity_checked_and_restrictions_sent(self):
        resolved = {"thread": {"id": THREAD}, "model": MODEL,
                    "reasoningEffort": "low", "modelProvider": "openai"}
        server = self.make_server(resolved)
        tools = [{"name": "chers_step"}]
        result = server.start_thread(MODEL, "low", "public rules", tools, Path("/empty"))
        self.assertIs(result, resolved)
        method, request = server.call.call_args.args
        self.assertEqual(method, "thread/start")
        self.assertEqual(request["model"], MODEL)
        self.assertEqual(request["config"]["model_reasoning_effort"], "low")
        self.assertIs(request["allowProviderModelFallback"], False)
        self.assertEqual(request["environments"], [])
        self.assertEqual(request["selectedCapabilityRoots"], [])
        self.assertEqual(request["dynamicTools"], tools)

    def test_resolved_model_fallback_is_rejected(self):
        server = self.make_server({"model": "different-model", "reasoningEffort": "low"})
        with self.assertRaisesRegex(RuntimeError, "Requested model.*replaced"):
            server.start_thread(MODEL, "low", "rules", [], Path("/empty"))

    def test_resolved_effort_fallback_or_missing_effort_is_rejected(self):
        for resolved_effort in ("medium", None):
            with self.subTest(resolved_effort=resolved_effort):
                result = {"model": MODEL}
                if resolved_effort is not None:
                    result["reasoningEffort"] = resolved_effort
                server = self.make_server(result)
                with self.assertRaisesRegex(RuntimeError, "Requested effort.*replaced"):
                    server.start_thread(MODEL, "low", "rules", [], Path("/empty"))


class ImageRouteTests(unittest.TestCase):
    def probe(self, messages):
        server = FakeServer(messages)
        # Exercise routing and result validation independently of the optional
        # font renderer. The fake model returns text; no actual vision is tested.
        fixture = ("01230", evaluation.rgba_png(bytes((0, 0, 0, 255)) * (640 * 360)))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "probe"
            with patch.object(evaluation, "render_vision_challenge", return_value=fixture):
                record = evaluation.probe_vision(server, MODEL, Path(directory), output)
            image = (output / "image.png").read_bytes()
            persisted = json.loads((output / "result.json").read_text())
        return server, record, image, persisted

    @staticmethod
    def answer_messages(text="01230"):
        return [
            {"method": "item/completed", "params": {"threadId": THREAD,
                "item": {"type": "agentMessage", "text": text}}},
            {"method": "turn/completed", "params": {"threadId": THREAD,
                "turn": {"status": "completed"}}},
        ]

    def test_native_image_is_delivered_through_gameplay_tool_route(self):
        server, result, image, persisted = self.probe(
            [tool_call({}, tool="vision_probe")] + self.answer_messages())
        self.assertTrue(result["passed"])
        self.assertTrue(persisted["passed"])
        payload = server.responses[0][1]
        self.assertTrue(payload["success"])
        content = payload["contentItems"]
        self.assertEqual([item["type"] for item in content], ["inputImage"])
        self.assertEqual(base64.b64decode(content[0]["imageUrl"].split(",", 1)[1]), image)
        first_turn = next(params for method, params, _ in server.calls if method == "turn/start")
        self.assertEqual([item["type"] for item in first_turn["input"]], ["text"])
        self.assertNotIn("01230", json.dumps(first_turn))
        self.assertNotIn("01230", server.thread_requests[0][2])

    def test_correct_text_without_receiving_image_does_not_pass(self):
        _, result, _, _ = self.probe(self.answer_messages())
        self.assertFalse(result["passed"])

    def test_wrong_image_transcription_does_not_pass(self):
        _, result, _, _ = self.probe(
            [tool_call({}, tool="vision_probe")] + self.answer_messages("99999"))
        self.assertFalse(result["passed"])


class TrialLifecycleTests(unittest.TestCase):
    def run_fake(self, game, bridge, server, *, benchmark=None, verification_passed=True):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "trial"

            def launch(*args, **kwargs):
                if benchmark is not None:
                    results = output / "benchmark" / "run"
                    results.mkdir(parents=True)
                    (results / "result.json").write_text(json.dumps(benchmark))
                return game

            with ExitStack() as stack:
                stack.enter_context(patch.object(evaluation.subprocess, "Popen", side_effect=launch))
                verify = stack.enter_context(patch.object(evaluation.subprocess, "run", return_value=
                    Mock(returncode=0 if verification_passed else 1,
                         stdout=json.dumps({"passed": verification_passed}), stderr="")))
                stack.enter_context(patch.object(evaluation, "find_window_for_pid", return_value=123))
                stack.enter_context(patch.object(evaluation, "NativeVisionBridge", return_value=bridge))
                stack.enter_context(patch.object(evaluation.time, "sleep"))
                stack.enter_context(redirect_stdout(io.StringIO()))
                report = evaluation.run_trial(server, MODEL, "low", Path(directory), output)
                persisted = json.loads((output / "run.json").read_text())
                event_log = json.loads((output / "events.json").read_text())
                verifier_calls = verify.call_count
        return report, persisted, event_log, verifier_calls

    def test_bridge_close_failure_still_terminates_game_and_writes_report(self):
        game = FakeGame()
        bridge = FakeBridge(close_error="test close failure")
        server = FakeServer()  # Closed transport triggers ordinary failed-run cleanup.
        report, persisted, _, _ = self.run_fake(game, bridge, server)
        self.assertTrue(game.terminated)
        self.assertTrue(game.waited)
        self.assertTrue(bridge.closed)
        self.assertFalse(bridge.exported)
        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["publishable"])
        self.assertIn("test close failure", " ".join(persisted["cleanup_errors"]))
        self.assertTrue(any(method == "turn/interrupt" for method, _, _ in server.calls))

    def test_bridge_failure_before_start_is_not_a_model_score(self):
        game = FakeGame()
        bridge = FakeBridge(error="capture connection lost")
        report, persisted, _, _ = self.run_fake(game, bridge, FakeServer())
        self.assertTrue(game.terminated)
        self.assertTrue(bridge.exported)
        self.assertEqual(report["status"], "failed")
        self.assertIn("bridge failed before trial completion", persisted["error"])
        self.assertFalse(report["publishable"])

    def test_nonzero_game_exit_cannot_publish_even_if_verifier_is_mocked_successful(self):
        report, _, _, calls = self.run_fake(FakeGame(returncode=2), FakeBridge(), FakeServer(),
            benchmark={"valid": True, "completed": True})
        self.assertEqual(calls, 1)
        self.assertTrue(report["verification_passed"])
        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["publishable"])

    def test_failed_replay_cannot_publish_normal_game_exit(self):
        report, _, _, _ = self.run_fake(FakeGame(returncode=0), FakeBridge(), FakeServer(),
            benchmark={"valid": True, "completed": True}, verification_passed=False)
        self.assertFalse(report["verification_passed"])
        self.assertFalse(report["publishable"])

    def test_malformed_model_arguments_receive_error_and_allow_next_call(self):
        game, bridge = FakeGame(), FakeBridge()
        server = FakeServer([
            tool_call([], number=1),
            tool_call({"actions": []}, number=2),
        ])
        report, persisted, _, _ = self.run_fake(game, bridge, server)
        self.assertEqual(len(server.responses), 2)
        self.assertFalse(server.responses[0][1]["success"])
        self.assertTrue(server.responses[1][1]["success"])
        self.assertEqual(bridge.starts, 1)
        self.assertIn("invalid_arguments", persisted["model_decisions"][0])
        self.assertEqual(len(report["model_decisions"]), 2)
        self.assertTrue(game.terminated)

    def test_actions_use_latest_returned_image_and_invalid_call_keeps_reference(self):
        game, bridge = FakeGame(), FakeBridge()
        first_actions = [{"lane": "A", "delay_ms": 123}]
        second_actions = [{"lane": "C", "delay_ms": 345}]
        server = FakeServer([
            tool_call({"actions": []}, number=1),
            tool_call([], number=2),
            tool_call({"actions": first_actions}, number=3),
            tool_call({"actions": second_actions}, number=4),
        ])
        self.run_fake(game, bridge, server)
        self.assertEqual([reply["success"] for _, reply in server.responses],
                         [True, False, True, True])
        self.assertEqual(bridge.starts, 1)
        self.assertEqual(bridge.submitted, [("obs-1", first_actions), ("obs-2", second_actions)])
        self.assertEqual(len(bridge.events), 3)

    def test_first_nonempty_action_batch_cannot_start_or_tap(self):
        game, bridge = FakeGame(), FakeBridge()
        server = FakeServer([
            tool_call({"actions": [{"lane": "A", "delay_ms": 0}]}, number=1),
            tool_call({"actions": []}, number=2),
        ])
        self.run_fake(game, bridge, server)
        self.assertEqual([reply["success"] for _, reply in server.responses], [False, True])
        self.assertEqual(bridge.starts, 1)
        self.assertEqual(bridge.submitted, [])


if __name__ == "__main__":
    unittest.main()
