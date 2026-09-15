import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path('/tmp/chers-readme-20260916')
OUT = Path('/tmp/chers-native-keyboard-preflight')
sys.path.insert(0, str(ROOT / 'examples'))
from codex_vision_bridge import NativeVisionBridge, find_window_for_pid

summaries = []
for lanes, density in [(2, 4), (16, 32)]:
    directory = OUT / f'{lanes}lane-{density}'
    directory.mkdir(exist_ok=True)
    env = dict(os.environ)
    for key in list(env):
        if key.startswith(('CHERS_', 'CHRONOLANE_')):
            del env[key]
    endpoint = f'/tmp/chers-kbd-preflight-{os.getpid()}-{lanes}.sock'
    env.update(CHERS_OUTPUT=str(directory / 'benchmark'), CHERS_SOCKET=endpoint,
               SDL_VIDEODRIVER='x11')
    bridge = None
    with (directory / 'benchmark.log').open('w') as log:
        process = subprocess.Popen([str(ROOT / 'build/chers'), str(lanes), str(density)],
                                   env=env, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        try:
            window_id = find_window_for_pid(process.pid, timeout=10)
            bridge = NativeVisionBridge(lanes, window_id, endpoint=endpoint,
                                        library=str(ROOT / 'build/libchers_client.so'),
                                        expected_pid=process.pid)
            first = bridge.observe()
            bridge.start()
            start_local = time.monotonic()
            time.sleep(1.5)
            observation = bridge.observe()
            submitted = bridge.submit(observation['observation_id'], [
                {'lane': 'A', 'delay_ms': 100},
                {'lane': 'A', 'delay_ms': 100},
                {'lane': chr(ord('A') + lanes - 1), 'delay_ms': 100},
            ])
            time.sleep(max(0, 2.2 - (time.monotonic() - start_local)))
            snapshot = bridge.snapshot()
            bridge.close()
            bridge.export(directory / 'agent')
            process.send_signal(signal.SIGINT)
            returncode = process.wait(timeout=10)
        finally:
            if bridge is not None:
                bridge.close()
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    inputs_paths = list((directory / 'benchmark').glob('*/inputs.csv'))
    assert len(inputs_paths) == 1, inputs_paths
    with inputs_paths[0].open() as stream:
        inputs = list(csv.DictReader(stream))
    expected = [0, 0, lanes - 1]
    observed = [int(row['lane']) for row in inputs]
    assert observed == expected, (observed, inputs)
    assert all(row['stage'] == 'calibration' for row in inputs), inputs
    result_path = inputs_paths[0].with_name('result.json')
    result = json.loads(result_path.read_text())
    assert snapshot['counts'].get('action_sent') == 3, snapshot['counts']
    assert snapshot['input_mode'] == 'keyboard'
    summary = dict(lanes=lanes, density=density, window_id=window_id,
                   game_pid=process.pid, observed_input_lanes=observed,
                   duplicate_lane0_count=observed.count(0), keyboard_events_sent=3,
                   submission=submitted, exit_code=returncode,
                   result_file=str(result_path), bridge_file=str(directory / 'agent/bridge.json'),
                   source_field_available='source' in inputs[0],
                   source_evidence='XTest keyboard sink used; transport tap method never called',
                   aborted_by_design=True, model_score=False,
                   purpose='Owned X11 GUI keyboard route preflight with fresh hardware entropy')
    (directory / 'verification.json').write_text(json.dumps(summary, indent=2) + '\n')
    summaries.append(summary)
    print(json.dumps(summary), flush=True)
(OUT / 'summary.json').write_text(json.dumps(summaries, indent=2) + '\n')
