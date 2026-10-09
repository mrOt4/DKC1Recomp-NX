#include "switch_clock.h"

#ifdef __SWITCH__

#include <switch.h>

#include <stdio.h>

enum {
  kBoostHz = 1785000000u,
  kGpuHandheldHz = 460800000u,
  kGpuDockedHz = 768000000u,
};

static int AllowedCores(int cores[4]) {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
    return 0;
  int count = 0;
  for (int core = 0; core < 4; core++)
    if (mask & (1ull << core))
      cores[count++] = core;
  return count;
}

static void Pin(int core) {
  const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
  if (R_FAILED(rc))
    fprintf(stderr, "[cores] pin to core %d failed (0x%x)\n", core,
            (unsigned)rc);
}

void Dkc1SwitchPinMainThread(void) {
  int cores[4];
  const int count = AllowedCores(cores);
  if (count > 1) {
    Pin(cores[0]);
    fprintf(stderr, "[cores] %d usable; emulation on core %d\n", count,
            cores[0]);
  }
}

void Dkc1SwitchPinWorker(int index) {
  int cores[4];
  const int count = AllowedCores(cores);
  if (count > 1 && index > 0)
    Pin(cores[1 + (index - 1) % (count - 1)]);
}

static bool s_open, s_failed, s_clkrst, s_boosted;
static ClkrstSession s_session;
static u32 s_original_hz;
/* GPU: optional, the CPU boost works without it. */
static bool s_gpu_open;
static ClkrstSession s_gpu_session;
static u32 s_gpu_original_hz;

static void OpenGpu(void) {
  Result rc;
  if (s_clkrst) {
    rc = clkrstOpenSession(&s_gpu_session, PcvModuleId_GPU, 3);
    if (R_SUCCEEDED(rc)) {
      rc = clkrstGetClockRate(&s_gpu_session, &s_gpu_original_hz);
      if (R_FAILED(rc))
        clkrstCloseSession(&s_gpu_session);
    }
  } else {
    rc = pcvGetClockRate(PcvModule_GPU, &s_gpu_original_hz);
  }
  s_gpu_open = R_SUCCEEDED(rc) && s_gpu_original_hz;
  if (!s_gpu_open)
    fprintf(stderr, "[clock] GPU clock control unavailable (0x%x)\n",
            (unsigned)rc);
}

static u32 GpuRate(void) {
  u32 now = 0;
  if (s_clkrst)
    clkrstGetClockRate(&s_gpu_session, &now);
  else
    pcvGetClockRate(PcvModule_GPU, &now);
  return now;
}

static void SetGpu(u32 hz) {
  const Result rc = s_clkrst ? clkrstSetClockRate(&s_gpu_session, hz)
                             : pcvSetClockRate(PcvModule_GPU, hz);
  if (R_FAILED(rc))
    fprintf(stderr, "[clock] GPU set %u Hz failed (0x%x)\n", (unsigned)hz,
            (unsigned)rc);
}

/* The boosted GPU rate for the current mode; never below the current one. */
static u32 GpuTarget(void) {
  return appletGetOperationMode() == AppletOperationMode_Console
             ? kGpuDockedHz
             : kGpuHandheldHz;
}

static void BoostGpu(void) {
  if (!s_gpu_open)
    return;
  const u32 now = GpuRate();
  if (now && now < GpuTarget())
    SetGpu(GpuTarget());
}

/* clkrst replaced pcv's clock calls in 8.0.0. */
static bool Open(void) {
  if (s_open)
    return true;
  if (s_failed)
    return false;
  Result rc;
  s_clkrst = hosversionAtLeast(8, 0, 0);
  if (s_clkrst) {
    rc = clkrstInitialize();
    if (R_SUCCEEDED(rc)) {
      rc = clkrstOpenSession(&s_session, PcvModuleId_CpuBus, 3);
      if (R_SUCCEEDED(rc))
        rc = clkrstGetClockRate(&s_session, &s_original_hz);
      if (R_FAILED(rc)) {
        clkrstCloseSession(&s_session);
        clkrstExit();
      }
    }
  } else {
    rc = pcvInitialize();
    if (R_SUCCEEDED(rc)) {
      rc = pcvGetClockRate(PcvModule_CpuBus, &s_original_hz);
      if (R_FAILED(rc))
        pcvExit();
    }
  }
  if (R_FAILED(rc) || !s_original_hz) {
    fprintf(stderr, "[clock] CPU clock control unavailable (0x%x)\n",
            (unsigned)rc);
    s_failed = true;
    return false;
  }
  s_open = true;
  OpenGpu();
  return true;
}

static uint32_t Set(u32 hz) {
  Result rc = s_clkrst ? clkrstSetClockRate(&s_session, hz)
                       : pcvSetClockRate(PcvModule_CpuBus, hz);
  u32 now = 0;
  if (s_clkrst)
    clkrstGetClockRate(&s_session, &now);
  else
    pcvGetClockRate(PcvModule_CpuBus, &now);
  if (R_FAILED(rc))
    fprintf(stderr, "[clock] set %u Hz failed (0x%x)\n", (unsigned)hz,
            (unsigned)rc);
  return now / 1000000u;
}

uint32_t Dkc1SwitchClockBoost(bool enabled) {
  if (!Open())
    return 0;
  const u32 target =
      enabled && s_original_hz < kBoostHz ? kBoostHz : s_original_hz;
  s_boosted = target == kBoostHz;
  const uint32_t mhz = Set(target);
  if (s_gpu_open) {
    if (enabled)
      BoostGpu();
    else
      SetGpu(s_gpu_original_hz);
  }
  fprintf(stderr, "[clock] CPU %u MHz (boot %u MHz), GPU %u MHz (boot %u MHz)\n",
          (unsigned)mhz, (unsigned)(s_original_hz / 1000000u),
          (unsigned)(s_gpu_open ? GpuRate() / 1000000u : 0),
          (unsigned)(s_gpu_original_hz / 1000000u));
  return mhz;
}

void Dkc1SwitchClockMaintain(void) {
  if (!s_open || !s_boosted)
    return;
  u32 now = 0;
  if (s_clkrst)
    clkrstGetClockRate(&s_session, &now);
  else
    pcvGetClockRate(PcvModule_CpuBus, &now);
  if (now && now < kBoostHz)
    Set(kBoostHz);
  BoostGpu();  /* also follows dock changes: 768 MHz docked */
}

uint32_t Dkc1SwitchGpuClockMHz(void) {
  return s_gpu_open ? GpuRate() / 1000000u : 0;
}

void Dkc1SwitchClockExit(void) {
  if (!s_open)
    return;
  Set(s_original_hz);
  s_boosted = false;
  if (s_gpu_open) {
    SetGpu(s_gpu_original_hz);
    if (s_clkrst)
      clkrstCloseSession(&s_gpu_session);
    s_gpu_open = false;
  }
  if (s_clkrst) {
    clkrstCloseSession(&s_session);
    clkrstExit();
  } else {
    pcvExit();
  }
  s_open = false;
}

#endif
