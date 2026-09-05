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

## 4. What Wave B inherits

The same chain API serves the playback mixer: `oe_audio_factor` is
GTK-free and FFmpeg-free, factors are buffer-constant, and the
matrix helper is reusable. The inspector UI for gain/pan and the
track headers' mute/solo/voice meters land there, on top of
persisted state that already round-trips byte-identically.
