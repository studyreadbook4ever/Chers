#!/usr/bin/env python3
"""Full-frame CHERS observations and explicitly selected keyboard actions.

This adapter performs no note detection, OCR, timing inference, or model calls.
Observation IDs and receipt timestamps belong to this process, not the game.
Only the public pixel client is read. Export evidence after the trial ends.
"""

import base64
import ctypes as C
import ctypes.util
import heapq
import json
import math
from pathlib import Path
import struct
import threading
import time
import zlib

from python_client import Client


class _XErrorEvent(C.Structure):
    _fields_ = [("type", C.c_int), ("display", C.c_void_p),
                ("resourceid", C.c_ulong), ("serial", C.c_ulong),
                ("error_code", C.c_ubyte), ("request_code", C.c_ubyte),
                ("minor_code", C.c_ubyte)]


_X_PROTOCOL_ERRORS = threading.local()
_XErrorHandler = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(_XErrorEvent))


@_XErrorHandler
def _record_x11_error(display, error):
    """Retained for process lifetime: Xlib must never call a freed callback."""
    event = error.contents
    errors = getattr(_X_PROTOCOL_ERRORS, "errors", [])
    errors.append((int(display or 0), int(event.error_code), int(event.request_code),
                   int(event.resourceid)))
    _X_PROTOCOL_ERRORS.errors = errors[-32:]
    return 0


def _checked_x11(x11, display, operation, call, *, missing_window_ok=False):
    """Convert X protocol errors to exceptions, including asynchronous replies.

    The callback is process-wide, while records are per calling thread. XSync
    drains this connection's replies before inspecting errors. A stale member
    of _NET_CLIENT_LIST is expected and can be skipped by the property reader.
    """
    _X_PROTOCOL_ERRORS.errors = []
    try:
        result = call()
    finally:
        # In particular, flush a keyup even if its preceding keydown failed.
        x11.XSync(display, 0)
    errors = getattr(_X_PROTOCOL_ERRORS, "errors", [])
    if errors:
        if missing_window_ok and all(error[1] == 3 for error in errors):
            return None
        _, code, request, resource = errors[0]
        raise RuntimeError(f"X11 {operation} failed: error={code}, request={request}, window={resource:#x}")
    return result


def _configure_x11_properties(x11):
    # Xlib's default handler exits the whole Python process on BadWindow.
    # Keep our callback installed so disappearing desktop windows are handled.
    x11.XSetErrorHandler.argtypes = [_XErrorHandler]
    x11.XSetErrorHandler.restype = C.c_void_p
    x11.XSetErrorHandler(_record_x11_error)
    x11.XSync.argtypes = [C.c_void_p, C.c_int]
    x11.XInternAtom.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
    x11.XInternAtom.restype = C.c_ulong
    x11.XGetWindowProperty.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong,
                                     C.c_long, C.c_long, C.c_int, C.c_ulong,
                                     C.POINTER(C.c_ulong), C.POINTER(C.c_int),
                                     C.POINTER(C.c_ulong), C.POINTER(C.c_ulong),
                                     C.POINTER(C.c_void_p)]
    x11.XGetWindowProperty.restype = C.c_int
    x11.XFree.argtypes = [C.c_void_p]


def _property32(x11, display, window, name):
    atom = x11.XInternAtom(display, name.encode("ascii"), 1)
    if not atom:
        return []
    actual, count, remaining = C.c_ulong(), C.c_ulong(), C.c_ulong()
    fmt, data = C.c_int(), C.c_void_p()
    try:
        result = _checked_x11(x11, display, "read window property",
                             lambda: x11.XGetWindowProperty(
                                 display, window, atom, 0, 4096, 0, 0,
                                 C.byref(actual), C.byref(fmt), C.byref(count),
                                 C.byref(remaining), C.byref(data)),
                             missing_window_ok=True)
        if result != 0 or fmt.value != 32 or not data:
            return []
        return list(C.cast(data, C.POINTER(C.c_ulong))[:count.value])
    finally:
        if data:
            x11.XFree(data)


