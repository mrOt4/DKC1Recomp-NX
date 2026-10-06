#include "dkc1_msu1.h"

#include <SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __SWITCH__
#include "switch_clock.h"
#endif

/* Compressed packs (tools/msu1_compress.py): Ogg Vorbis tracks. A worker
 * thread reads the current track into memory and decodes it ahead into a
 * ring, so the frame-critical mixer never touches the disk or the decoder.
 * Header-only here; the implementation is built in runner/dkc1_stb_vorbis.c. */
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis/stb_vorbis.c"

#if defined(__SWITCH__)
/* newlib has no mmap: Switch plays compressed (.ogg) packs only. */
#define DKC1_MSU1_NO_MMAP 1
#endif

#ifdef DKC1_MSU1_NO_MMAP
#elif defined(_WIN32)
#include <windows.h>
#include <io.h>
#define open(path, flags) _open(path, (flags) | _O_BINARY)
#define close _close
#define fstat _fstat64
#define stat _stat64
#define PROT_READ 0
#define MAP_PRIVATE 0
#define MAP_FAILED ((void *)-1)
static void *mmap(void *address,size_t size,int protection,int flags,int fd,int offset) {
  (void)address; (void)protection; (void)flags; (void)offset;
  HANDLE mapping=CreateFileMappingW((HANDLE)_get_osfhandle(fd),NULL,PAGE_READONLY,0,0,NULL);
  if (!mapping) return MAP_FAILED;
  void *view=MapViewOfFile(mapping,FILE_MAP_READ,0,0,size);
  CloseHandle(mapping); return view ? view : MAP_FAILED;
}
static int munmap(void *address,size_t size) {
  (void)size; return UnmapViewOfFile(address) ? 0 : -1;
}
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum {
  kMsuPcmRate = 44100,    /* MSU-1 PCM is 44.1 kHz by definition */
  kMsuPcmHeaderSize = 8,
  kMsuMaximumTheme = 31,
  kMsuTrackCount = kMsuMaximumTheme + 1,
  kSpcMuteRomOffset = 0x0AA9E5,
  /* Decoded-ahead ring: half a second kept ready, decoded in small steps. */
  kMsuRingFrames = 1 << 15,
  kMsuRingAhead = 24000,
  kMsuDecodeFrames = 1024,
  kMsuBlockFrames = 512,  /* frames the mixer takes from the ring at once */
  /* Resampler: 16-tap Kaiser-windowed sinc, 128 phases interpolated. */
  kMsuTaps = 16,
  kMsuPhases = 128,
};

typedef struct Dkc1MsuTrack {
  const uint8_t *mapping;
  size_t mapping_size;
  uint32_t total_frames;
  uint32_t loop_frame;
  uint32_t rate;          /* source sample rate */
  int descriptor;
  bool present;
  bool vorbis;            /* compressed: decoded by the stream worker */
  char path[PATH_MAX];
} Dkc1MsuTrack;

/* Shared with the decode worker; every field below `lock` is guarded by it.
 * `request` names the track the main thread wants; the ring holds frames of
 * request `ring_request` only, so stale frames are never mixed. */
typedef struct MsuStream {
  SDL_Thread *thread;
  SDL_mutex *lock;
  SDL_cond *wake;
  bool quit;
  uint32_t request;
  const Dkc1MsuTrack *request_track;
  bool request_loop;
  uint32_t ring_request;
  uint32_t read, write;   /* free-running frame counts */
  bool ended;             /* no more frames for ring_request */
  int16_t ring[kMsuRingFrames * 2];
} MsuStream;

