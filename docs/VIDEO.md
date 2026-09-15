# CHERS demonstration video

The demonstration is recorded locally for the project owner to upload to
YouTube. The README's image is a gameplay-only poster; it does not link to a
video until the owner adds the actual published URL.

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
artifacts. The local video may include the user's own Cursor conversation and
is left for the owner to publish.

## Add the YouTube link

After the owner uploads the video, replace the README's demo image with a
linked image using the actual video URL. A comment next to the image marks the
edit location. Keep the description of the diagnostic player so viewers can
distinguish benchmark engineering from an evaluation of a trained model.
