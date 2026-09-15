# CHERS — visual real-time benchmark contract

CHERS is a Linux C++ benchmark for visual timing and action control. Rules
are public: this is not a hidden-rule discovery task. There is no audio. Pooling,
image processing, prediction, and scheduling in the external agent are allowed;
report all such components when publishing a model result.

## Fixed workload

Run `chers LANES DENSITY`. LANES is an integer from 2 to 16, mapped in order
to A through P. DENSITY is the global average generation-attempt rate per second,
between 2×LANES and 24×LANES. It is not an exact per-second quota or a guaranteed
delivered density. Each attempt uses an exponential waiting time and selects
uniformly among unlocked lanes. If all lanes are locked, discard the attempt.
Accepted target times on a lane are separated by at least 40 ms.

Default entropy comes from hardware RDSEED. An unavailable or failed source is
an explicit startup error, never an invisible fallback. Private charts and input
records are written only after completion/abort, for independent replay.

## Time and phases

Time zero is the START control or human Space/Enter. An initial one-second
preroll gives the first calibration notes their full visual preview.

| Phase | Target-time interval since START |
| --- | --- |
| Calibration | [1.000 s, 6.000 s) |
| Calibration final judgement allowance | through 6.020 s |
| Wait | 6.020 s to 11.020 s |
| Scored trial | [11.020 s, 61.020 s) |
| Final judgement allowance | through 61.040 s |

Each note first appears one second before its target time. Consequently the last
second of the wait displays the first trial notes. Each phase is armed 20 ms
before its target-time interval so its earliest notes retain the full early
judgement window. Calibration events never enter trial scores. Completed notes
disappear immediately from the logical matching state. Notes not hit by their
late boundary are misses. A final result can appear after the last 20 ms window.

## Two evaluation values

For `N` actual scored notes, `P` Perfect, `G` GOOD, and `E` empty taps:

1. `score = (2*P + G - 5*E) / (2*N)`; displayed percentage is `100*score`.
2. `delaySum_ms = sum(abs(hit_time - target_time)) + 100*E`, in milliseconds,
   summing successful scored hits only before adding empty-tap penalties.

The result also displays `AVERAGE` beside `DELAYSUM`: the sum of absolute
successful-hit timing errors divided by `P+G`, in milliseconds rounded to three
decimal places. Empty-tap penalties do not enter this average. With no successful
hits, the average is N/A. This is explanatory timing information alongside the
two evaluation values, not a replacement for either metric.

Perfect includes ±8 ms; GOOD covers the remainder through ±20 ms. Misses earn
zero. N=0 is N/A, not a valid zero score. A note is consumed once. Repeated taps
are all processed, with no debounce or input cooldown. If a subsequent note is
eligible, a subsequent tap can hit it; otherwise each tap is an empty tap.
Exactly tied eligible notes resolve to the earlier target. Scores can be
negative. Record N and hit counts alongside delaySum: its raw magnitude depends
on workload size and is not a stand-alone ranking across densities.

## Observation and action boundary

The external agent uses `libchers_client` and `chers/client.h`. It gets
only complete RGBA pixels plus transport shape/format/frame sequence. The
640×360 image is filled by N equal-width vertical lanes. Note motion, phase
labels and hit feedback are part of those pixels. During scored play the top-left
phase label is intentionally absent; other phase labels remain visible. No global game clock is drawn
or exported. An agent can track trajectories against its own monotonic
frame-receipt clock. Frames target 80 fps (12.5 ms); delivery gaps
over 16 ms are measured and reported. The human window can render independently
at another size and refresh rate. Human-window frames are not agent observations.

The client sends zero-based lane taps (A=0). The server stamps inputs with its
monotonic clock when receiving them; agent-provided timestamps or note IDs are
not accepted. START and STOP are session controls, not gameplay information.
Waiting for a predicted future tap belongs to the external agent. Every tap must
be a separate event. Slow image consumers may skip frames; input events cannot
be silently dropped. Resource overflow invalidates the run.

The protocol is an information boundary between separate processes. It is not a
claim of protection against hostile programs running as the same Unix user or
root. For competitive submissions, use an external supervisor to restrict the
agent's filesystem, process inspection and available endpoints, for example
with a suitably configured namespace sandbox. The current socket accepts the
same Unix UID only; a different-UID deployment needs an explicit credential or
proxy policy. Never expose debugger or result-directory access during a trial.

## Honest evaluation

Report model, preprocessing, pooling, scheduler, hardware, renderer/client
versions, observed frame gaps, and benchmark validity. Constant input delay may
be learned during calibration; it is not silently subtracted by the benchmark.
The diagnostic pixel player demonstrates the interface and timing path. Its
results must not be presented as a trained model's or an LLM's performance.