struct Dkc1Msu1 {
  char directory[PATH_MAX];
  Dkc1MsuTrack tracks[kMsuTrackCount];
  const Dkc1MsuTrack *track;
  uint32_t total_frames;
  uint32_t loop_frame;
  uint32_t source_frame;  /* PCM read position */
  uint16_t theme;
  unsigned track_number;
  MsuStream *stream;
  uint32_t request;       /* this thread's view of stream->request */
  int16_t block[kMsuBlockFrames * 2];
  int block_frames, block_pos;
  /* Resampler: the last kMsuTaps source frames (twice, so the window is
   * always contiguous), the fractional position in output-rate units, and
   * the phase table for the current rate pair. */
  float history[2][kMsuTaps * 2];
  int history_pos;
  uint32_t phase;
  uint32_t rate_in, rate_out;
  float taps[kMsuPhases + 1][kMsuTaps];
  double gain;
  bool loop;
  bool playing;
  bool music_state_valid;
  bool started;
};

/* Restoration playback policy, indexed by DKC's zero-based music ID. */
static const uint8_t kLoopTheme[27] = {
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1,
  1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0,
};

static void SetError(char *error, size_t error_size, const char *message) {
  if (error && error_size)
    snprintf(error, error_size, "%s", message ? message : "unknown error");
}

#ifndef DKC1_MSU1_NO_MMAP
static uint32_t ReadLittle32(const uint8_t bytes[4]) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}
#endif

static int16_t ReadLittle16(const uint8_t bytes[2]) {
  return (int16_t)(uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* ---- Decode worker ----------------------------------------------------- */

static uint8_t *LoadWholeFile(const char *path, int *size) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;
  uint8_t *data = NULL;
  long length = -1;
  if (fseek(file, 0, SEEK_END) == 0)
    length = ftell(file);
  if (length > 0 && length < INT_MAX && fseek(file, 0, SEEK_SET) == 0) {
    data = malloc((size_t)length);
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
      free(data);
      data = NULL;
    }
  }
  fclose(file);
  if (data)
    *size = (int)length;
  return data;
}

static int SDLCALL StreamWorker(void *arg) {
  MsuStream *stream = arg;
#ifdef __SWITCH__
  Dkc1SwitchPinWorker(2);  /* off the emulation core */
#endif
  uint32_t current = 0;
  const Dkc1MsuTrack *track = NULL;
  bool loop = false;
  uint8_t *data = NULL;
  stb_vorbis *decoder = NULL;
  uint32_t position = 0;
  static int16_t decoded[kMsuDecodeFrames * 2];

  SDL_LockMutex(stream->lock);
  for (;;) {
    while (!stream->quit && stream->request == current &&
           (stream->ended || !decoder ||
            stream->write - stream->read >= kMsuRingAhead))
      SDL_CondWaitTimeout(stream->wake, stream->lock, 50);
    if (stream->quit)
      break;
    if (stream->request != current) {
      /* New track: drop the old one and load this one outside the lock. */
      current = stream->request;
      track = stream->request_track;
      loop = stream->request_loop;
      stream->ring_request = current;
      stream->read = stream->write = 0;
      stream->ended = false;
      SDL_UnlockMutex(stream->lock);
      if (decoder)
        stb_vorbis_close(decoder);
      decoder = NULL;
      free(data);
      data = NULL;
      position = 0;
      int size = 0, error = 0;
      if (track && track->vorbis &&
          (data = LoadWholeFile(track->path, &size)) != NULL)
        decoder = stb_vorbis_open_memory(data, size, &error, NULL);
      SDL_LockMutex(stream->lock);
      if (stream->request == current && !decoder)
        stream->ended = true;
      continue;
    }
    SDL_UnlockMutex(stream->lock);
    uint32_t want = kMsuDecodeFrames;
    if (position + want > track->total_frames)
      want = track->total_frames - position;
    const int got = want ? stb_vorbis_get_samples_short_interleaved(
                               decoder, 2, decoded, (int)want * 2)
                         : 0;
    bool end = false;
    if (got > 0) {
      position += (uint32_t)got;
    } else if (loop && track->loop_frame < track->total_frames &&
               stb_vorbis_seek(decoder, track->loop_frame)) {
      position = track->loop_frame;
    } else {
      end = true;
    }
    SDL_LockMutex(stream->lock);
    if (stream->request != current)
      continue;
    if (end)
      stream->ended = true;
    for (int i = 0; i < got; i++) {
      const uint32_t at = (stream->write + (uint32_t)i) & (kMsuRingFrames - 1);
      stream->ring[at * 2] = decoded[i * 2];
      stream->ring[at * 2 + 1] = decoded[i * 2 + 1];
    }
    stream->write += (uint32_t)(got > 0 ? got : 0);
  }
  SDL_UnlockMutex(stream->lock);
  if (decoder)
    stb_vorbis_close(decoder);
  free(data);
  return 0;
}

