# CHERS demonstration video

[Watch the CHERS demonstration on YouTube](https://youtu.be/eflUBbF9UCc).
The README uses a linked gameplay poster; clicking it opens the published video.

## What the video shows

- **1920×1080, 16:9** composition filled by two views: the real Cursor/Codex
  session on the left and the running CHERS window on the right.
- **8 lanes, total average attempted density 20/s**, invoked with
  `./build/chers 8 20`.
- A fresh hardware-entropy chart, five seconds of calibration, five seconds of
  waiting, and fifty seconds of scored play, with the normal preview and final
  judgement allowances.
- The separate **deterministic pixel diagnostic client** performing the taps.
  The displayed Codex session is not the millisecond gameplay controller.
- Explanatory white subtitles. There is no game music or game audio.

Actual emitted notes vary from run to run. Density is an average generation
attempt rate, not a per-second quota. The final result and replay artifacts are
the evidence for the recorded run; subtitles must agree with those results.

## Local delivery files

The recording workflow writes:

```text
media/CHERS-demo-1080p.mp4
media/CHERS-demo-1080p.srt
```

These files and raw desktop/session captures are ignored by Git. The source
repository contains only the curated gameplay poster and public verification
artifacts. The published video is linked above; raw desktop/session captures
remain outside the repository.

## README presentation

The poster links to the video without sharing-query parameters. Its description
identifies the deterministic diagnostic player so viewers can distinguish the
demonstration from the separately reported native-model evaluations.