def find_window_for_pid(pid, timeout=10):
    """Find a window through public X11 properties for the supplied owned PID."""
    if isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0:
        raise ValueError("an owned process PID is required")
    x11 = C.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
    x11.XInitThreads.argtypes = []
    x11.XInitThreads.restype = C.c_int
    if not x11.XInitThreads():
        raise RuntimeError("XInitThreads failed")
    x11.XOpenDisplay.argtypes = [C.c_char_p]
    x11.XOpenDisplay.restype = C.c_void_p
    x11.XCloseDisplay.argtypes = [C.c_void_p]
    x11.XDefaultRootWindow.argtypes = [C.c_void_p]
    x11.XDefaultRootWindow.restype = C.c_ulong
    _configure_x11_properties(x11)
    display = x11.XOpenDisplay(None)
    if not display:
        raise RuntimeError("X11 display unavailable")
    deadline = time.monotonic() + timeout
    try:
        root = x11.XDefaultRootWindow(display)
        while True:
            windows = _property32(x11, display, root, "_NET_CLIENT_LIST")
            for window in windows:
                owners = _property32(x11, display, window, "_NET_WM_PID")
                if owners == [pid]:
                    return window
            if time.monotonic() >= deadline:
                raise TimeoutError("owned CHERS process has no visible X11 window")
            time.sleep(0.05)
    finally:
        x11.XCloseDisplay(display)


def rgba_png(rgba):
    """Losslessly encode the entire fixed-size observation without cropping."""
    width, height = 640, 360
    if len(rgba) != width * height * 4:
        raise ValueError("expected a complete 640x360 RGBA observation")

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

    rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4]
                    for y in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, level=1)) + chunk(b"IEND", b""))


class XTestKeyboard:
    """Discrete X11 key presses, restricted to one explicitly supplied window.

    XTest injects through the desktop keyboard path; it is not a physical USB
    keyboard. No window discovery or arbitrary desktop navigation is performed.
    Losing focus fails closed instead of typing into another application.
    """

    def __init__(self, window_id, expected_pid=None):
        if isinstance(window_id, bool) or not isinstance(window_id, int) or window_id <= 0:
            raise ValueError("an owned CHERS X11 window ID is required")
        self.window = window_id
        self._lock = threading.Lock()
        self._pressed = set()
        self.x11 = C.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        self.xtst = C.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")
        self.x11.XInitThreads.argtypes = []
        self.x11.XInitThreads.restype = C.c_int
        if not self.x11.XInitThreads():
            raise RuntimeError("XInitThreads failed")
        self.x11.XOpenDisplay.argtypes = [C.c_char_p]
        self.x11.XOpenDisplay.restype = C.c_void_p
        self.x11.XCloseDisplay.argtypes = [C.c_void_p]
        self.x11.XFetchName.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_void_p)]
        self.x11.XFetchName.restype = C.c_int
        self.x11.XFree.argtypes = [C.c_void_p]
        self.x11.XSetInputFocus.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_ulong]
        self.x11.XGetInputFocus.argtypes = [C.c_void_p, C.POINTER(C.c_ulong), C.POINTER(C.c_int)]
        self.x11.XStringToKeysym.argtypes = [C.c_char_p]
        self.x11.XStringToKeysym.restype = C.c_ulong
        self.x11.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
        self.x11.XKeysymToKeycode.restype = C.c_ubyte
        self.x11.XSync.argtypes = [C.c_void_p, C.c_int]
        _configure_x11_properties(self.x11)
        self.xtst.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.xtst.XTestFakeKeyEvent.restype = C.c_int
        self.display = self.x11.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("X11 display unavailable")
        try:
            name = C.c_void_p()
            fetched = _checked_x11(self.x11, self.display, "read owned window title",
                                   lambda: self.x11.XFetchName(self.display, self.window, C.byref(name)))
            if not fetched or not name:
                raise RuntimeError("cannot verify the supplied CHERS window")
            try:
                title = C.string_at(name).decode("utf-8", errors="replace")
            finally:
                self.x11.XFree(name)
            if not title.startswith("CHERS"):
                raise RuntimeError("supplied window is not titled CHERS")
            if expected_pid is not None:
                owners = _property32(self.x11, self.display, self.window, "_NET_WM_PID")
                if owners != [expected_pid]:
                    raise RuntimeError("supplied CHERS window is not owned by the expected process")
            self._codes = {}
            for key in ["space"] + list("abcdefghijklmnop"):
                code = self.x11.XKeysymToKeycode(self.display, self.x11.XStringToKeysym(key.encode()))
                if not code:
                    raise RuntimeError("keyboard mapping unavailable: " + key)
                self._codes[key] = int(code)
        except BaseException:
            self.x11.XCloseDisplay(self.display)
            self.display = None
            raise

    def _assert_focus(self):
        current, revert = C.c_ulong(), C.c_int()
        _checked_x11(self.x11, self.display, "check keyboard focus",
                     lambda: self.x11.XGetInputFocus(self.display, C.byref(current), C.byref(revert)))
        if current.value != self.window:
            raise RuntimeError("owned CHERS window lost keyboard focus")

    def _press(self, key):
        if not self.display:
            raise RuntimeError("keyboard is closed")
        self._assert_focus()
        code = self._codes[key]
        def pair():
            self._pressed.add(code)
            try:
                if not self.xtst.XTestFakeKeyEvent(self.display, code, 1, 0):
                    raise RuntimeError("XTest keydown failed")
            finally:
                released = self.xtst.XTestFakeKeyEvent(self.display, code, 0, 0)
                if released:
                    self._pressed.discard(code)
                else:
                    raise RuntimeError("XTest keyup failed")
        _checked_x11(self.x11, self.display, "send keyboard pair", pair)

    def start(self):
        with self._lock:
            _checked_x11(self.x11, self.display, "focus owned window",
                         lambda: self.x11.XSetInputFocus(self.display, self.window, 2, 0))
            self._press("space")

    def tap(self, lane):
        with self._lock:
            self._press(chr(ord("a") + lane))

    def close(self):
        with self._lock:
            if self.display:
                for code in self._pressed:
                    self.xtst.XTestFakeKeyEvent(self.display, code, 0, 0)
                self.x11.XSync(self.display, 0)
                self._pressed.clear()
                self.x11.XCloseDisplay(self.display)
                self.display = None


