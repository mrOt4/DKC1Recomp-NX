#include "desktop_audio_rate.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void Dkc1AudioStretchReset(Dkc1AudioStretch *stretch) {
  if (!stretch) return;
  memset(stretch, 0, sizeof *stretch);
}

/* Interpolation kernel: Kaiser-windowed sinc (beta 8, about 80 dB of
 * stopband) at the full Nyquist rate, since the ratio stays within a
 * fraction of a percent of 1. kTaps[p][k] weights element k of a window
 * whose output point lies p/kPhases past element kHalf - 1. At p = 0 it is
 * an exact unit impulse, so a ratio of exactly 1 passes samples unchanged.
 * Linear interpolation, by contrast, attenuates treble by an amount that
 * follows the slowly sweeping fraction and so modulates it. */
enum { kHalf = DKC1_AUDIO_STRETCH_TAPS / 2, kPhases = 256 };
static float s_taps[kPhases + 1][DKC1_AUDIO_STRETCH_TAPS];
static bool s_taps_ready;

static double BesselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 32; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}

static void BuildTaps(void) {
  const double beta = 8.0, norm = BesselI0(beta);
  for (int p = 0; p <= kPhases; p++) {
    const double frac = (double)p / kPhases;
    double sum = 0.0;
    for (int k = 0; k < DKC1_AUDIO_STRETCH_TAPS; k++) {
      const double x = (double)k - (kHalf - 1) - frac;
      const double sinc = fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x);
      const double w = x / kHalf;
      const double window =
          fabs(w) >= 1.0 ? 0.0 : BesselI0(beta * sqrt(1.0 - w * w)) / norm;
      s_taps[p][k] = (float)(sinc * window);
      sum += s_taps[p][k];
    }
    for (int k = 0; k < DKC1_AUDIO_STRETCH_TAPS; k++)
      s_taps[p][k] = (float)(s_taps[p][k] / sum);
  }
  s_taps_ready = true;
}

int Dkc1AudioStretchProcess(Dkc1AudioStretch *stretch, double ratio,
                            const int16_t *in, int in_frames, int16_t *out,
                            int out_capacity) {
  enum { kKept = DKC1_AUDIO_STRETCH_TAPS };
  if (!stretch || !in || !out || in_frames <= 0 || out_capacity <= 0)
    return 0;
  if (!(ratio > 0.0)) ratio = 1.0;
  if (!s_taps_ready)
    BuildTaps();
  if (!stretch->primed) {
    for (int i = 0; i < kKept; i++) {
      stretch->last[i][0] = in[0];
      stretch->last[i][1] = in[1];
    }
    stretch->position = kKept;
    stretch->primed = true;
  }
  /* This call sees the kept frames followed by the input: element j is
   * last[j] for j < kKept and in[j - kKept] after. Position p interpolates
   * between elements floor(p) and floor(p) + 1 from kHalf - 1 elements
   * before to kHalf after, so p stays below in_frames + kHalf; the last
   * kKept elements are kept for the next call, where p resumes in_frames
   * lower. The stream runs kHalf frames late. */
#define ELEMENT(j, c) \
  ((j) < kKept ? stretch->last[(j)][(c)] : in[((j) - kKept) * 2 + (c)])
  const double step = 1.0 / ratio;
  double position = stretch->position;
  int written = 0;
  while (position < (double)(in_frames + kHalf) && written < out_capacity) {
    const int index = (int)position;
    const double scaled = (position - (double)index) * kPhases;
    const int phase = (int)scaled;
    const float t = (float)(scaled - phase);
    const float *a = s_taps[phase], *b = s_taps[phase + 1];
    const int first = index - (kHalf - 1);
    for (int c = 0; c < 2; c++) {
      float acc = 0.0f;
      for (int k = 0; k < DKC1_AUDIO_STRETCH_TAPS; k++)
        acc += (a[k] + (b[k] - a[k]) * t) * (float)ELEMENT(first + k, c);
      const long value = lrintf(acc);
      out[written * 2 + c] = (int16_t)(value > 32767 ? 32767
                                       : value < -32768 ? -32768 : value);
    }
    written++;
    position += step;
  }
  int16_t kept[kKept][2];
  for (int i = 0; i < kKept; i++)
    for (int c = 0; c < 2; c++)
      kept[i][c] = ELEMENT(in_frames + i, c);
#undef ELEMENT
  memcpy(stretch->last, kept, sizeof kept);
  stretch->position = position - (double)in_frames;
  return written;
}

double Dkc1AudioRateRatio(double fill_average, double target,
                          double max_deviation, double gain) {
  if (!(target > 0.0) || !(max_deviation > 0.0)) return 1.0;
  double deviation = gain * (target - fill_average) / target;
  if (deviation > 1.0) deviation = 1.0;
  if (deviation < -1.0) deviation = -1.0;
  return 1.0 + max_deviation * deviation;
}

double Dkc1AudioFillAverage(double previous, double fill, double weight) {
  if (previous < 0.0) return fill;
  if (weight <= 0.0) return previous;
  if (weight >= 1.0) return fill;
  return previous + (fill - previous) * weight;
}
