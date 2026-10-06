/* Compares an MSU-1 PCM pack with its compressed (tools/msu1_compress.py)
 * version through the real player (runner/dkc1_msu1.c).
 *
 *   cc -O2 -Irunner $(sdl2-config --cflags) tests/test_msu1_pack.c \
 *      runner/dkc1_msu1.c runner/dkc1_stb_vorbis.c \
 *      $(sdl2-config --libs) -lm -o build/test_msu1_pack
 *   build/test_msu1_pack msu-1 msu-1-ogg
 *
 * Each track is mixed at 48 kHz from both packs and the outputs are
 * compared (SNR). Looping track 17 plays past its end, so a wrong Vorbis
 * loop point would collapse the SNR after the loop. Packs are private;
 * nothing here embeds or writes music. */
#include "dkc1_msu1.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kRate = 48000, kChunk = 800 };

typedef struct Result {
  double snr_db;
  long frames;
} Result;

static int Play(const char *dir, unsigned track, long frames, int16_t *out) {
  char error[256];
  Dkc1Msu1 *player = Dkc1Msu1Open(dir, error, sizeof error);
  if (!player) {
    fprintf(stderr, "%s: %s\n", dir, error);
    return 0;
  }
  Dkc1Msu1ObserveMusicState(player, (uint16_t)(track - 1), 1);
  if (Dkc1Msu1CurrentTrack(player) != track ||
      !Dkc1Msu1WaitReady(player, 5000)) {
    Dkc1Msu1Close(player);
    return 0;
  }
  memset(out, 0, (size_t)frames * 2 * sizeof *out);
  for (long done = 0; done < frames; done += kChunk) {
    const int n = (int)(frames - done < kChunk ? frames - done : kChunk);
    Dkc1Msu1Mix(player, out + done * 2, n, 2, kRate);
  }
  Dkc1Msu1Close(player);
  return 1;
}

static Result Compare(const int16_t *a, const int16_t *b, long frames) {
  double signal = 0, noise = 0;
  for (long i = 0; i < frames * 2; i++) {
    signal += (double)a[i] * a[i];
    noise += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
  }
  Result r = {noise ? 10 * log10(signal / noise) : 99.0, frames};
  return r;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <pcm-pack> <ogg-pack>\n", argv[0]);
    return 2;
  }
  /* Track, seconds. Track 17 loops at 61 s: 75 s crosses the loop. */
  const struct { unsigned track; int seconds; } cases[] = {
    {1, 20}, {2, 20}, {12, 20}, {15, 20}, {17, 75}, {25, 20},
  };
  int failures = 0;
  for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    const long frames = (long)cases[c].seconds * kRate;
    int16_t *pcm = malloc((size_t)frames * 4), *ogg = malloc((size_t)frames * 4);
    if (!pcm || !ogg ||
        !Play(argv[1], cases[c].track, frames, pcm) ||
        !Play(argv[2], cases[c].track, frames, ogg)) {
      printf("track %2u: could not play\n", cases[c].track);
      failures++;
    } else {
      const Result whole = Compare(pcm, ogg, frames);
      const long tail = frames / 6;  /* last sixth: past the loop for 17 */
      const Result end = Compare(pcm + (frames - tail) * 2,
                                 ogg + (frames - tail) * 2, tail);
      const int ok = whole.snr_db > 15.0 && end.snr_db > 15.0;
      failures += !ok;
      printf("track %2u: %2d s, snr %5.1f dB (last %ld frames %5.1f dB) %s\n",
             cases[c].track, cases[c].seconds, whole.snr_db, tail,
             end.snr_db, ok ? "ok" : "FAIL");
    }
    free(pcm);
    free(ogg);
  }
  printf(failures ? "MSU1_PACK_FAIL\n" : "MSU1_PACK_PASS\n");
  return failures != 0;
}
