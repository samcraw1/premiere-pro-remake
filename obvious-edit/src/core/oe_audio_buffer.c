/* oe_audio_buffer.c — the buffer math (see the header for the law). */

#include "oe_audio_buffer.h"

#include <math.h>

gfloat
oe_audio_buffer_peak (const gfloat *interleaved, gsize n_frames, int channels, int channel)
{
  if (interleaved == NULL || n_frames == 0)
    return 0.0f;

  const int ch = CLAMP (channel, 0, MAX (channels, 1) - 1);
  const int stride = MAX (channels, 1);
  gfloat peak = 0.0f;

  for (gsize i = 0; i < n_frames; i++)
    {
      const gfloat v = fabsf (interleaved[i * (gsize) stride + (gsize) ch]);

      if (v > peak)
        peak = v;
    }

  return peak;
}
