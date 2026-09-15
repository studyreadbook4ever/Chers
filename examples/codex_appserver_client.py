#!/usr/bin/env python3
"""Minimal official Codex app-server client for local subscription evaluations.

Authentication stays in the installed Codex client. No tokens are read/exported.
The evaluated thread has no execution environment or installed capabilities;
the host supplies only explicitly declared benchmark tools.
"""
from __future__ import annotations

import json
import queue
import subprocess
import threading
import time


DISABLED_FEATURES = (
    "apps", "plugins", "hooks", "shell_tool", "unified_exec", "multi_agent",
    "browser_use", "browser_use_external", "computer_use", "image_generation",
    "in_app_browser", "memories", "shell_snapshot", "tool_suggest", "goals",
    "code_mode",
)


class AppServer:
    def __init__(self, stderr_file, *, codex="codex"):
        command = [codex, "app-server", "--stdio", "-c", 'web_search="disabled"',
                   "-c", "mcp_servers.openaiDeveloperDocs.enabled=false"]
        for feature in DISABLED_FEATURES:
            command += ["-c", f"features.{feature}=false"]
        self.command = command
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=stderr_file,
                                        text=True, bufsize=1)
        self.messages = queue.Queue()
        self._next_id = 1
        self._write_lock = threading.Lock()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        self.initialize = self.call("initialize", {
            "clientInfo": {"name": "chers_native_vision", "version": "0.1.0"},
            "capabilities": {"experimentalApi": True}})
        self.send({"method": "initialized"})

    def _read(self):
        try:
            for line in self.process.stdout:
                self.messages.put((time.monotonic_ns(), json.loads(line)))
        except Exception as exc:
            self.messages.put((time.monotonic_ns(), {"transport_error": str(exc)}))
        finally:
            self.messages.put((time.monotonic_ns(), {"transport_closed": True}))

    def send(self, value):
        with self._write_lock:
            self.process.stdin.write(json.dumps(value, separators=(",", ":")) + "\n")
            self.process.stdin.flush()

    def request(self, method, params):
        number = self._next_id
        self._next_id += 1
        self.send({"id": number, "method": method, "params": params})
        return number

    def receive(self, timeout=1):
        return self.messages.get(timeout=timeout)

    def respond(self, number, result):
        self.send({"id": number, "result": result})

    def call(self, method, params, timeout=30):
        number = self.request(method, params)
        deadline = time.monotonic() + timeout
        deferred = []
        try:
            while time.monotonic() < deadline:
                try:
                    received, msg = self.receive(min(1, max(.001, deadline-time.monotonic())))
                except queue.Empty:
                    continue
                if msg.get("transport_closed") or msg.get("transport_error"):
                    raise RuntimeError(f"app-server transport ended during {method}")
                if msg.get("id") == number and "method" not in msg:
                    if "error" in msg:
                        raise RuntimeError(json.dumps(msg["error"]))
                    return msg["result"]
                deferred.append((received, msg))
            raise TimeoutError(method)
        finally:
            for item in deferred:
                self.messages.put(item)

    def models(self):
        found, cursor = [], None
        while True:
            params = {"includeHidden": False, "limit": 100}
            if cursor:
                params["cursor"] = cursor
            page = self.call("model/list", params)
            found.extend(page["data"])
            cursor = page.get("nextCursor")
            if not cursor:
                return found

    def start_thread(self, model, effort, instructions, tools, cwd):
        result = self.call("thread/start", {
            "model": model, "modelProvider": "openai", "ephemeral": True,
            "serviceTier": "priority",
            "allowProviderModelFallback": False,
            "cwd": str(cwd), "environments": [], "selectedCapabilityRoots": [],
            "approvalPolicy": "never", "sandbox": "read-only",
            "baseInstructions": instructions,
            "developerInstructions": "Use only the supplied benchmark interface. "
                "Do not access files, shell, browsers, other models, or external state.",
            "dynamicTools": tools,
            "config": {"model_reasoning_effort": effort, "web_search": "disabled"},
        })
        if result["model"] != model:
            raise RuntimeError(f"Requested model {model} replaced by {result['model']}")
        if result.get("reasoningEffort") != effort:
            raise RuntimeError(f"Requested effort {effort} replaced by {result.get('reasoningEffort')}")
        return result

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
