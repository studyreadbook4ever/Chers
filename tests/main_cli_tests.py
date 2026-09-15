#!/usr/bin/env python3
"""Subprocess integration checks for arguments, startup cleanup and early abort.

Usage: python3 tests/main_cli_tests.py build/chers
Successful startup checks require the benchmark's RDSEED-capable Linux host.
Every endpoint/output directory is isolated from ordinary running sessions.
"""

import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def environment(root):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("CHERS_", "CHRONOLANE_"))}
    env.update(CHERS_HEADLESS="1", CHERS_SOCKET=str(root / "bench.sock"),
               CHERS_OUTPUT=str(root / "runs"))
    return env


def run(binary, args, env, expected):
    result = subprocess.run([binary, *args], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=8)
    require(result.returncode == expected,
            f"{args}: exit {result.returncode}, expected {expected}: {result.stderr!r}")
    return result


def ready(process, timeout=8):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        readable, _, _ = select.select([process.stdout], [], [], 0.1)
        if readable:
            chunk = os.read(process.stdout.fileno(), 4096)
            if chunk:
                data.extend(chunk)
                if b"READY:" in data:
                    return bytes(data)
            elif process.poll() is not None:
                break
        if process.poll() is not None:
            break
    error = process.stderr.read() if process.poll() is not None else b"still running"
    raise AssertionError(f"startup did not reach READY: {bytes(data)!r} {error!r}")


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    try:
        return process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise AssertionError("shutdown hung after SIGTERM") from None


def main(binary):
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix="chers-cli-") as temporary:
        root = Path(temporary)
        env = environment(root)
        for args in ([], ["2"], ["2", "4", "extra"], ["1", "4"], ["17", "34"],
                     ["-2", "4"], ["+2", "4"], [" 2", "4"], ["2.0", "4"],
                     ["4294967296", "4"], ["2", "nan"], ["2", "inf"], ["2", "-inf"],
                     ["2", "3.999"], ["2", "48.001"], ["16", "31.999"],
                     ["16", "384.001"], ["2", "4x"], ["2", ""], ["2", "1e999"]):
            result = run(binary, args, env, 1)
            require(b"error" in result.stderr.lower(), f"no diagnostic for {args}")
            require(not (root / "bench.sock").exists(), "invalid arguments created an endpoint")
        require(b"Usage: chers LANES DENSITY" in run(binary, ["--help"], env, 0).stdout, "branded help missing")
        require(b"CHERS 0.2.0" in run(binary, ["--version"], env, 0).stdout, "branded version missing")

        # An existing ordinary file must never be silently unlinked by bind.
        endpoint = root / "bench.sock"
        endpoint.write_bytes(b"do not overwrite")
        result = run(binary, ["2", "4"], env, 1)
        require(b"bind" in result.stderr, "existing endpoint did not report bind failure")
        require(endpoint.read_bytes() == b"do not overwrite", "endpoint collision destroyed existing file")
        endpoint.unlink()

        for args in (["2", "4"], ["2", "48"], ["16", "32"], ["16", "384"]):
            process = subprocess.Popen([binary, *args], env=env, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE)
            try:
                ready(process)
                require(endpoint.exists(), "READY did not own its endpoint")
                # A competing benchmark must fail without disturbing the first.
                if args == ["2", "4"]:
                    run(binary, ["2", "4"], env, 1)
                    require(process.poll() is None and endpoint.exists(), "endpoint collision disturbed live server")
            finally:
                stop(process)
            require(process.returncode == 2, "stopping an unstarted benchmark must not report completed success")
            require(not endpoint.exists(), "endpoint leaked after normal shutdown")
            require(not (root / "runs").exists(), "unstarted benchmark wrote a trial result")

        # GUI initialization failure occurs after endpoint construction. It must
        # unwind the endpoint and return promptly, without entering worker loops.
        gui_env = dict(env, CHERS_HEADLESS="0", SDL_VIDEODRIVER="chers-no-such-driver")
        run(binary, ["2", "4"], gui_env, 1)
        require(not endpoint.exists(), "GUI startup failure leaked endpoint")

        # Exercise an actual started run for only 150 ms, then verify that an
        # interruption creates an explicitly incomplete, invalid audit artifact.
        active_env = dict(env, CHERS_AUTOSTART="1")
        process = subprocess.Popen([binary, "2", "4"], env=active_env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready(process)
            time.sleep(0.15)
        finally:
            stop(process)
        require(process.returncode == 2, "interrupted active trial reported success")
        reports = list((root / "runs").glob("*/result.json"))
        require(len(reports) == 1, f"expected exactly one abort report, got {reports}")
        result = json.loads(reports[0].read_text())
        require(result["completed"] is False and result["valid"] is False,
                "aborted trial was recorded as valid/complete")
        require("signal" in result["invalid_reason"], "abort cause missing from report")
        require(result["trial"]["notes"] > 0 and result["trial"]["perfect"] == 0,
                "aborted trial denominator or scoring is inconsistent")
        require(not endpoint.exists(), "aborted run leaked endpoint")
    print("CLI: invalid arguments, parameter boundaries, live/file endpoint collisions, "
          "GUI failure cleanup, signal shutdown, invalid abort artifacts passed")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/chers")
