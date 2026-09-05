/* oe_audio_factor.h — one shared integer audio factor chain, GTK-free
 * and FFmpeg-free (Phase 10 Wave A).
 *
 * Every audio contribution — in the export mixdown today, in the
 * playback mixer from Wave B — is scaled by ONE deterministic chain
 * computed once per (clip, track, channel) per buffer:
 *
 *   factor[ch] = fade × clip_gain × pan_pair(clip_pan)[ch]
 *                × track_volume × pan_pair(track_pan)[ch]
 *
 * All stages are integer fixed point with unity 1024 (matching
 * OE_FADE_SCALE in oe_fades.h — the same scale convention the fades
 * envelope established). The product is computed exactly in 64-bit
 * integers and normalized with ONE rounding (half away from zero, the
 * oe_time_round_ratio convention) into a per-channel fixed-point gain
 * the consumer applies as sample × factor / 1024.
 *
 * Pan law (locked decision D3): the linear pair L = 1024 − pan,
 * R = pan, normalized so CENTER PAN IS UNITY per channel — the pair
 * values are doubled onto the chain's scale (2 × (1024 − pan) and
 * 2 × pan), so a centered pan contributes exactly 1024 and a hard pan
 * puts 2048 (+6 dB relative to center) on one channel and 0 on the
 * other. The L + R sum is constant (2048) at every pan position: the
 * classic linear equal-sum law. Centered channels therefore sit −6 dB
 * below a hard-panned channel — the documented center attenuation.
 * Keeping center = unity is what makes an all-default Phase 10
 * project mix exactly like a Phase 9 one (locked parity gate: the
 * pre-existing parity tests are untouched); a constant-power law can
 * replace the pair later behind this same interface.
 *
 * Mute and lose-solo zero the whole chain (D5): the caller resolves
 * the track-level matrix (any soloed audio track → only soloed ones
 * contribute; none soloed → mute zeroes the track) and passes the
 * verdict in @mute_or_solo_zero — 0 silences both channels, nonzero
 * computes normally. The single final hard clamp in the export
 * mixdown stays the last word.
 *
 * Integer math only: no floats in the model, the serialized state, or
 * this chain. Factors are buffer-constant by construction; the fade
 * envelope keeps its per-path cadence (playback per sample frame,
 * export per AVFrame) — the chain is recomputed whenever the fade
 * value changes, which is exactly the existing envelope rhythm.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Fixed-point unity for every chain stage and for the output (the
 * consumer divides by this when scaling samples). Same value as
 * OE_FADE_SCALE by design. */
#define OE_AUDIO_UNITY 1024

/* Gain/volume domain (locked decision D7): 0 = silence, 1024 = unity,
 * 2048 = +6 dB. */
#define OE_AUDIO_GAIN_MAX 2048

/* Pan domain (D7): 0 = hard left, 512 = center, 1024 = hard right. */
#define OE_AUDIO_PAN_MAX 1024
#define OE_AUDIO_PAN_CENTER 512

/**
 * oe_audio_factor: the per-(clip, track, channel) chain, one call per
 * buffer (per AVFrame in the export mixdown).
 *
 * @fade: the oe_fade_gain() envelope value for this buffer, 0..1024.
 * @clip_gain: clip gain state, 0..2048 (unity 1024).
 * @clip_pan: clip pan state, 0..1024 (center 512).
 * @track_volume: track volume state, 0..2048 (unity 1024).
 * @track_pan: track pan state, 0..1024 (center 512).
 * @mute_or_solo_zero: the resolved mute/solo verdict — 0 zeroes both
 *     output channels, nonzero computes the chain (D5).
 * @factor_out: receives [L], [R] as fixed-point gains on the
 *     #OE_AUDIO_UNITY scale; the consumer scales samples with
 *     sample × factor / #OE_AUDIO_UNITY. Pure function of its
 *     arguments: no state, no allocation.
 */
void oe_audio_factor (gint32 fade, gint32 clip_gain, gint32 clip_pan, gint32 track_volume,
                      gint32 track_pan, gint32 mute_or_solo_zero, gint32 factor_out[2]);

/**
 * oe_audio_audible: the track-level mute/solo matrix (D5), resolved
 * once per track per mix. With ANY audio track soloed, only soloed
 * tracks are audible; with none soloed, mute zeroes the track. Solo
 * is serialized project state honored identically by preview and
 * export — never monitoring-only. The caller scans the audio tracks
 * for @any_solo (video tracks carry no audio state and never count).
 */
gboolean oe_audio_audible (gint32 mute, gint32 solo, gboolean any_solo);

G_END_DECLS
