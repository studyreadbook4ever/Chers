# Keyboard route checks

Infrastructure checks, not model benchmark scores. Fresh RDSEED runs at 2 lanes / 4 attempts per second and 16 lanes / 32 attempts per second each received three XTest keyboard taps: A, A, and the final lane. The recorded input lanes match `[0, 0, 1]` and `[0, 0, 15]`. Both runs intentionally stopped during calibration after about 2.2 seconds; their incomplete results must not enter model score tables.

`run.py` is the exact local driver used, including original absolute paths. Each `verification.json` and the top-level `summary.json` retain those paths as historical provenance. The corresponding files are included beneath this directory. These checks used the bridge source preserved under `../source-v1/`.
