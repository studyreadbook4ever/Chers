# Codex native vision evaluation

This EP.1 pilot measures a model and its observation/action adapter together:
complete CHERS images enter Codex, and explicit model decisions produce Linux
keyboard events. The game keeps running while the model responds. It is a
preliminary configuration comparison, not a general model capability ranking.

## Scope and configuration

The target set is the image-capable models visible through this account's
official Codex `model/list` catalog. It does not establish the complete model
selection available through every ChatGPT Pro product or account. The catalog,
client version, requested model and returned thread settings are recorded.
Provider model fallback is disabled; a model or effort mismatch fails setup.

| Model ID | Lower effort | Maximum advertised effort |
| --- | --- | --- |
| `gpt-6-astra` | `low` | `ultra` |
| `gpt-5.6-sol` | `low` | `ultra` |
| `gpt-5.6-terra` | `low` | `ultra` |
| `gpt-5.6-luna` | `low` | `max` |
| `gpt-5.5` | `low` | `xhigh` |

All ten configurations have an eligible **8-lane, 20 total attempts/s** pilot
with fresh hardware entropy and the `priority` service tier. Connection,
provider-stream or adapter-capacity failures required additional attempts.
All attempts are retained, and the first completed attempt that passes the
validation gates supplies the configuration's result; selection is not based
on score. The documented Luna preflight reuse below is part of that validation.
The full session runs continuously for 61.040 seconds after START, including
initial preview, calibration, wait, 50 seconds of scored play and final
judgement allowances.
There is no pause during inference. Each configuration receives a different
random chart; one trial cannot establish a stable ranking or uncertainty range.
Only completed, independently verified trials supply numeric result rows.
An engine may finish and produce a valid score while its model session reports
a provider error. The strict publication gate excludes that attempt from the
comparison and preserves it as a failed attempt with its diagnostic evidence.

## What the model receives and controls

The [vision bridge](../examples/codex_vision_bridge.py) reads only CHERS' public
640×360 RGBA frames and losslessly encodes the entire image as PNG. The sole
gameplay tool, `chers_step`, returns that native image through a dynamic tool's
`inputImage` content, a locally assigned observation ID, and an acknowledgement
of the model's previous action batch. The adapter performs no OCR, note
detection, cropping, coordinate extraction or target selection. Rules and lane
mapping are supplied in the saved prompt; no private chart, note IDs, target
times or game clock are supplied to the model.
The existing phase countdown and hit feedback remain visible within the image;
there is no continuously increasing global clock or clock field in the transport.

Each model is checked by transcribing a randomly rendered five-digit image
through the same dynamic-tool image path used for gameplay. Pillow renders this
preflight fixture with DejaVu Sans Mono; actual game frames retain their original
pixels and use the bridge's standard-library PNG encoder. The runner normally
requires an exact answer before starting that model's trials. This tests image
delivery and a simple visual read; it does not establish rhythm-game competence.
A failed preflight is a setup result, not a zero benchmark score.

All five models passed this native-image check at least once; some preflight
attempts failed. The final Luna-low trial reused its earlier exact `37259`
transcription from `retry-luna/` after the later `retry-final/` check returned
`72914` for `67604`. Both checks are preserved. The model, CLI version and
native-image delivery route were unchanged; the selected check and its hash
are recorded in the run's
[preflight reference](../evidence/public/codex-native-20260916/retry-final/gpt-5.6-luna-low-1/preflight-reference.json).
This reuse establishes that the route worked, not that every visual read was
correct. It is explicitly recorded rather than applied silently by the runner.

The first `chers_step({"actions": []})` call starts the game. Subsequent calls
contain only an `actions` list, with a lane and `delay_ms` for each chosen tap.
The host anchors that list to the immediately preceding returned image's local
receipt time. The model need not copy the observation ID back. Calls must be
sequential; a request received before the previous image response finishes is
rejected. An empty action list after START requests another observation.

The allowed delay is 0–1000 ms from that image receipt, not from completion of
the model's response. The external scheduler executes only these explicit
choices and drops actions over 20 ms past their requested deadline at submission
or dispatch. This is an adapter policy, separate from CHERS' judgement rules;
dropped actions never reach the benchmark and incur no empty-tap penalty.
Duplicates remain separate actions.

Inputs use XTest keydown/keyup pairs through the X11 desktop, SDL's keyboard
queue and CHERS' input thread. START is a Space keypress and lanes use A–P.
This is software keyboard injection, not a physical USB keyboard or direct
game-state input. CHERS requires no mouse actions. The adapter verifies the
owned window and fails if it loses keyboard focus. Separate 2- and 16-lane
[route checks](../evidence/public/codex-native-20260916/keyboard-route-checks/README.md) exercise lane mapping and duplicate taps; their intentional
aborts are infrastructure checks, not model scores.