class _TransportKeys:
    """Test-only alternative. Production evidence must use XTestKeyboard."""

    def __init__(self, client):
        self.client = client

    def tap(self, lane):
        self.client.tap(lane)

    def start(self):
        self.client.start()

    def close(self):
        pass


class NativeVisionBridge:
    LATE_NS = 20_000_000
    MAX_ACTIONS = 128
    MAX_PENDING = 8192
    # One complete 61.040 s trial at 80 fps needs about 4,884 observations.
    # Leave room for ready-screen frames and final-result inspection.
    MAX_OBSERVATIONS = 8192
    MAX_PNG_BYTES = 128 * 1024 * 1024
    MAX_TOTAL_ACTIONS = 32768

    def __init__(self, lanes, window_id=None, endpoint=None, library=None, *,
                 client=None, key_sink=None, clock_ns=time.monotonic_ns,
                 background=True, mode="keyboard", expected_pid=None):
        if isinstance(lanes, bool) or not isinstance(lanes, int) or not 2 <= lanes <= 16:
            raise ValueError("lanes must be an integer from 2 through 16")
        if mode not in ("keyboard", "transport-test"):
            raise ValueError("mode must be keyboard or transport-test")
        self.lanes = lanes
        self._clock = clock_ns
        self._client = client if client is not None else Client(endpoint, library)
        try:
            self._keys = key_sink if key_sink is not None else (
                _TransportKeys(self._client) if mode == "transport-test"
                else XTestKeyboard(window_id, expected_pid=expected_pid))
        except BaseException:
            self._client.close()
            raise
        self.mode = "injected-test" if key_sink is not None else mode
        self.window_id = window_id
        self._cv = threading.Condition(threading.RLock())
        self._stop = threading.Event()
        self._latest = None
        self._last_returned_frame = 0
        self._observations = {}
        self._png_bytes = 0
        self._pending = []
        self._events = []
        self._error = None
        self._closed = False
        self._started = False
        self._next_action = 0
        self._frames = 0
        self._previous_receipt = None
        self._gap_sum_ns = 0
        self._gap_max_ns = 0
        self._gaps_over_16ms = 0
        self._threads = []
        self._background = background
        if background:
            self._threads = [threading.Thread(target=self._capture_loop, name="chers-public-pixels", daemon=True),
                             threading.Thread(target=self._schedule_loop, name="chers-keyboard", daemon=True)]
            for thread in self._threads:
                thread.start()

    def _check(self):
        if self._error:
            raise RuntimeError("native vision bridge failed: " + self._error)
        if self._closed:
            raise RuntimeError("native vision bridge is closed")

    def _fail(self, error):
        with self._cv:
            if self._error is None:
                self._error = str(error)
                self._events.append({"type": "error", "local_ns": self._clock(), "message": str(error)})
            self._stop.set()
            self._cv.notify_all()

    def _capture_once(self):
        value = self._client.frame(timeout_ms=100)
        if value is None:
            return False
        info, rgba = value
        receipt = self._clock()
        if (info.width, info.height, info.stride) != (640, 360, 2560):
            raise RuntimeError("public frame shape changed")
        pixels = bytes(rgba)
        if len(pixels) != 640 * 360 * 4:
            raise RuntimeError("incomplete public frame")
        with self._cv:
            if self._previous_receipt is not None:
                gap = receipt - self._previous_receipt
                if gap < 0:
                    raise RuntimeError("local monotonic clock moved backwards")
                self._gap_sum_ns += gap
                self._gap_max_ns = max(self._gap_max_ns, gap)
                self._gaps_over_16ms += gap > 16_000_000
            self._frames += 1
            self._previous_receipt = receipt
            self._latest = (self._frames, receipt, pixels)
            self._cv.notify_all()
        return True

    def _capture_loop(self):
        try:
            while not self._stop.is_set():
                self._capture_once()
        except Exception as error:
            self._fail(error)

    def observe(self, timeout=2.0):
        """Return a newly captured frame as a local ID and complete PNG data URL.

        Fast polling waits for the next public frame instead of repeatedly
        packaging one cached frame. The game and capture thread keep running.
        """
        deadline = time.monotonic() + timeout
        with self._cv:
            self._check()
        if not self._background:
            self._capture_once()
        with self._cv:
            while self._latest is None or self._latest[0] <= self._last_returned_frame:
                self._check()
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("no new public frame received")
                self._cv.wait(remaining)
            self._check()
            frame, receipt, rgba = self._latest
            self._last_returned_frame = frame
        png = rgba_png(rgba)
        with self._cv:
            self._check()
            if (len(self._observations) >= self.MAX_OBSERVATIONS
                    or self._png_bytes + len(png) > self.MAX_PNG_BYTES):
                raise RuntimeError("observation evidence capacity exceeded")
            identifier = f"obs-{len(self._observations) + 1:06d}"
            self._observations[identifier] = {"receipt_ns": receipt, "png": png}
            self._png_bytes += len(png)
            self._events.append({"type": "observation", "observation_id": identifier,
                                 "local_receipt_ns": receipt, "local_return_ns": self._clock()})
        return {"observation_id": identifier,
                "image_url": "data:image/png;base64," + base64.b64encode(png).decode("ascii")}

    def start(self):
        with self._cv:
            self._check()
            if self._started:
                raise RuntimeError("START may only be sent once")
            self._keys.start()
            self._started = True
            self._events.append({"type": "start", "local_ns": self._clock()})

    def submit(self, observation_id, actions):
        """Schedule explicit model decisions against the named local receipt.

        Each action is {'lane': 'A', 'delay_ms': 123.0}; lane integers are also
        accepted (A=0). Every duplicate is a separate keydown/keyup pair.
        The entire batch is validated before any action is queued.
        """
        if not isinstance(actions, list) or len(actions) > self.MAX_ACTIONS:
            raise ValueError("actions must be a list of at most 128 explicit taps")
        validated = []
        for action in actions:
            if not isinstance(action, dict) or set(action) != {"lane", "delay_ms"}:
                raise ValueError("each action must contain exactly lane and delay_ms")
            lane, delay = action["lane"], action["delay_ms"]
            if isinstance(lane, str) and len(lane) == 1:
                lane = ord(lane.upper()) - ord("A")
            if isinstance(lane, bool) or not isinstance(lane, int) or not 0 <= lane < self.lanes:
                raise ValueError("lane is outside this workload")
            if (isinstance(delay, bool) or not isinstance(delay, (int, float))
                    or not math.isfinite(delay) or not 0 <= delay <= 1000):
                raise ValueError("delay_ms must be finite and between 0 and 1000")
            validated.append((lane, round(delay * 1_000_000)))
        with self._cv:
            self._check()
            if not self._started:
                raise RuntimeError("START is required before actions")
            if observation_id not in self._observations:
                raise ValueError("unknown local observation ID")
            if len(self._pending) + len(validated) > self.MAX_PENDING:
                raise RuntimeError("action queue capacity exceeded")
            if self._next_action + len(validated) > self.MAX_TOTAL_ACTIONS:
                raise RuntimeError("action evidence capacity exceeded")
            receipt = self._observations[observation_id]["receipt_ns"]
            now = self._clock()
            scheduled = dropped = 0
            for lane, delay in validated:
                due = receipt + delay
                self._next_action += 1
                action_id = self._next_action
                self._events.append({"type": "action_proposed", "action_id": action_id,
                                     "observation_id": observation_id, "lane": lane,
                                     "delay_ms": delay / 1_000_000, "local_due_ns": due,
                                     "local_submit_ns": now})
                if now - due > self.LATE_NS:
                    self._events.append({"type": "action_dropped", "action_id": action_id,
                                         "reason": "late_at_submission", "lateness_ms": (now - due) / 1_000_000})
                    dropped += 1
                else:
                    heapq.heappush(self._pending, (due, action_id, lane))
                    scheduled += 1
            self._cv.notify_all()
        return {"accepted": len(validated), "scheduled": scheduled, "dropped_late": dropped}

    def _dispatch_due(self):
        """Dispatch ready actions; separated for deterministic clock tests."""
        with self._cv:
            self._check()
            while self._pending and self._pending[0][0] <= self._clock():
                due, action_id, lane = heapq.heappop(self._pending)
                now = self._clock()
                if now - due > self.LATE_NS:
                    self._events.append({"type": "action_dropped", "action_id": action_id,
                                         "reason": "late_at_dispatch", "lateness_ms": (now - due) / 1_000_000})
                    continue
                self._keys.tap(lane)
                self._events.append({"type": "action_sent", "action_id": action_id,
                                     "lane": lane, "local_send_ns": now,
                                     "local_return_ns": self._clock(), "lateness_ms": (now - due) / 1_000_000})

    def _schedule_loop(self):
        try:
            while not self._stop.is_set():
                self._dispatch_due()
                with self._cv:
                    if self._stop.is_set():
                        break
                    wait = max(0, (self._pending[0][0] - self._clock()) / 1e9) if self._pending else 0.1
                    self._cv.wait(min(wait, 0.1))
        except Exception as error:
            self._fail(error)

    def snapshot(self):
        with self._cv:
            counts = {}
            for event in self._events:
                counts[event["type"]] = counts.get(event["type"], 0) + 1
            return {"adapter": "native-full-frame-vision-explicit-keyboard-v2",
                    "input_mode": self.mode, "window_id": self.window_id,
                    "lanes": self.lanes, "frame_shape": [640, 360, 4],
                    "preprocessing": "lossless PNG encoding only; no image feature extraction",
                    "scheduler": {"reference": "local observation receipt", "max_delay_ms": 1000,
                                  "drop_when_late_over_ms": 20, "duplicates": "each dispatched separately"},
                    "frames_received": self._frames,
                    "receipt_gap_scope": "entire connected capture session; not scored-window compliance",
                    "receipt_gap_count": max(0, self._frames - 1),
                    "receipt_gap_mean_ms": self._gap_sum_ns / max(1, self._frames - 1) / 1_000_000,
                    "receipt_gap_max_ms": self._gap_max_ns / 1_000_000,
                    "receipt_gaps_over_16ms": self._gaps_over_16ms,
                    "observations_returned": len(self._observations), "pending_actions": len(self._pending),
                    "started": self._started, "closed": self._closed, "error": self._error,
                    "counts": counts, "events": [dict(event) for event in self._events]}

    def close(self):
        with self._cv:
            if self._closed:
                return
            self._stop.set()
            self._cv.notify_all()
        for thread in self._threads:
            thread.join(timeout=2.0)
        if any(thread.is_alive() for thread in self._threads):
            self._keys.close()
            raise RuntimeError("bridge worker did not stop; client handle remains open")
        with self._cv:
            while self._pending:
                _, action_id, _ = heapq.heappop(self._pending)
                self._events.append({"type": "action_dropped", "action_id": action_id, "reason": "bridge_closed"})
            try:
                self._keys.close()
            finally:
                self._client.close()
                self._closed = True

    def export(self, directory):
        """Write evidence only after close; never read benchmark result files."""
        if not self._closed:
            raise RuntimeError("close the bridge before exporting evidence")
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        frames = directory / "observations"
        frames.mkdir(exist_ok=True)
        for identifier, observation in self._observations.items():
            (frames / (identifier + ".png")).write_bytes(observation["png"])
        target = directory / "bridge.json"
        target.write_text(json.dumps(self.snapshot(), indent=2) + "\n", encoding="utf-8")
        return target

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
