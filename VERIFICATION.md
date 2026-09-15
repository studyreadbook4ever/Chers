# CHERS verification

The publication build was verified locally on 2026-09-15. It has no increasing
clock and no `SCORED` label during the trial. Other phase labels remain visible.
The human GUI renders at its actual window size; the model stream remains
640×360 at a target of 80 frames/s.

## Automated checks

Release and ASan/UBSan passed **all six CTest groups**: core rules and generation,
renderer geometry/labels/average error, isolated IPC and slow readers, diagnostic
image tracking, independent replay, and CLI lifecycle. Checks include duplicate
taps, consumed notes, inclusive judgement boundaries and the 40 ms lane lock.
Sanitizer tests establish correctness, not production timing.

[Release log](evidence/public/release-ctest.log) · [ASan/UBSan log](evidence/public/asan-ctest.log)

## Fresh real-time sessions

Each run generated a different hardware-RDSEED chart and completed the full
calibration, wait, 50-second scored stage and final judgement allowance. Every
result passed independent replay; no saved chart was supplied to the player.

| Lanes / attempted density | Scored notes | Perfect | Score | delaySum ms | Average ms | Producer max gap ms | Client max gap ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 / 8 | 366 | 366 | 100.000% | 103.933 | 0.284 | 15.083197 | 15.159300 |
| 16 / 384 | 16,251 | 16,251 | 100.000% | 4250.861 | 0.262 | 14.609557 | 14.610200 |
| 8 / 20 | 1,002 | 1,002 | 100.000% | 270.631 | 0.270 | 15.339113 | 15.368800 |

All three had zero GOOD, empty taps and misses, and no recorded producer or
client gaps above 16 ms. These are measured results, not a hard-real-time
guarantee. The 2- and 16-lane runs were headless; the 8-lane run used the human
GUI while recording the desktop at 1080p60.

[Comparison data](evidence/public/summary.json) · [Demo result](docs/assets/chers-demo-result.png)

## Replay the public evidence

```sh
python tools/verify_run.py evidence/public/pixel-2 --require-timing
python tools/verify_run.py evidence/public/pixel-16 --require-timing
python tools/verify_run.py evidence/public/pixel-8 --require-timing
```

Each directory includes its post-run chart, input events, raw benchmark timing
samples, result.json and replay report. Diagnostic summaries identify the
client and receipt gaps. The desktop recording remains outside this source
repository for the owner to upload separately.

## Interpretation and limits

The player is a separate **deterministic pixel diagnostic**, not a trained model
or a frontier LLM performing millisecond inference. It receives complete RGBA
images, tracks motion against local receipt times and learns constant delay
from visible feedback. After observing WAIT, it infers scored play from the
absent label and intact playfield. Completion is read from FINISHED pixels.
Codex orchestrates execution and verification in the recording.

The average includes successful-hit absolute errors only; delaySum also
includes the empty-tap penalties. Density is an attempted generation rate.
Include note counts when comparing results across workloads.

The machine used an Intel Core i7-10700 and GTX 1660 SUPER on Linux. Compiler,
kernel, worker affinities and build type are in each result manifest. General
Linux scheduling and host load affect timing. The same-UID/root process boundary
is not a hostile-agent sandbox. RDSEED execution does not certify physical
entropy; replay detects inconsistent artifacts, not replacement of all records.
Timing statistics are producer/client measurements, not independent wall-clock
attestation. Use [README.md](README.md) and [agent.md](agent.md) to reproduce.