Release and ASan/UBSan builds passed all nine CTest groups, including 22 native
vision bridge cases. Sanitizer runs check correctness; reported gameplay
timing comes from the release build. The [test logs](../evidence/public/codex-native-20260916/checks/)
and [artifact audit](../evidence/public/codex-native-20260916/artifact-audit.json)
are retained: all 112 selected-run images matched the recorded tool responses,
and all ten selected runs passed replay with timing compliance required.

### Adapter versions and source records

The initial matrix used bridge v1, with capacity for 2,048 retained observations.
The Astra-low and Luna-low attempts under `retry-final/` use v2, which waits
for a fresh captured frame before returning another observation and allows
8,192 retained observations. Both versions retain the 128 MiB image-evidence
limit. The change affects observation freshness and operational capacity;
full-frame PNG encoding, model prompt, keyboard path, scheduler and game scoring
rules are unchanged. These versions must remain identifiable when comparing
the preliminary results.

The [original execution manifest](../evidence/public/codex-native-20260916/execution-manifest.json)
records the initial source and binaries; the exact
[v1 bridge source](../evidence/public/codex-native-20260916/source-v1/codex_vision_bridge.py)
is retained. The [retry manifest](../evidence/public/codex-native-20260916/retry-final/execution-manifest.json)
records the v2 source used for those attempts. A Luna-low attempt exhausted
v1's observation capacity while running a model-authored polling loop; its
invalid, aborted record is retained under `retry-luna/` and contributes no
numeric comparison result.

## Process boundaries and measurements

The [app-server client](../examples/codex_appserver_client.py) uses the official
Codex CLI and its existing sign-in. The adapter does not read or export account
tokens and requires no separate API key. Evaluated threads have an empty
working directory, no execution environments or selected capability roots,
and disabled shell, browser, app, plugin and multi-agent features. The Code Mode
host remains enabled to route the supplied benchmark tool. It can execute
model-authored JavaScript loops that call the tool repeatedly, without a new
model inference for each call. No model filesystem execution environment is
attached. This is an interface restriction, not an operating-system sandbox
against hostile same-UID processes.
The supervisor reads benchmark result files only after stopping the evaluated
thread and closing its adapter.

The image capture worker targets the benchmark's 80 fps stream. Each tool
response carries an image; **80 fps capture is not 80 fps inference**. A
model-authored JavaScript loop can request images without yielding each one
back to the model. Tool-call count therefore does not equal inference count.
The legacy `model_decisions` field in raw run records stores tool calls.

Observation-to-tool turnaround measures the interval from image receipt to the
next tool request. Depending on the caller, this can include image delivery,
service transport and inference, or just another iteration of a generated
loop. A fast polling loop can dominate the mean; this metric must not be
labelled inference latency. Its summary includes answered observations; the
age of the final unanswered observation is recorded separately. Receiver gap
statistics cover the whole connected capture session and must not be confused
with the engine's timing-compliance measurement.

Report score, actual note count, Perfect/GOOD/empty/miss counts, delaySum and
successful-hit mean error together. With no successful hits and no empty taps,
delaySum is zero and the hit average is N/A; that does not indicate accurate
timing. Benchmark validity, timing compliance, model errors and scheduler
drops are separate outcomes. See [the benchmark contract](../agent.md).

## Reproduce

Use the Linux build requirements in the [README](../README.md#build-and-run),
Python 3 with Pillow, DejaVu Sans Mono, Node/npm, an active X11 session and the
X11/XTest runtime libraries. The preflight locates its font at either
`/usr/share/fonts/TTF/DejaVuSansMono.ttf` (Arch-style layout) or
`/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf` (Debian-style layout).
Keep the CHERS window focused during a trial. The tested client is Codex CLI
0.154.0; an isolated npm installation avoids replacing another installed CLI.
For installation and ChatGPT sign-in guidance, see the
[official Codex CLI documentation](https://learn.chatgpt.com/docs/codex/cli).

```sh
npm install --prefix "$HOME/.local/share/chers-codex-0.154.0" @openai/codex@0.154.0
CHERS_CODEX="$HOME/.local/share/chers-codex-0.154.0/node_modules/.bin/codex"
"$CHERS_CODEX" --version
"$CHERS_CODEX" login status

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

python3 tools/run_codex_evaluations.py \
  --codex "$CHERS_CODEX" \
  --output runs/codex-native-my-run \
  --lanes 8 --density 20 \
  --efforts low maximum --repetitions 1
```

If no sign-in exists, run `"$CHERS_CODEX" login` first. The runner discovers
the visible image-capable catalog unless `--models` supplies a subset, and
resolves `maximum` from each model's advertised efforts. Use a fresh output
directory for each invocation. Account access and usage limits still apply.

The [runner](../tools/run_codex_evaluations.py) saves the catalog, preflight
images and responses, exact prompts and tool schema, tool calls, returned
images, action records, benchmark results and independent replay verification.
Published evidence belongs in
[`evidence/public/codex-native-20260916/`](../evidence/public/codex-native-20260916/).
Failed setup attempts and unexecuted configurations do not supply numeric
results. The deterministic pixel diagnostic in the demonstration video uses
a different controller and is not a result from these model trials.