static MsuStream *StartStream(void) {
  MsuStream *stream = calloc(1, sizeof *stream);
  if (!stream)
    return NULL;
  stream->lock = SDL_CreateMutex();
  stream->wake = SDL_CreateCond();
  if (stream->lock && stream->wake)
    stream->thread =
        SDL_CreateThread(StreamWorker, "DKC1 MSU-1", stream);
  if (!stream->thread) {
    if (stream->wake) SDL_DestroyCond(stream->wake);
    if (stream->lock) SDL_DestroyMutex(stream->lock);
    free(stream);
    return NULL;
  }
  return stream;
}

static void StopStream(MsuStream *stream) {
  if (!stream)
    return;
  SDL_LockMutex(stream->lock);
  stream->quit = true;
  SDL_CondSignal(stream->wake);
  SDL_UnlockMutex(stream->lock);
  SDL_WaitThread(stream->thread, NULL);
  SDL_DestroyCond(stream->wake);
  SDL_DestroyMutex(stream->lock);
  free(stream);
}

/* Ask the worker for `track` (NULL: stop). */
static void RequestStream(Dkc1Msu1 *player, const Dkc1MsuTrack *track,
                          bool loop) {
  MsuStream *stream = player->stream;
  if (!stream)
    return;
  SDL_LockMutex(stream->lock);
  stream->request++;
  stream->request_track = track;
  stream->request_loop = loop;
  player->request = stream->request;
  SDL_CondSignal(stream->wake);
  SDL_UnlockMutex(stream->lock);
  player->block_frames = player->block_pos = 0;
}

/* ---- Resampler ----------------------------------------------------------- */

static double BesselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 32; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}

/* taps[p][k]: weight of window frame k for an output at fraction p/P past
 * window frame kMsuTaps/2 - 1. Low-pass at 95 % of the lower Nyquist rate,
 * Kaiser beta 7 (about 70 dB stopband), unity DC gain per phase. */
static void BuildTaps(Dkc1Msu1 *player, uint32_t rate_in, uint32_t rate_out) {
  const double ratio = rate_out < rate_in ? (double)rate_out / rate_in : 1.0;
  const double cutoff = 0.5 * ratio * 0.95;  /* cycles per input sample */
  const double beta = 7.0, half = kMsuTaps / 2.0, norm = BesselI0(beta);
  for (int p = 0; p <= kMsuPhases; p++) {
    const double frac = (double)p / kMsuPhases;
    double sum = 0.0;
    for (int k = 0; k < kMsuTaps; k++) {
      const double x = (double)k - (half - 1.0) - frac;
      const double arg = 2.0 * cutoff * x;
      const double sinc =
          fabs(arg) < 1e-9 ? 1.0 : sin(M_PI * arg) / (M_PI * arg);
      const double w = x / half;
      const double window =
          fabs(w) >= 1.0 ? 0.0 : BesselI0(beta * sqrt(1.0 - w * w)) / norm;
      player->taps[p][k] = (float)(2.0 * cutoff * sinc * window);
      sum += player->taps[p][k];
    }
    for (int k = 0; k < kMsuTaps; k++)
      player->taps[p][k] = (float)(player->taps[p][k] / sum);
  }
  player->rate_in = rate_in;
  player->rate_out = rate_out;
}

static void ResetResampler(Dkc1Msu1 *player) {
  memset(player->history, 0, sizeof player->history);
  player->history_pos = 0;
  player->phase = 0;
}

