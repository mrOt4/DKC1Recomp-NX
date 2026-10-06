#include "switch_audio.h"

#ifdef __SWITCH__

#include <switch.h>

#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum {
  kBuffers = 8,  /* 40 ms queued in audren at most */
  kRingFrames = 1 << 14,
  kFeederPriority = 0x2B,  /* above the main thread's 0x2C */
  kFeederStack = 0x8000,
};

static const AudioRendererConfig kConfig = {
  .output_rate = AudioRendererOutputRate_48kHz,
  .num_voices = 2,
  .num_effects = 0,
  .num_sinks = 1,
  .num_mix_objs = 1,
  .num_mix_buffers = 2,
};

static struct {
  bool open;
  AudioDriver driver;
  AudioDriverWaveBuf buffers[kBuffers];
  int16_t *pool;
  size_t pool_size;
  int next;  /* the buffer to refill next; buffers play in this order */
  Thread thread;
  volatile bool quit;
  /* Ring, guarded by `lock`. Counts run free; index with & (kRingFrames-1). */
  Mutex lock;
  int16_t ring[kRingFrames * 2];
  uint32_t read, write;
  bool paused;
  uint32_t underruns;
} s;

/* Diagnostic capture (Dkc1SwitchAudioCaptureStart). */
static struct {
  int16_t *queued, *played;
  uint32_t frames;
  uint32_t queued_n, played_n;  /* played_n: written by the feeder only */
  volatile bool on;
} s_capture;

static uint32_t Fill(int16_t *out) {
  uint32_t taken = 0;
  mutexLock(&s.lock);
  if (!s.paused) {
    taken = s.write - s.read;
    if (taken > kDkc1SwitchAudioBufferFrames)
      taken = kDkc1SwitchAudioBufferFrames;
    for (uint32_t i = 0; i < taken; i++) {
      const uint32_t at = (s.read + i) & (kRingFrames - 1);
      out[i * 2] = s.ring[at * 2];
      out[i * 2 + 1] = s.ring[at * 2 + 1];
    }
    s.read += taken;
    if (taken < kDkc1SwitchAudioBufferFrames && s.write != 0)
      s.underruns++;
  }
  mutexUnlock(&s.lock);
  memset(out + taken * 2, 0,
         (kDkc1SwitchAudioBufferFrames - taken) * 2 * sizeof *out);
  if (s_capture.on && s_capture.played_n < s_capture.frames) {
    uint32_t n = kDkc1SwitchAudioBufferFrames;
    if (n > s_capture.frames - s_capture.played_n)
      n = s_capture.frames - s_capture.played_n;
    memcpy(s_capture.played + (size_t)s_capture.played_n * 2, out,
           (size_t)n * 4);
    __atomic_store_n(&s_capture.played_n, s_capture.played_n + n,
                     __ATOMIC_RELEASE);
  }
  return taken;
}

static void Feeder(void *unused) {
  (void)unused;
  while (!s.quit) {
    /* Refill, in play order, every buffer audren has finished with. */
    for (int n = 0; n < kBuffers; n++) {
      AudioDriverWaveBuf *buffer = &s.buffers[s.next];
      if (buffer->state != AudioDriverWaveBufState_Free &&
          buffer->state != AudioDriverWaveBufState_Done)
        break;
      int16_t *samples = s.pool + (size_t)s.next *
                                      kDkc1SwitchAudioBufferFrames * 2;
      Fill(samples);
      armDCacheFlush(samples, kDkc1SwitchAudioBufferFrames * 4);
      audrvVoiceAddWaveBuf(&s.driver, 0, buffer);
      s.next = (s.next + 1) % kBuffers;
    }
    if (!audrvVoiceIsPlaying(&s.driver, 0))
      audrvVoiceStart(&s.driver, 0);
    audrvUpdate(&s.driver);
    audrenWaitFrame();
  }
}

/* The last core the process may use: never the emulation core (the first). */
static int FeederCore(void) {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
    return -2;
  for (int core = 3; core >= 0; core--)
    if (mask & (1ull << core))
      return core;
  return -2;
}

