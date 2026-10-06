#ifndef DKC1_SWITCH_AUDIO_H
#define DKC1_SWITCH_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* Audio output for Switch, replacing SDL's queued audio device.
 *
 * SDL 2.28's Switch driver runs its feeder thread at priority 0x3B (it maps
 * every request but SDL_THREAD_PRIORITY_HIGH there, the lowest level) on the
 * process's default core, which is the emulation core, and gives audren only
 * two buffers. That thread then runs only while the emulation thread sleeps,
 * and any long frame leaves audren without data: audible crackle. Here a
 * feeder above the emulation thread's priority, on another core, keeps eight
 * 5 ms audren buffers filled from a ring that the host fills.
 *
 * The functions mirror the SDL queue calls the host uses: Queue appends
 * 16-bit stereo frames at kDkc1SwitchAudioRate, QueuedBytes reports what has
 * not yet been handed to audren, Clear drops it, and a paused device plays
 * silence without consuming the ring. */
enum {
  kDkc1SwitchAudioRate = 48000,
  kDkc1SwitchAudioBufferFrames = 240,  /* one audren frame (5 ms) */
};

/* Returns false (with a message on stderr) if audren cannot be started. */
bool Dkc1SwitchAudioOpen(void);
void Dkc1SwitchAudioClose(void);
void Dkc1SwitchAudioPause(bool paused);
/* 0 on success, -1 when the ring has no room (the data is dropped). */
int Dkc1SwitchAudioQueue(const void *data, uint32_t bytes);
uint32_t Dkc1SwitchAudioQueuedBytes(void);
void Dkc1SwitchAudioClear(void);
/* Buffers audren had to play as silence because the ring ran dry. */
uint32_t Dkc1SwitchAudioUnderruns(void);

/* Diagnostics: record the next `frames` frames both as queued by the host
 * and as handed to audren (silence included). Dump writes the two raw
 * 16-bit stereo 48 kHz files once both recordings are complete and returns
 * true that one time. */
bool Dkc1SwitchAudioCaptureStart(uint32_t frames);
bool Dkc1SwitchAudioCaptureDump(const char *queued_path,
                                const char *played_path);

#endif