static void PushHistory(Dkc1Msu1 *player, const int16_t sample[2]) {
  const int at = player->history_pos;
  for (int c = 0; c < 2; c++)
    player->history[c][at] = player->history[c][at + kMsuTaps] = sample[c];
  player->history_pos = (at + 1) % kMsuTaps;
}

/* ---- Source frames -------------------------------------------------------- */

static void CloseTrack(Dkc1Msu1 *player) {
  if (player->track && player->track->vorbis)
    RequestStream(player, NULL, false);
  player->track = NULL;
  player->playing = false;
  player->track_number = 0;
  player->total_frames = 0;
  player->source_frame = 0;
  player->block_frames = player->block_pos = 0;
  ResetResampler(player);
}

#ifndef DKC1_MSU1_NO_MMAP
static bool SeekFrame(Dkc1Msu1 *player, uint32_t frame) {
  if (!player->track || frame >= player->total_frames)
    return false;
  player->source_frame = frame;
  return true;
}
#endif

/* Next source frame: 1 = read, 0 = not decoded yet (try again next call),
 * -1 = the track is over. */
static int ReadFrame(Dkc1Msu1 *player, int16_t sample[2]) {
  if (!player->track)
    return -1;
  if (player->track->vorbis) {
    if (player->block_pos >= player->block_frames) {
      MsuStream *stream = player->stream;
      int result = -1;
      SDL_LockMutex(stream->lock);
      if (stream->ring_request == player->request) {
        uint32_t available = stream->write - stream->read;
        if (available > kMsuBlockFrames)
          available = kMsuBlockFrames;
        for (uint32_t i = 0; i < available; i++) {
          const uint32_t at = (stream->read + i) & (kMsuRingFrames - 1);
          player->block[i * 2] = stream->ring[at * 2];
          player->block[i * 2 + 1] = stream->ring[at * 2 + 1];
        }
        stream->read += available;
        player->block_frames = (int)available;
        player->block_pos = 0;
        result = available ? 1 : stream->ended ? -1 : 0;
        if (available && stream->write - stream->read < kMsuRingAhead)
          SDL_CondSignal(stream->wake);
      } else {
        result = 0;  /* the worker has not switched tracks yet */
      }
      SDL_UnlockMutex(stream->lock);
      if (result <= 0)
        return result;
    }
    sample[0] = player->block[player->block_pos * 2];
    sample[1] = player->block[player->block_pos * 2 + 1];
    player->block_pos++;
    return 1;
  }
#ifdef DKC1_MSU1_NO_MMAP
  return -1;
#else
  if (player->source_frame >= player->total_frames) {
    if (!player->loop || !SeekFrame(player, player->loop_frame))
      return -1;
  }
  const size_t offset =
      kMsuPcmHeaderSize + (size_t)player->source_frame * 4u;
  if (offset > player->track->mapping_size ||
      player->track->mapping_size - offset < 4u)
    return -1;
  const uint8_t *bytes = player->track->mapping + offset;
  sample[0] = ReadLittle16(bytes);
  sample[1] = ReadLittle16(bytes + 2);
  player->source_frame++;
  return 1;
#endif
}

static void UnmapTrack(Dkc1MsuTrack *track) {
  if (!track)
    return;
#ifndef DKC1_MSU1_NO_MMAP
  if (track->mapping && track->mapping_size)
    (void)munmap((void *)track->mapping, track->mapping_size);
  if (track->descriptor >= 0)
    (void)close(track->descriptor);
#endif
  *track = (Dkc1MsuTrack){.descriptor = -1};
}

/* Registers a compressed track: length from the stream, loop frame from the
 * MSU1_LOOP comment written by tools/msu1_compress.py. */
