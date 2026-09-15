#!/usr/bin/env python3
"""Run documented Codex native-image / XTest keyboard CHERS pilots.

Uses the installed official app-server and its existing ChatGPT sign-in.
Only the supervisor launches the game and reads results, after the game exits.
Evaluated threads receive full images and the public rules, never result paths.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples"))
from codex_appserver_client import AppServer
from codex_vision_bridge import NativeVisionBridge, find_window_for_pid, rgba_png


def instructions(lanes):
    return f"""You are the sole visual controller being evaluated on CHERS, a real-time rhythm game.
This is an execution task. Play until the host says SESSION COMPLETE. Use only chers_step.
You receive native complete 640x360 game images, without any visual preprocessing or OCR.
There are {lanes} equal vertical lanes, left to right {','.join(chr(65+i) for i in range(lanes))}.
Cyan notes with bright white centers move downward toward the horizontal judgement line.
Each note is visible for one second before its target time. Timing is random, not periodic.
Same-lane target spacing is at least 40 ms. There is no audio and no global game clock.
You may infer position, speed, and constant latency from images and on-screen hit feedback.
The trial has 5 seconds calibration, 5 seconds wait, 50 seconds scored play, plus initial preview
and 20 ms final judgement allowances. Calibration is excluded from scoring.
Perfect inclusive +/-8 ms earns 2; GOOD through +/-20 ms earns 1; every empty tap costs 5 and adds
100 ms to delaySum. Misses earn 0. Each hit consumes one note. Duplicates are judged separately.
Your objective is high score and low successful-hit error; indiscriminate tapping is penalized.
First call chers_step with actions=[] when ready; this starts the clock.
Each chers_step returns a fresh full image. Make calls sequentially, never in parallel.
To act on the most recent returned image call chers_step with actions, each with lane
and delay_ms. delay_ms is the predicted keypress time RELATIVE TO THAT IMAGE'S LOCAL RECEIPT,
NOT relative to the time you finish responding. Range 0..1000 ms. You may batch multiple taps.
The host schedules only your explicit choices and injects separate XTest keydown/keyup events
through Linux/SDL. It does not detect notes or choose targets. Requests more than 20 ms past
their requested deadline are dropped; the host will report the number dropped.
An empty actions list only observes again. Keep using chers_step, including during waits.
Do not give explanations while playing; respond with the tool call promptly. Never use another
tool, access files, query the game state, or delegate decisions. The host runs the real game
continuously while you infer. When SESSION COMPLETE is returned, finish briefly.
"""


def tool_spec(lanes):
    return [{"type": "function", "name": "chers_step",
        "description": "Schedule your explicit lane taps against the most recent returned image's local receipt, then observe a fresh full game image. First empty call starts the game. Call sequentially.",
        "inputSchema": {"type": "object", "additionalProperties": False,
            "properties": {
                "actions": {"type": "array", "maxItems": 128, "items": {
                    "type": "object", "additionalProperties": False,
                    "properties": {"lane": {"type": "string", "enum": [chr(65+i) for i in range(lanes)]},
                        "delay_ms": {"type": "number", "minimum": 0, "maximum": 1000}},
                    "required": ["lane", "delay_ms"]}}},
            "required": ["actions"]}}]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def safe_event(value):
    """Avoid duplicating image data in event logs; this is not a secret scrubber."""
    if isinstance(value, str) and value.startswith("data:image/"):
        return {"image_data_omitted": True, "data_url_sha256": hashlib.sha256(value.encode()).hexdigest()}
    if isinstance(value, list):
        return [safe_event(v) for v in value]
    if isinstance(value, dict):
        return {k: safe_event(v) for k, v in value.items()}
    return value


def render_vision_challenge():
    """Create the visual connection fixture; Pillow is an optional runner dependency."""
    # Pillow is used only for a legible connection-test fixture, never on game pixels.
    from PIL import Image, ImageDraw, ImageFont
    answer = "".join(str(b % 10) for b in os.urandom(5))
    font_candidates = [Path("/usr/share/fonts/TTF/DejaVuSansMono.ttf"),
                       Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")]
    font_path = next((p for p in font_candidates if p.exists()), None)
    if font_path is None:
        raise RuntimeError("install DejaVu Sans Mono for the native-image preflight")
    image = Image.new("RGBA", (640,360), (0,0,0,255))
    ImageDraw.Draw(image).text((90,100), answer, font=ImageFont.truetype(str(font_path),120), fill="white")
    png = rgba_png(image.tobytes())
    return answer, png


def probe_vision(server, model, cwd, output):
    """Native image acceptance and transcription, independent of game scoring."""
    answer, png = render_vision_challenge()
    output.mkdir(parents=True, exist_ok=False)
    (output/"image.png").write_bytes(png)
    record = {"requested_model": model, "effort": "low", "expected": answer,
              "png_sha256": hashlib.sha256(png).hexdigest(), "passed": False}
    tid = turn_id = None
    try:
        probe_tool = [{"type":"function", "name":"vision_probe", "description":"Return the native test image.",
                       "inputSchema":{"type":"object","properties":{},"additionalProperties":False}}]
        info = server.start_thread(model, "low", "Call vision_probe to receive an image. Read that image and reply only with the five digits shown. Do not use any other tool.", probe_tool, cwd)
        record["thread_settings"] = {k:v for k,v in info.items() if k != "thread"}
        tid = info["thread"]["id"]
        started = time.monotonic()
        turn = server.call("turn/start", {"threadId": tid, "effort": "low", "input": [
            {"type": "text", "text": "Use vision_probe and transcribe its image."}]})
        turn_id = turn["turn"]["id"]
        record["image_route"] = "dynamic tool response contentItems.inputImage (same as gameplay)"
        record["tool_calls"] = []
        texts = []
        while time.monotonic()-started < 90:
            try:
                _, msg = server.receive(1)
            except queue.Empty:
                continue
            if msg.get("transport_closed") or msg.get("transport_error"):
                raise RuntimeError("app-server transport lost during image preflight")
            if msg.get("params", {}).get("threadId") != tid:
                continue
            if msg.get("method") == "item/tool/call":
                if msg["params"].get("tool") != "vision_probe":
                    raise RuntimeError("unexpected preflight tool")
                record["tool_calls"].append({"tool":"vision_probe","arguments":msg["params"]["arguments"]})
                server.respond(msg["id"], {"success":True,"contentItems":[
                    {"type":"inputImage","imageUrl":"data:image/png;base64,"+base64.b64encode(png).decode()}]})
            if msg.get("method") == "item/completed":
                item = msg["params"]["item"]
                if item.get("type") == "agentMessage":
                    texts.append(item.get("text", ""))
            if msg.get("method") == "error":
                record["error"] = msg["params"].get("error")
            if msg.get("method") == "turn/completed":
                record["turn_status"] = msg["params"]["turn"]["status"]
                break
        record.update(response="\n".join(texts), duration_ms=(time.monotonic()-started)*1000)
        record["passed"] = (record.get("turn_status") == "completed" and bool(record["tool_calls"])
                            and record["response"].strip() == answer)
    except Exception as exc:
        record["error"] = str(exc)
    finally:
        if tid and turn_id and "turn_status" not in record:
            try:
                server.call("turn/interrupt", {"threadId":tid,"turnId":turn_id}, timeout=5)
            except Exception as exc:
                record["interrupt_error"] = str(exc)
    write_json(output/"result.json", record)
    return record


def run_trial(server, model, effort, cwd, output, lanes=8, density=20):
    output.mkdir(parents=True, exist_ok=False)
    prompt = instructions(lanes)
    (output/"prompt.txt").write_text(prompt)
    write_json(output/"tool-schema.json", tool_spec(lanes))
    report = {"requested_model": model, "effort": effort, "lanes": lanes, "density": density,
              "evaluation_type": "native-full-frame-vision-with-XTest-keyboard",
              "status": "setup", "started_utc": time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              "prompt_sha256": hashlib.sha256(prompt.encode()).hexdigest(),
              "model_decisions": [], "model_errors": [], "turns": [], "continuation_prompts": 0}
    game = bridge = None
    events = []
    last_observation = None
    start_ns = None
    tid = turn_id = None
    game_log = (output/"game.log").open("w")
    try:
        info = server.start_thread(model, effort, prompt, tool_spec(lanes), cwd)
        report["thread_settings"] = {k:v for k,v in info.items() if k != "thread"}
        tid = info["thread"]["id"]
        sock = str(Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp"))/f"chers-codex-{os.getpid()}.sock")
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(("CHERS_", "CHRONOLANE_")):
                del env[key]
        env.update(CHERS_SOCKET=sock, CHERS_OUTPUT=str(output/"benchmark"), CHERS_EXIT_AFTER_RUN="1")
        game = subprocess.Popen([str(ROOT/"build/chers"), str(lanes), str(density)],
                                cwd=ROOT, env=env, stdout=game_log, stderr=subprocess.STDOUT)
        window = find_window_for_pid(game.pid)
        bridge = NativeVisionBridge(lanes, window, endpoint=sock,
                    library=str(ROOT/"build/libchers_client.so"), expected_pid=game.pid)
        ready_deadline = time.monotonic()+120
        turn = server.call("turn/start", {"threadId": tid, "effort": effort,
            "input": [{"type": "text", "text": "The game is ready. Call chers_step with empty actions to START, then play continuously using the full images."}]})
        turn_id = turn["turn"]["id"]
        report["turns"].append({"id": turn_id, "started_local_ns": time.monotonic_ns()})
        report["status"] = "waiting_for_start"
        while True:
            if game.poll() is not None:
                report["game_exit"] = game.returncode
                break
            now = time.monotonic_ns()
            bridge_error = bridge.snapshot()["error"]
            if bridge_error and (start_ns is None or now-start_ns < 61_040_000_000):
                raise RuntimeError("bridge failed before trial completion: "+bridge_error)
            if start_ns is None and time.monotonic() > ready_deadline:
                raise TimeoutError("model did not start within120seconds")
            if start_ns is not None and now-start_ns > 66_000_000_000:
                raise TimeoutError("game did not finish within66seconds of START")
            try:
                received_ns, msg = server.receive(.1)
            except queue.Empty:
                continue
            if msg.get("transport_closed") or msg.get("transport_error"):
                raise RuntimeError("app-server transport lost")
            params = msg.get("params", {})
            if params.get("threadId") != tid:
                continue
            method = msg.get("method")
            if method in ("item/completed", "error", "turn/completed", "thread/tokenUsage/updated"):
                if method != "item/completed" or params.get("item", {}).get("type") != "reasoning":
                    events.append({"received_local_ns": received_ns, **safe_event(msg)})
            if method == "item/tool/call":
                if params.get("tool") != "chers_step":
                    raise RuntimeError("unexpected tool requested: "+str(params.get("tool")))
                args = params["arguments"]
                decision = {"received_local_ns": received_ns, "arguments": args}
                try:
                    if isinstance(args, str):
                        args = json.loads(args)
                    if not isinstance(args, dict):
                        raise ValueError("arguments must be a JSON object")
                    if last_observation is not None and received_ns < last_observation["return_ns"]:
                        raise ValueError("overlapping chers_step calls are not allowed; wait for the returned image")
                    if last_observation is not None:
                        decision["reference_observation_id"] = last_observation["observation_id"]
                        decision["observation_to_tool_ms"] = (received_ns-last_observation["receipt_ns"])/1e6
                    if set(args) != {"actions"}:
                        raise ValueError("expected exactly actions")
                    if start_ns is None:
                        if args != {"actions": []}:
                            raise ValueError("first call must use empty actions")
                        bridge.start()
                        start_ns = time.monotonic_ns()
                        report["start_key_return_local_ns"] = start_ns
                        report["status"] = "running"
                        # Let the capture worker receive a post-START frame.
                        time.sleep(.025)
                        outcome = {"accepted": 0, "scheduled": 0, "dropped_late": 0}
                    else:
                        outcome = bridge.submit(last_observation["observation_id"], args["actions"])
                    observation = bridge.observe()
                    bridge_events = bridge.snapshot()["events"]
                    receipt = next(e["local_receipt_ns"] for e in reversed(bridge_events)
                                   if e["type"] == "observation" and e["observation_id"] == observation["observation_id"])
                    last_observation = {"observation_id": observation["observation_id"], "receipt_ns": receipt,
                                        "return_ns": time.monotonic_ns()}
                    decision["outcome"] = outcome
                    decision["returned_observation_id"] = observation["observation_id"]
                    response = {"success": True, "contentItems": [
                        {"type": "inputText", "text": json.dumps({"observation_id": observation["observation_id"], "previous_actions": outcome})},
                        {"type": "inputImage", "imageUrl": observation["image_url"]}]}
                except (ValueError, TypeError) as exc:
                    decision["invalid_arguments"] = str(exc)
                    response = {"success": False, "contentItems": [{"type": "inputText", "text": str(exc)}]}
                server.respond(msg["id"], response)
                if "returned_observation_id" in decision:
                    last_observation["return_ns"] = time.monotonic_ns()
                report["model_decisions"].append(decision)
                print(json.dumps({"model":model,"effort":effort,"decisions":len(report["model_decisions"]),
                                  "outcome":decision.get("outcome"),"latency_ms":decision.get("observation_to_tool_ms")}), flush=True)
            elif method == "error":
                report["model_errors"].append(params.get("error"))
            elif method == "thread/tokenUsage/updated":
                report["token_usage"] = params.get("tokenUsage")
            elif method == "turn/completed":
                status = params["turn"]["status"]
                report["turns"][-1].update(status=status, ended_local_ns=received_ns)
                if status == "completed" and game.poll() is None:
                    report["continuation_prompts"] += 1
                    follow = server.call("turn/start", {"threadId":tid,"effort":effort,
                        "input":[{"type":"text","text":"Continue playing. The game is still running; use chers_step until the host ends the session."}]})
                    turn_id = follow["turn"]["id"]
                    report["turns"].append({"id":turn_id,"started_local_ns":time.monotonic_ns()})
                elif status == "failed":
                    raise RuntimeError("model turn failed; see recorded error")
            elif "id" in msg:
                raise RuntimeError("unexpected server request: "+str(method))
        report["status"] = "completed" if report.get("game_exit") == 0 and not report["model_errors"] else "failed"
    except Exception as exc:
        report["status"] = "failed"
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if tid and turn_id:
            try:
                server.call("turn/interrupt", {"threadId":tid,"turnId":turn_id}, timeout=5)
            except Exception:
                pass
        if bridge:
            try:
                bridge.close()
                bridge.export(output/"agent")
            except Exception as exc:
                report.setdefault("cleanup_errors", []).append("bridge: "+str(exc))
                report["status"] = "failed"
        if game and game.poll() is None:
            try:
                game.terminate()
                try:
                    game.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    game.kill(); game.wait()
            except Exception as exc:
                report.setdefault("cleanup_errors", []).append("game: "+str(exc))
        game_log.close()
        report["ended_utc"] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
        if last_observation:
            report["final_observation_unanswered_age_ms"] = (time.monotonic_ns()-last_observation["receipt_ns"])/1e6
        latencies = [d["observation_to_tool_ms"] for d in report["model_decisions"] if "observation_to_tool_ms" in d]
        report["observation_to_tool_ms"] = {"count":len(latencies),
            "mean":statistics.mean(latencies) if latencies else None,
            "median":statistics.median(latencies) if latencies else None,
            "max":max(latencies) if latencies else None}
        # The evaluated model has finished. Only now may the supervisor inspect scores.
        results = list((output/"benchmark").glob("*/result.json"))
        if len(results) == 1:
            result_dir = results[0].parent
            verify = subprocess.run([sys.executable,str(ROOT/"tools/verify_run.py"),str(result_dir)],capture_output=True,text=True)
            try:
                verification = json.loads(verify.stdout)
            except json.JSONDecodeError:
                verification = {"passed":False,"error":verify.stderr or verify.stdout}
            write_json(output/"verification.json", verification)
            report["verification_passed"] = verify.returncode == 0 and verification.get("passed") is True
            report["benchmark_result"] = str(results[0].relative_to(output))
            report["benchmark"] = json.loads(results[0].read_text())
        else:
            report["verification_passed"] = False
        report["publishable"] = (report["status"] == "completed" and report["verification_passed"]
                                 and report.get("benchmark", {}).get("completed") is True
                                 and report.get("benchmark", {}).get("valid") is True)
        write_json(output/"run.json", report)
        write_json(output/"events.json", events)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codex", default="codex")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--models", nargs="+")
    parser.add_argument("--efforts", nargs="+", default=["low", "maximum"])
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--lanes", type=int, default=8)
    parser.add_argument("--density", type=float, default=20)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([args.codex,"--version"],text=True).strip()
    summaries = []
    with tempfile.TemporaryDirectory(prefix="chers-model-empty-") as clean_cwd:
        with (args.output/"app-server.log").open("w") as log, AppServer(log,codex=args.codex) as server:
            models = server.models()
            catalog = {"observed_utc":time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),"client_version":version,"models":models}
            write_json(args.output/"model-catalog.json",catalog)
            print("Catalog:",", ".join(m["model"] for m in models),flush=True)
            requested = args.models or [m["model"] for m in models if "image" in m.get("inputModalities",[])]
            for name in requested:
                model_info = next((m for m in models if m["model"] == name),None)
                if model_info is None:
                    raise ValueError("model is not in current visible catalog: "+name)
                preflight = probe_vision(server,name,Path(clean_cwd),args.output/"preflight"/name)
                print("Native image preflight",name,preflight["passed"],flush=True)
                if not preflight["passed"]:
                    summaries.append({"model":name,"status":"image_preflight_failed"})
                    continue
                supported = [r["reasoningEffort"] for r in model_info["supportedReasoningEfforts"]]
                for requested_effort in args.efforts:
                    effort = supported[-1] if requested_effort == "maximum" else requested_effort
                    if effort not in supported:
                        raise ValueError(name+" does not support effort "+effort)
                    for repeat in range(1,args.repetitions+1):
                        label = f"{name}-{effort}-{repeat}"
                        print("Starting",label,flush=True)
                        record = run_trial(server,name,effort,Path(clean_cwd),args.output/label,args.lanes,args.density)
                        summaries.append({"model":name,"effort":effort,"repeat":repeat,"directory":label,
                            "status":record["status"],"verification_passed":record["verification_passed"]})
                        write_json(args.output/"index.json",summaries)
                        print("Finished",label,record["status"],"verified",record["verification_passed"],flush=True)
            write_json(args.output/"index.json",summaries)


if __name__ == "__main__":
    main()
