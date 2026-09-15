# CHERS native Codex vision pilots

Fresh-chart pilot attempts. Workloads: 8 lanes, 20 total attempts/s.

## Selected results

For each (model, effort), select the first numeric-eligible run by started_utc; break equal timestamps by relative directory path. A valid timezone-qualified timestamp is required. Scores and latency do not affect selection. Catalog model order; low then the catalog's maximum supported effort, then other efforts.

The full selection record is in [selection.json](selection.json).

| Model | Effort | Score | Hits / notes | delaySum (ms) | Hit mean (ms) | Obs→tool mean (s) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| [gpt-6-astra](retry-final/gpt-6-astra-low-1/run.json) | low | 0.000% | 0 / 1017 | 0.000 | N/A | 4.269 |
| [gpt-6-astra](gpt-6-astra-ultra-1/run.json) | ultra | 0.000% | 0 / 936 | 0.000 | N/A | 4.322 |
| [gpt-5.6-sol](gpt-5.6-sol-low-1/run.json) | low | 0.000% | 0 / 990 | 0.000 | N/A | 3.974 |
| [gpt-5.6-sol](gpt-5.6-sol-ultra-1/run.json) | ultra | 0.000% | 0 / 1034 | 0.000 | N/A | 12.079 |
| [gpt-5.6-terra](gpt-5.6-terra-low-1/run.json) | low | 0.000% | 0 / 958 | 0.000 | N/A | 4.727 |
| [gpt-5.6-terra](gpt-5.6-terra-ultra-1/run.json) | ultra | 0.000% | 0 / 958 | 0.000 | N/A | 10.979 |
| [gpt-5.6-luna](retry-final/gpt-5.6-luna-low-1/run.json) | low | 0.000% | 0 / 963 | 0.000 | N/A | 5.127 |
| [gpt-5.6-luna](retry-luna/gpt-5.6-luna-max-1/run.json) | max | 0.000% | 0 / 1010 | 0.000 | N/A | 12.171 |
| [gpt-5.5](gpt-5.5-low-1/run.json) | low | 0.000% | 0 / 1021 | 0.000 | N/A | 4.200 |
| [gpt-5.5](gpt-5.5-xhigh-1/run.json) | xhigh | 0.000% | 0 / 991 | 0.000 | N/A | 9.253 |

## All attempted runs

Failed attempts and later eligible runs remain listed below.

| Model | Effort | Score | Hits / notes | delaySum (ms) | Hit mean (ms) | Obs→tool mean (s) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| [gpt-6-astra](gpt-6-astra-low-1/run.json) | low | failed | — | — | — | — |
| [gpt-6-astra](retry-final/gpt-6-astra-low-1/run.json) | low | 0.000% | 0 / 1017 | 0.000 | N/A | 4.269 |
| [gpt-6-astra](gpt-6-astra-ultra-1/run.json) | ultra | 0.000% | 0 / 936 | 0.000 | N/A | 4.322 |
| [gpt-5.6-sol](gpt-5.6-sol-low-1/run.json) | low | 0.000% | 0 / 990 | 0.000 | N/A | 3.974 |
| [gpt-5.6-sol](gpt-5.6-sol-ultra-1/run.json) | ultra | 0.000% | 0 / 1034 | 0.000 | N/A | 12.079 |
| [gpt-5.6-terra](gpt-5.6-terra-low-1/run.json) | low | 0.000% | 0 / 958 | 0.000 | N/A | 4.727 |
| [gpt-5.6-terra](gpt-5.6-terra-ultra-1/run.json) | ultra | 0.000% | 0 / 958 | 0.000 | N/A | 10.979 |
| [gpt-5.6-luna](retry-luna/gpt-5.6-luna-low-1/run.json) | low | failed | — | — | — | — |
| [gpt-5.6-luna](retry-final/gpt-5.6-luna-low-1/run.json) | low | 0.000% | 0 / 963 | 0.000 | N/A | 5.127 |
| [gpt-5.6-luna](retry-luna/gpt-5.6-luna-max-1/run.json) | max | 0.000% | 0 / 1010 | 0.000 | N/A | 12.171 |
| [gpt-5.5](gpt-5.5-low-1/run.json) | low | 0.000% | 0 / 1021 | 0.000 | N/A | 4.200 |
| [gpt-5.5](gpt-5.5-xhigh-1/run.json) | xhigh | 0.000% | 0 / 991 | 0.000 | N/A | 9.253 |

### Excluded attempts

- [gpt-6-astra-low-1](gpt-6-astra-low-1/run.json): run not eligible for numeric publication
- [retry-luna/gpt-5.6-luna-low-1](retry-luna/gpt-5.6-luna-low-1/run.json): RuntimeError: observation evidence capacity exceeded


## Interpretation

Zero delaySum with zero hits is not accurate timing; the hit mean is N/A.
Obs→tool measures local image receipt to the returned tool call and includes runtime, transport, model processing, and tool-loop overhead; it is not pure inference time. It covers answered observations only. Capture rate is not inference rate. The summary's tool_calls count includes observation-only and invalid calls, not only action decisions.
See each run's agent/bridge.json, verification.json and benchmark result for actions and timing.