static int ProbeVorbisFile(const char *path, Dkc1MsuTrack *track) {
  FILE *probe = fopen(path, "rb");
  if (!probe)
    return 0;
  fclose(probe);
  int error = 0;
  stb_vorbis *decoder = stb_vorbis_open_filename(path, &error, NULL);
  if (!decoder)
    return -1;
  const stb_vorbis_info info = stb_vorbis_get_info(decoder);
  const unsigned frames = stb_vorbis_stream_length_in_samples(decoder);
  uint32_t loop = 0;
  const stb_vorbis_comment comments = stb_vorbis_get_comment(decoder);
  for (int i = 0; i < comments.comment_list_length; i++) {
    const char *entry = comments.comment_list[i];
    static const char kKey[] = "MSU1_LOOP=";
    size_t k = 0;
    while (kKey[k] && entry[k] &&
           (entry[k] == kKey[k] || entry[k] == kKey[k] + ('a' - 'A')))
      k++;
    if (!kKey[k])
      loop = (uint32_t)strtoul(entry + k, NULL, 10);
  }
  stb_vorbis_close(decoder);
  if (info.channels != 2 || info.sample_rate < 8000 ||
      info.sample_rate > 192000 || frames < 2 ||
      snprintf(track->path, sizeof track->path, "%s", path) >=
          (int)sizeof track->path)
    return -1;
  track->total_frames = frames;
  track->loop_frame = loop;
  track->rate = info.sample_rate;
  track->vorbis = true;
  track->present = true;
  return 1;
}

/* Map each PCM once during host startup. Playback then performs deterministic
 * pointer reads instead of tens of thousands of stdio calls on the frame-
 * critical thread. MADV_SEQUENTIAL lets the kernel keep the next PCM pages
 * ahead of the mixer without changing the sample timeline. */
#ifdef DKC1_MSU1_NO_MMAP
static int MapTrackFile(const char *path, Dkc1MsuTrack *track) {
  (void)path;
  (void)track;
  return 0;
}
#else
static int MapTrackFile(const char *path, Dkc1MsuTrack *track) {
  if (!path || !track)
    return -1;
  const int descriptor = open(path, O_RDONLY);
  if (descriptor < 0)
    return errno == ENOENT ? 0 : -1;
  struct stat info;
  if (fstat(descriptor, &info) != 0 ||
      info.st_size < kMsuPcmHeaderSize + 4 ||
      (uintmax_t)info.st_size > (uintmax_t)SIZE_MAX) {
    (void)close(descriptor);
    return -1;
  }
  const size_t mapping_size = (size_t)info.st_size;
  const uint8_t *mapping = mmap(
      NULL, mapping_size, PROT_READ, MAP_PRIVATE, descriptor, 0);
  if (mapping == MAP_FAILED) {
    (void)close(descriptor);
    return -1;
  }
  if (memcmp(mapping, "MSU1", 4) != 0) {
    (void)munmap((void *)mapping, mapping_size);
    (void)close(descriptor);
    return -1;
  }
#ifdef F_RDAHEAD
  (void)fcntl(descriptor, F_RDAHEAD, 1);
#endif
#ifdef MADV_SEQUENTIAL
  (void)madvise((void *)mapping, mapping_size, MADV_SEQUENTIAL);
#endif
  track->mapping = mapping;
  track->mapping_size = mapping_size;
  track->total_frames =
      (uint32_t)((mapping_size - kMsuPcmHeaderSize) / 4u);
  track->loop_frame = ReadLittle32(mapping + 4);
  track->rate = kMsuPcmRate;
  track->descriptor = descriptor;
  track->present = true;
  return 1;
}
#endif

static int CacheTrack(Dkc1Msu1 *player, unsigned track_number) {
  if (!player || track_number == 0 || track_number > kMsuTrackCount)
    return -1;
  Dkc1MsuTrack *track = &player->tracks[track_number - 1u];
  /* Uncompressed PCM first (exact original), then compressed Vorbis. */
  const char *patterns[] = {"%s/track-%u.pcm", "%s/dkc_msu-%u.pcm",
                            "%s/track-%u.ogg", "%s/dkc_msu-%u.ogg"};
  char path[PATH_MAX];
  for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; i++) {
    if (snprintf(path, sizeof path, patterns[i], player->directory,
                 track_number) >= (int)sizeof path)
      continue;
    const int found = i < 2 ? MapTrackFile(path, track)
                            : ProbeVorbisFile(path, track);
    if (found != 0)
      return found;
  }
  return 0;
}

