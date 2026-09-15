# CHERS contributor instructions

Read `agent.md` for the benchmark contract and external agent interface. This
repository implements the benchmark and a diagnostic pixel client, not a trained
model. Preserve the separation between private game state and public pixels.

## Invariants

- The only workload arguments are lane count (2–16) and total attempted note
  density (2×lanes through 24×lanes per second).
- Every lane must respect a minimum 40 ms between note target times. Drop a
  generation attempt when all lanes are locked; never backfill it.
- Perfect is inclusive ±8 ms, GOOD inclusive ±20 ms. One input consumes at most
  one note. An empty tap costs 5 score units and adds 100 ms to delaySum.
- Misses earn zero and remain in the denominator. Scores may be negative.
- Show the successful-hit mean absolute timing error beside delaySum, rounded
  to three decimal places. Exclude empty-tap penalties from this average; show
  N/A when there are no successful hits.
- Match and consume online; aggregate final metrics from recorded events after
  the run. All accepted taps count, including rapid duplicates.
- Model observations are complete 640×360 RGBA images with edge-to-edge equal
  lanes. Never export chart, target times, note IDs, or game time in transport
  metadata. Do not draw a global game clock. Phase labels and hit feedback may
  be drawn into the image.
- Use integer monotonic timestamps. The public client cannot backdate inputs.
- Do not silently substitute pseudorandom output for required hardware entropy.
- A timing budget is measured, not guaranteed by a requested thread frequency.
  Keep benchmark validity and measured timing compliance distinct.

## Development and checks

Build with CMake/Ninja and run CTest. Changes to timing, scoring, IPC, rendering,
or generation require the corresponding invariant tests and an end-to-end run.
Exercise both 2 and 16 lanes, fresh entropy, duplicate taps, and a slow frame
consumer. Use ASan/UBSan for correctness checks; report release-build timing
separately. Do not quote sanitizer timings as production performance.

Keep capture/renderer work off the input polling thread. Avoid allocation,
formatting, file I/O, and model inference on that thread during a scored run.
Evidence must identify the client used: a deterministic diagnostic pixel player
is not a frontier LLM evaluation. Do not claim unseen or unexecuted checks passed.

Do not change unrelated files, users' sessions, or system power settings as a
normal benchmark feature. Shutdown is not part of this application's behavior.
