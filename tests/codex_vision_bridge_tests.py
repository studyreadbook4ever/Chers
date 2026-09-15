#!/usr/bin/env python3
"""Contract tests for the model-agnostic native vision/keyboard bridge."""

import base64
import ctypes as C
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples"))
from codex_vision_bridge import (NativeVisionBridge, XTestKeyboard, rgba_png,
                                 _XErrorEvent, _record_x11_error, _property32)


PIXELS = bytes(range(256)) * (640 * 360 * 4 // 256)


class Clock:
    def __init__(self):
        self.now = 1_000_000_000

    def __call__(self):
        return self.now


class FakeClient:
    def __init__(self):
        self.frames = [(SimpleNamespace(width=640, height=360, stride=2560,
                                       sequence=9000, note_id="forbidden-test-value"), PIXELS)]
        self.closed = False
        self.taps = []
        self.starts = 0

    def frame(self, timeout_ms):
        return self.frames.pop(0) if self.frames else None

    def close(self):
        self.closed = True

    def tap(self, lane):
        self.taps.append(lane)

    def start(self):
        self.starts += 1


class Keys:
    def __init__(self):
        self.taps = []
        self.starts = 0
        self.closed = False

    def start(self):
        self.starts += 1

    def tap(self, lane):
        self.taps.append(lane)

    def close(self):
        self.closed = True


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.clock, self.client, self.keys = Clock(), FakeClient(), Keys()
        self.bridge = NativeVisionBridge(8, client=self.client, key_sink=self.keys,
                                         clock_ns=self.clock, background=False)
        self.addCleanup(self.bridge.close)

    def observation(self):
        return self.bridge.observe()["observation_id"]

    def test_png_retains_every_rgba_byte(self):
        png = rgba_png(PIXELS)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        offset, encoded, header = 8, b"", None
        while offset < len(png):
            length = struct.unpack(">I", png[offset:offset + 4])[0]
            kind = png[offset + 4:offset + 8]
            payload = png[offset + 8:offset + 8 + length]
            crc = struct.unpack(">I", png[offset + 8 + length:offset + 12 + length])[0]
            self.assertEqual(crc, zlib.crc32(kind + payload) & 0xffffffff)
            if kind == b"IHDR":
                header = struct.unpack(">IIBBBBB", payload)
            elif kind == b"IDAT":
                encoded += payload
            offset += length + 12
        self.assertEqual(header, (640, 360, 8, 6, 0, 0, 0))
        rows = zlib.decompress(encoded)
        self.assertEqual(len(rows), 360 * 2561)
        self.assertTrue(all(rows[y * 2561] == 0 for y in range(360)))
        decoded = b"".join(rows[y * 2561 + 1:(y + 1) * 2561] for y in range(360))
        self.assertEqual(decoded, PIXELS)

    def test_model_observation_contains_only_image_and_local_id(self):
        result = self.bridge.observe()
        self.assertEqual(set(result), {"image_url", "observation_id"})
        self.assertEqual(result["observation_id"], "obs-000001")
        self.assertEqual(base64.b64decode(result["image_url"].split(",", 1)[1]), rgba_png(PIXELS))
        self.assertNotIn("forbidden-test-value", json.dumps(self.bridge.snapshot()))
        self.assertNotIn("sequence", result)
        self.assertNotIn("game_time", result)

    def test_fast_observer_waits_for_a_new_captured_frame(self):
        first = self.bridge.observe()
        finished = threading.Event()
        results, errors = [], []

        def observe_again():
            try:
                results.append(self.bridge.observe(timeout=1))
            except Exception as error:
                errors.append(error)
            finally:
                finished.set()

        waiter = threading.Thread(target=observe_again)
        waiter.start()
        try:
            self.assertFalse(finished.wait(0.02), "cached frame was returned twice")
            self.clock.now += 12_500_000
            changed = b"\xff" + PIXELS[1:]
            self.client.frames.append((SimpleNamespace(width=640, height=360, stride=2560), changed))
            self.bridge._capture_once()
            self.assertTrue(finished.wait(1), "new capture did not wake the observer")
            self.assertEqual(errors, [])
            self.assertEqual(results[0]["observation_id"], "obs-000002")
            self.assertNotEqual(results[0]["image_url"], first["image_url"])
        finally:
            waiter.join(timeout=2)

    def test_new_frame_wait_uses_elapsed_real_time_with_frozen_injected_clock(self):
        self.bridge.observe()
        started = time.monotonic()
        with self.assertRaisesRegex(TimeoutError, "no new public frame"):
            self.bridge.observe(timeout=0.02)
        self.assertLess(time.monotonic() - started, 0.5)
        self.assertEqual(self.bridge.snapshot()["observations_returned"], 1)

    def test_evidence_capacity_holds_a_complete_eighty_fps_session(self):
        # Simulate 65 seconds including ready/final margins, advancing the local
        # receipt clock. PNG encoding itself is already round-trip tested above.
        # Reuse a real encoded PNG here to measure the same byte-accounting cap.
        png = rgba_png(PIXELS)
        with patch("codex_vision_bridge.rgba_png", return_value=png):
            for index in range(80 * 65):
                if index:
                    self.clock.now += 12_500_000
                    self.client.frames.append((SimpleNamespace(width=640, height=360, stride=2560), PIXELS))
                observation = self.bridge.observe()
        self.assertEqual(observation["observation_id"], "obs-005200")
        self.assertEqual(self.bridge.snapshot()["observations_returned"], 5200)
        self.assertEqual(self.bridge._png_bytes, len(png) * 5200)

    def test_receipt_time_is_schedule_reference(self):
        identifier = self.observation()
        self.bridge.start()
        self.clock.now += 100_000_000
        self.bridge.submit(identifier, [{"lane": "B", "delay_ms": 200}])
        self.bridge._dispatch_due()
        self.assertEqual(self.keys.taps, [])
        self.clock.now += 100_000_000
        self.bridge._dispatch_due()
        self.assertEqual(self.keys.taps, [1])

    def test_duplicate_actions_are_never_debounced(self):
        identifier = self.observation()
        self.bridge.start()
        result = self.bridge.submit(identifier, [{"lane": "A", "delay_ms": 8}] * 3)
        self.assertEqual(result["scheduled"], 3)
        self.clock.now += 8_000_000
        self.bridge._dispatch_due()
        self.assertEqual(self.keys.taps, [0, 0, 0])
        self.assertEqual(self.bridge.snapshot()["counts"]["action_sent"], 3)

    def test_twenty_ms_boundary_and_late_submission(self):
        identifier = self.observation()
        self.bridge.start()
        self.clock.now += 20_000_000
        result = self.bridge.submit(identifier, [{"lane": 0, "delay_ms": 0}])
        self.assertEqual(result["scheduled"], 1)
        self.bridge._dispatch_due()
        self.assertEqual(self.keys.taps, [0])
        self.clock.now += 1
        result = self.bridge.submit(identifier, [{"lane": 0, "delay_ms": 0}])
        self.assertEqual(result["dropped_late"], 1)
        self.bridge._dispatch_due()
        self.assertEqual(self.keys.taps, [0])

    def test_late_dispatch_is_dropped_and_recorded(self):
        identifier = self.observation()
        self.bridge.start()
        self.bridge.submit(identifier, [{"lane": 0, "delay_ms": 10}])
        self.clock.now += 30_000_001
        self.bridge._dispatch_due()
        self.assertEqual(self.keys.taps, [])
        event = self.bridge.snapshot()["events"][-1]
        self.assertEqual(event["reason"], "late_at_dispatch")
        self.assertGreater(event["lateness_ms"], 20)

    def test_invalid_batch_does_not_partially_schedule(self):
        identifier = self.observation()
        self.bridge.start()
        for bad in [{"lane": 8, "delay_ms": 0}, {"lane": "A", "delay_ms": -1},
                    {"lane": "A", "delay_ms": 1001}, {"lane": "A", "delay_ms": float("nan")},
                    {"lane": True, "delay_ms": 0}, {"lane": "A", "delay_ms": 0, "note_id": 3}]:
            with self.assertRaises(ValueError):
                self.bridge.submit(identifier, [{"lane": 0, "delay_ms": 0}, bad])
            self.assertEqual(self.bridge.snapshot()["pending_actions"], 0)
        with self.assertRaises(ValueError):
            self.bridge.submit("obs-unknown", [])

    def test_start_is_required_and_cannot_be_repeated(self):
        identifier = self.observation()
        with self.assertRaises(RuntimeError):
            self.bridge.submit(identifier, [{"lane": 0, "delay_ms": 0}])
        self.bridge.start()
        with self.assertRaises(RuntimeError):
            self.bridge.start()
        self.assertEqual(self.keys.starts, 1)

    def test_reader_failure_prevents_stale_cached_observation(self):
        self.observation()
        self.bridge._fail(RuntimeError("public frame channel disconnected"))
        with self.assertRaisesRegex(RuntimeError, "disconnected"):
            self.bridge.observe()
        self.assertIn("disconnected", self.bridge.snapshot()["error"])

    def test_complete_frame_shape_is_required(self):
        self.client.frames[0][0].width = 320
        with self.assertRaisesRegex(RuntimeError, "shape changed"):
            self.bridge.observe()
        with self.assertRaises(ValueError):
            rgba_png(PIXELS[:-1])

    def test_receipt_gap_statistics_measure_real_local_intervals(self):
        self.bridge._capture_once()
        for delta in [12_500_000, 17_000_000]:
            self.clock.now += delta
            self.client.frames.append((SimpleNamespace(width=640, height=360, stride=2560), PIXELS))
            self.bridge._capture_once()
        stats = self.bridge.snapshot()
        self.assertEqual(stats["frames_received"], 3)
        self.assertEqual(stats["receipt_gap_count"], 2)
        self.assertEqual(stats["receipt_gap_mean_ms"], 14.75)
        self.assertEqual(stats["receipt_gap_max_ms"], 17)
        self.assertEqual(stats["receipt_gaps_over_16ms"], 1)

    def test_close_cancels_pending_actions_and_export_is_deferred(self):
        identifier = self.observation()
        self.bridge.start()
        self.bridge.submit(identifier, [{"lane": "H", "delay_ms": 1000}])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(RuntimeError):
                self.bridge.export(directory)
            self.assertEqual(list(Path(directory).iterdir()), [])
            self.bridge.close()
            target = self.bridge.export(directory)
            data = json.loads(target.read_text())
            self.assertTrue(data["closed"])
            self.assertEqual(data["events"][-1]["reason"], "bridge_closed")
            self.assertEqual((Path(directory) / "observations" / (identifier + ".png")).read_bytes(), rgba_png(PIXELS))
        self.assertTrue(self.client.closed)
        self.assertTrue(self.keys.closed)
        self.assertEqual(self.keys.taps, [])

    def test_transport_test_mode_is_explicitly_labeled(self):
        client = FakeClient()
        bridge = NativeVisionBridge(2, client=client, background=False,
                                    clock_ns=self.clock, mode="transport-test")
        try:
            identifier = bridge.observe()["observation_id"]
            bridge.start()
            bridge.submit(identifier, [{"lane": "B", "delay_ms": 0}])
            bridge._dispatch_due()
            self.assertEqual(client.taps, [1])
            self.assertEqual(bridge.snapshot()["input_mode"], "transport-test")
        finally:
            bridge.close()


class FakeX11:
    def __init__(self):
        self.focus = 42

    def XGetInputFocus(self, display, focus, revert):
        C.cast(focus, C.POINTER(C.c_ulong))[0] = self.focus

    def XSync(self, *args):
        pass

    def XCloseDisplay(self, *args):
        pass

    def XInternAtom(self, *args):
        return 1

    def XFree(self, *args):
        pass


class FakeXTest:
    def __init__(self):
        self.events = []
        self.fail_down = False

    def XTestFakeKeyEvent(self, display, code, is_down, delay):
        self.events.append((code, is_down))
        return 0 if self.fail_down and is_down else 1


class KeyboardTests(unittest.TestCase):
    def setUp(self):
        import threading
        self.keys = object.__new__(XTestKeyboard)
        self.keys.display = 1
        self.keys.window = 42
        self.keys._lock = threading.Lock()
        self.keys._pressed = set()
        self.keys._codes = {"a": 38}
        self.keys.x11 = FakeX11()
        self.keys.xtst = FakeXTest()
        self.addCleanup(self.keys.close)

    def test_each_duplicate_has_down_and_up(self):
        self.keys.tap(0)
        self.keys.tap(0)
        self.assertEqual(self.keys.xtst.events, [(38, 1), (38, 0), (38, 1), (38, 0)])
        self.assertEqual(self.keys._pressed, set())

    def test_focus_loss_sends_no_events(self):
        self.keys.x11.focus = 17
        with self.assertRaisesRegex(RuntimeError, "lost keyboard focus"):
            self.keys.tap(0)
        self.assertEqual(self.keys.xtst.events, [])

    def test_failed_keydown_still_releases_key(self):
        self.keys.xtst.fail_down = True
        with self.assertRaisesRegex(RuntimeError, "keydown failed"):
            self.keys.tap(0)
        self.assertEqual(self.keys.xtst.events, [(38, 1), (38, 0)])
        self.assertEqual(self.keys._pressed, set())

    def test_stale_window_property_is_skipped_without_exiting(self):
        def vanished(*args):
            event = _XErrorEvent(display=1, resourceid=0x5000031,
                                 error_code=3, request_code=20)
            _record_x11_error(1, C.pointer(event))
            return 3
        self.keys.x11.XGetWindowProperty = vanished
        self.assertEqual(_property32(self.keys.x11, 1, 0x5000031, "_NET_WM_PID"), [])

    def test_focus_error_raises_before_any_keyboard_event(self):
        def vanished(*args):
            event = _XErrorEvent(display=1, resourceid=42,
                                 error_code=3, request_code=42)
            _record_x11_error(1, C.pointer(event))
            return 1
        self.keys.x11.XSetInputFocus = vanished
        with self.assertRaisesRegex(RuntimeError, "focus owned window failed"):
            self.keys.start()
        self.assertEqual(self.keys.xtst.events, [])

    def test_unexpected_property_protocol_error_is_not_silenced(self):
        def bad_value(*args):
            event = _XErrorEvent(display=1, resourceid=42,
                                 error_code=2, request_code=20)
            _record_x11_error(1, C.pointer(event))
            return 2
        self.keys.x11.XGetWindowProperty = bad_value
        with self.assertRaisesRegex(RuntimeError, "read window property failed"):
            _property32(self.keys.x11, 1, 42, "_NET_WM_PID")


if __name__ == "__main__":
    unittest.main()