static bool OpenTrack(Dkc1Msu1 *player, unsigned theme) {
  CloseTrack(player);
  if (theme > kMsuMaximumTheme)
    return false;

  const unsigned track = theme + 1;
  const Dkc1MsuTrack *cached = &player->tracks[track - 1u];
  if (!cached->present || (cached->vorbis && !player->stream))
    return false;
  player->track = cached;
  player->total_frames = cached->total_frames;
  player->loop_frame = cached->loop_frame;
  player->loop = theme < sizeof kLoopTheme && kLoopTheme[theme] != 0 &&
                 player->loop_frame < player->total_frames;
  player->track_number = track;
  player->source_frame = 0;
  ResetResampler(player);
  if (cached->vorbis)
    RequestStream(player, cached, player->loop);
  player->playing = true;
  return true;
}

Dkc1Msu1 *Dkc1Msu1Open(const char *directory, char *error,
                        size_t error_size) {
  if (!directory || !*directory) {
    SetError(error, error_size, "MSU-1 directory is empty");
    return NULL;
  }
  Dkc1Msu1 *player = calloc(1, sizeof *player);
  if (!player) {
    SetError(error, error_size, "out of memory opening MSU-1 pack");
    return NULL;
  }
  if (snprintf(player->directory, sizeof player->directory, "%s", directory) >=
      (int)sizeof player->directory) {
    SetError(error, error_size, "MSU-1 directory path is too long");
    free(player);
    return NULL;
  }
  for (unsigned track = 0; track < kMsuTrackCount; track++)
    player->tracks[track].descriptor = -1;
  for (unsigned track = 1; track <= kMsuTrackCount; track++)
    (void)CacheTrack(player, track);
  if (!player->tracks[0].present) {
    SetError(error, error_size, "MSU-1 pack has no valid track 1");
    Dkc1Msu1Close(player);
    return NULL;
  }
  bool any_vorbis = false;
  for (unsigned track = 0; track < kMsuTrackCount; track++)
    any_vorbis |= player->tracks[track].vorbis;
  if (any_vorbis && !(player->stream = StartStream())) {
    SetError(error, error_size, "cannot start the MSU-1 decode thread");
    Dkc1Msu1Close(player);
    return NULL;
  }
  player->gain = 1.0;
  const char *gain = getenv("DKC1_MSU1_GAIN");
  if (gain && *gain) {
    char *end = NULL;
    const double parsed = strtod(gain, &end);
    if (end && !*end && parsed >= 0.0 && parsed <= 4.0)
      player->gain = parsed;
  }
  SetError(error, error_size, "");
  return player;
}

void Dkc1Msu1Close(Dkc1Msu1 *player) {
  if (!player)
    return;
  CloseTrack(player);
  StopStream(player->stream);
  for (unsigned track = 0; track < kMsuTrackCount; track++)
    UnmapTrack(&player->tracks[track]);
  free(player);
}

int Dkc1Msu1ApplySpcMusicMute(uint8_t *rom, size_t rom_size,
                              char *error, size_t error_size) {
  /* Source bytes at HiROM $CAA9E5 in the checksum-locked USA v1.0 ROM. */
  static const uint8_t expected[2] = {0x01, 0xD4};
  static const uint8_t replacement[2] = {0x00, 0x6F};
  if (!rom || rom_size <= kSpcMuteRomOffset + 1) {
    SetError(error, error_size, "verified ROM is too small for MSU-1 mute");
    return 0;
  }
  if (memcmp(rom + kSpcMuteRomOffset, expected, sizeof expected) != 0) {
    SetError(error, error_size,
             "verified ROM does not match the MSU-1 SPC mute source bytes");
    return 0;
  }
  memcpy(rom + kSpcMuteRomOffset, replacement, sizeof replacement);
  SetError(error, error_size, "");
  return 1;
}

