# Phase 10 Wave A — Audio core: state, chain, undo, persistence, mixdown

*The guided walkthrough for the audio phase's first wave: per-clip
gain/pan and per-track volume/pan/mute/solo, one shared integer factor
chain consumed by the export mixdown, two new undo ops, and the audio
JSON members — with the playback mixer, UI, and metering deferred to
Wave B.*

## 1. Phase purpose

Through Phase 9 the mixdown multiplies each source by the fade
envelope only: no way to make a clip quieter, pan it, or mute a track.
Phase 10 Wave A adds the audio state model and the one chain that
consumes it, under a compatibility constraint that shapes every file
in the wave:

> **An all-default Phase 10 project mixes exactly like a Phase 9
> one.** Identity audio is the zero-value default — in the model, in
> the JSON, and in the chain's arithmetic (center pan contributes
> exactly unity) — so the pinned parity tests stay untouched and the
> factor chain changes nothing it was not asked to.

The wave delivers five things: the `OeClipAudio` substruct on every
clip and the `OeTrackAudio` fields on audio tracks only, with
identity defaults and validated mutators; the shared integer factor
chain in `src/core/oe_audio_factor.c` (fade × clip gain/pan × track
volume/pan, mute/lose-solo zeroing) wired into the export mixdown;
the two new undo ops replaying through the setters; the clip and
audio-track `audio` JSON members under the closed-member backfill
recipe; and the ratio-band, matrix, undo, and persistence tests that
pin all of it.

## 2. Decisions worth reading twice

- **Center unity (the pan law).** The naive linear pair `L = 1024 −
  pan, R = pan` attenuates the center 6 dB — an all-default project
  would export 6 dB quieter than Phase 9 and the parity tests would
  fail. Doubling the pair (`2 × (1024 − pan)`, `2 × pan`) makes
  center = unity and hard pan = 2048 on one channel, 0 on the other;
  the L+R sum is constant at every position. The documented −6 dB
  center attenuation falls out of the doubling instead of breaking
  the defaults.
- **Buffer-constant factors.** The clip/track audio state is folded
  into the per-AVFrame chain call; the fade envelope keeps its
  per-frame cadence unchanged. Nothing churns in the sample loop.
- **Track-indexed undo payload.** `OE_UNDO_OP_TRACK_AUDIO` is keyed
  by track index alone — no sentinel clip index. Audio state belongs
  to the track and the model rejects audio state on video tracks, so
  there is never a clip identity to carry.
- **Skip silenced media.** The mixdown resolves the mute/solo matrix
  before the per-clip loop and skips a silenced track before its
  source is even opened — behaviorally identical to adding zeros,
  materially cheaper.

## 3. The gate battery held

Zero-warning `-Werror` build, all suites plain and under
ASan/UBSan and Valgrind (existing suppression file, no new
suppressions), tree-wide clang-format, `scripts/run-headless.sh`
exit 0, and the parity harness untouched and green.

## 4. Wave B — multi-track playback, metering, mixer

Wave B turns playback into the mixer Wave A's chain deserved.
The lesson that transfers from Wave A: **one seam, two consumers**
becomes **one seam, three** — the mix window that feeds SDL also
feeds the meter and, through the `set_mix_func` observer, the parity
test. Nothing re-implements summation.

- **One mix buffer, one owner.** The session owns an interleaved
  f32 buffer spanning one decode-ahead window of sequence time;
  each contributing track's chunks are decoded sequentially (the
  worker carries one request) and accumulated in track-array order
  before any push. Deterministic order, gaps silent, clamps last.
- **Chunk labels must be buffer-anchored.** The worker's chunk
  buffer survives decoder frames; labeling each delivery from the
  current frame's arithmetic mislabels residuals late and trails the
  final partial chunk at source zero. Wave A's single-lane session
  never noticed because it consumed chunks in delivery order; the
  first consumer that maps chunks by time exposed it. Map by the
  buffer's anchor, not the frame.
- **Feeding is coverage, not frame counts.** The first window
  begins LOOKAHEAD ahead of the playhead, so `base + frames/rate`
  under-reports queued coverage and re-submits overlapping windows.
  Track pushed coverage in sequence time; keep the restart-anchored
  base for real-device drift correction.
- **Meters are observers, not threads.** Peaks extract per chunk on
  the main context into GTK-free math (`oe_audio_buffer`,
  `oe_meter_math`); the widget holds a peak with a short decay and
  releases to silence on pause, stop, and scrub. No paused tap, no
  timers while paused — the absence of chunks IS the signal.
- **Parity is the acceptance.** A two-track project with distinct
  levels and pans plays through the virtual clock while the mix
  observer captures the real windows; the export mixdown decodes
  back and both sides must agree per channel. The test fails by
  construction on single-lane playback — its pass is the proof.