bool Dkc1SwitchAudioOpen(void) {
  if (s.open)
    return true;
  memset(&s, 0, sizeof s);
  mutexInit(&s.lock);
  s.paused = true;
  Result rc = audrenInitialize(&kConfig);
  if (R_FAILED(rc)) {
    fprintf(stderr, "[audio] audrenInitialize failed (0x%x)\n", (unsigned)rc);
    return false;
  }
  rc = audrvCreate(&s.driver, &kConfig, 2);
  if (R_FAILED(rc)) {
    fprintf(stderr, "[audio] audrvCreate failed (0x%x)\n", (unsigned)rc);
    audrenExit();
    return false;
  }
  s.pool_size = ((size_t)kBuffers * kDkc1SwitchAudioBufferFrames * 4 + 0xfff) &
                ~(size_t)0xfff;
  s.pool = memalign(0x1000, s.pool_size);
  if (!s.pool) {
    audrvClose(&s.driver);
    audrenExit();
    return false;
  }
  memset(s.pool, 0, s.pool_size);
  for (int i = 0; i < kBuffers; i++) {
    s.buffers[i].data_raw = s.pool;
    s.buffers[i].size = s.pool_size;
    s.buffers[i].start_sample_offset = i * kDkc1SwitchAudioBufferFrames;
    s.buffers[i].end_sample_offset =
        s.buffers[i].start_sample_offset + kDkc1SwitchAudioBufferFrames;
  }
  const int pool_id = audrvMemPoolAdd(&s.driver, s.pool, s.pool_size);
  audrvMemPoolAttach(&s.driver, pool_id);
  static const u8 kSinkChannels[] = {0, 1};
  audrvDeviceSinkAdd(&s.driver, AUDREN_DEFAULT_DEVICE_NAME, 2, kSinkChannels);
  rc = audrenStartAudioRenderer();
  if (R_FAILED(rc)) {
    fprintf(stderr, "[audio] audrenStartAudioRenderer failed (0x%x)\n",
            (unsigned)rc);
    Dkc1SwitchAudioClose();
    return false;
  }
  audrvVoiceInit(&s.driver, 0, 2, PcmFormat_Int16, kDkc1SwitchAudioRate);
  audrvVoiceSetDestinationMix(&s.driver, 0, AUDREN_FINAL_MIX_ID);
  audrvVoiceSetMixFactor(&s.driver, 0, 1.0f, 0, 0);
  audrvVoiceSetMixFactor(&s.driver, 0, 0.0f, 0, 1);
  audrvVoiceSetMixFactor(&s.driver, 0, 0.0f, 1, 0);
  audrvVoiceSetMixFactor(&s.driver, 0, 1.0f, 1, 1);
  audrvVoiceStart(&s.driver, 0);
  s.open = true;
  const int core = FeederCore();
  rc = threadCreate(&s.thread, Feeder, NULL, NULL, kFeederStack,
                    kFeederPriority, core);
  if (R_SUCCEEDED(rc))
    rc = threadStart(&s.thread);
  if (R_FAILED(rc)) {
    fprintf(stderr, "[audio] feeder thread failed (0x%x)\n", (unsigned)rc);
    Dkc1SwitchAudioClose();
    return false;
  }
  fprintf(stderr, "[audio] audren 48 kHz, %d x %d frames, feeder core %d\n",
          kBuffers, kDkc1SwitchAudioBufferFrames, core);
  return true;
}

void Dkc1SwitchAudioClose(void) {
  if (s.thread.handle) {
    s.quit = true;
    threadWaitForExit(&s.thread);
    threadClose(&s.thread);
  }
  if (s.open) {
    audrvVoiceStop(&s.driver, 0);
    audrvClose(&s.driver);
    audrenExit();
  }
  free(s.pool);
  s.pool = NULL;
  s.open = false;
}

void Dkc1SwitchAudioPause(bool paused) {
  mutexLock(&s.lock);
  s.paused = paused;
  mutexUnlock(&s.lock);
}

int Dkc1SwitchAudioQueue(const void *data, uint32_t bytes) {
  const int16_t *in = data;
  const uint32_t frames = bytes / 4;
  int result = 0;
  mutexLock(&s.lock);
  if (kRingFrames - (s.write - s.read) < frames) {
    result = -1;
  } else {
    for (uint32_t i = 0; i < frames; i++) {
      const uint32_t at = (s.write + i) & (kRingFrames - 1);
      s.ring[at * 2] = in[i * 2];
      s.ring[at * 2 + 1] = in[i * 2 + 1];
    }
    s.write += frames;
    if (s_capture.on && s_capture.queued_n < s_capture.frames) {
      uint32_t n = frames;
      if (n > s_capture.frames - s_capture.queued_n)
        n = s_capture.frames - s_capture.queued_n;
      memcpy(s_capture.queued + (size_t)s_capture.queued_n * 2, in,
             (size_t)n * 4);
      s_capture.queued_n += n;
    }
  }
  mutexUnlock(&s.lock);
  return result;
}

uint32_t Dkc1SwitchAudioQueuedBytes(void) {
  mutexLock(&s.lock);
  const uint32_t frames = s.write - s.read;
  mutexUnlock(&s.lock);
  return frames * 4;
}

void Dkc1SwitchAudioClear(void) {
  mutexLock(&s.lock);
  s.read = s.write;
  mutexUnlock(&s.lock);
}

uint32_t Dkc1SwitchAudioUnderruns(void) {
  mutexLock(&s.lock);
  const uint32_t underruns = s.underruns;
  mutexUnlock(&s.lock);
  return underruns;
}

bool Dkc1SwitchAudioCaptureStart(uint32_t frames) {
  if (s_capture.on || s_capture.queued)
    return false;
  s_capture.queued = malloc((size_t)frames * 4);
  s_capture.played = malloc((size_t)frames * 4);
  if (!s_capture.queued || !s_capture.played) {
    free(s_capture.queued);
    free(s_capture.played);
    s_capture.queued = s_capture.played = NULL;
    return false;
  }
  s_capture.frames = frames;
  s_capture.queued_n = s_capture.played_n = 0;
  __atomic_store_n(&s_capture.on, true, __ATOMIC_RELEASE);
  return true;
}

static bool WriteRaw(const char *path, const int16_t *data, uint32_t frames) {
  FILE *file = fopen(path, "wb");
  if (!file)
    return false;
  const bool ok = fwrite(data, 4, frames, file) == frames;
  return fclose(file) == 0 && ok;
}

bool Dkc1SwitchAudioCaptureDump(const char *queued_path,
                                const char *played_path) {
  if (!s_capture.on)
    return false;
  mutexLock(&s.lock);
  const bool queued_full = s_capture.queued_n >= s_capture.frames;
  mutexUnlock(&s.lock);
  if (!queued_full ||
      __atomic_load_n(&s_capture.played_n, __ATOMIC_ACQUIRE) <
          s_capture.frames)
    return false;
  s_capture.on = false;
  const bool ok = WriteRaw(queued_path, s_capture.queued, s_capture.frames) &&
                  WriteRaw(played_path, s_capture.played, s_capture.frames);
  fprintf(stderr, "[audio-dump] %u frames -> %s, %s: %s\n",
          (unsigned)s_capture.frames, queued_path, played_path,
          ok ? "ok" : "FAILED");
  return true;
}

#endif