void Dkc1Msu1ObserveMusicState(Dkc1Msu1 *player, uint16_t requested_theme,
                               uint16_t start_state) {
  if (!player)
    return;

  const bool started = start_state != 0;
  const bool request_changed =
      !player->music_state_valid || player->theme != requested_theme;
  const bool start_changed =
      !player->music_state_valid || player->started != started;
  player->theme = requested_theme;
  player->started = started;

  if (!started) {
    if (start_changed || player->playing)
      CloseTrack(player);
  } else if (request_changed || start_changed) {
    (void)OpenTrack(player, requested_theme);
  }
  player->music_state_valid = true;
}

void Dkc1Msu1Reset(Dkc1Msu1 *player) {
  if (player) {
    CloseTrack(player);
    player->music_state_valid = false;
    player->started = false;
  }
}

static int16_t Saturate16(int value) {
  if (value > INT16_MAX)
    return INT16_MAX;
  if (value < INT16_MIN)
    return INT16_MIN;
  return (int16_t)value;
}

void Dkc1Msu1Mix(Dkc1Msu1 *player, int16_t *samples, int frames,
                  int channels, int output_rate) {
  if (!player || !player->playing || !samples || frames <= 0 ||
      channels != 2 || output_rate <= 0)
    return;
  const uint32_t rate_in = player->track->rate;
  if (rate_in != player->rate_in || (uint32_t)output_rate != player->rate_out)
    BuildTaps(player, rate_in, (uint32_t)output_rate);

  for (int frame = 0; frame < frames; frame++) {
    /* Output at fraction phase/output_rate past window frame taps/2 - 1:
     * blend the two nearest phase tables. */
    const uint64_t scaled = (uint64_t)player->phase * kMsuPhases;
    const int p = (int)(scaled / (uint32_t)output_rate);
    const float t =
        (float)(scaled % (uint32_t)output_rate) / (float)output_rate;
    const float *a = player->taps[p], *b = player->taps[p + 1];
    for (int channel = 0; channel < 2; channel++) {
      const float *window = &player->history[channel][player->history_pos];
      float acc = 0.0f;
      for (int k = 0; k < kMsuTaps; k++)
        acc += (a[k] + (b[k] - a[k]) * t) * window[k];
      const int external = (int)lrintf(acc * (float)player->gain);
      const int index = frame * channels + channel;
      samples[index] = Saturate16((int)samples[index] + external);
    }

    player->phase += rate_in;
    while (player->phase >= (uint32_t)output_rate) {
      int16_t sample[2];
      const int read = ReadFrame(player, sample);
      if (read == 0) {
        /* Not decoded yet (track start): hold the position; try again on
         * the next call rather than inventing samples. */
        player->phase -= rate_in;
        return;
      }
      if (read < 0) {
        player->playing = false;
        return;
      }
      player->phase -= (uint32_t)output_rate;
      PushHistory(player, sample);
    }
  }
}

/* Tools and tests: wait until the current track has frames ready. */
bool Dkc1Msu1WaitReady(Dkc1Msu1 *player, int timeout_ms) {
  if (!player || !player->playing)
    return false;
  if (!player->track->vorbis || !player->stream)
    return true;
  for (int waited = 0; waited <= timeout_ms; waited += 2) {
    MsuStream *stream = player->stream;
    SDL_LockMutex(stream->lock);
    const bool ready = stream->ring_request == player->request &&
                       (stream->ended ||
                        stream->write - stream->read >= kMsuRingAhead);
    SDL_UnlockMutex(stream->lock);
    if (ready)
      return true;
    SDL_Delay(2);
  }
  return false;
}

unsigned Dkc1Msu1CurrentTrack(const Dkc1Msu1 *player) {
  return player ? player->track_number : 0;
}

const char *Dkc1Msu1Directory(const Dkc1Msu1 *player) {
  return player ? player->directory : "";
}
