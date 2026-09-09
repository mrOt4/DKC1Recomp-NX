/* Nintendo Switch MSU-1 stub for DKC1Recomp.
 *
 * The desktop MSU-1 music player memory-maps PCM packs (sys/mman.h, absent
 * on newlib) and needs user-owned pack directories that have no Switch
 * UX (no file picker). The host only opens a pack when configured, which
 * never happens on Switch, so every entry point here is a safe no-op.
 * Only compiled on __SWITCH__; desktop builds keep dkc1_msu1.c.
 */
#ifdef __SWITCH__

#include "dkc1_msu1.h"

#include <stddef.h>

Dkc1Msu1 *Dkc1Msu1Open(const char *directory, char *error,
                       size_t error_size) {
  (void)directory;
  (void)error;
  (void)error_size;
  return NULL;
}

void Dkc1Msu1Close(Dkc1Msu1 *player) { (void)player; }

int Dkc1Msu1ApplySpcMusicMute(uint8_t *rom, size_t rom_size,
                              char *error, size_t error_size) {
  (void)rom;
  (void)rom_size;
  (void)error;
  (void)error_size;
  return 0;
}

void Dkc1Msu1ObserveMusicState(Dkc1Msu1 *player, uint16_t requested_theme,
                               uint16_t start_state) {
  (void)player;
  (void)requested_theme;
  (void)start_state;
}

void Dkc1Msu1Reset(Dkc1Msu1 *player) { (void)player; }

void Dkc1Msu1Mix(Dkc1Msu1 *player, int16_t *samples, int frames,
                 int channels, int output_rate) {
  (void)player;
  (void)samples;
  (void)frames;
  (void)channels;
  (void)output_rate;
}

unsigned Dkc1Msu1CurrentTrack(const Dkc1Msu1 *player) {
  (void)player;
  return 0;
}

const char *Dkc1Msu1Directory(const Dkc1Msu1 *player) {
  (void)player;
  return NULL;
}

#endif /* __SWITCH__ */
