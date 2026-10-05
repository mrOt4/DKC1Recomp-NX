/* Nintendo Switch platform layer for DKC1Recomp (libnx + SDL2).
 *
 * sdl_host.c drives the game through the Dkc1Mac* platform surface, with
 * per-OS implementations (macos_*.m on Apple, windows_platform.c on
 * Windows). This file is the Switch implementation: no-file-picker ROM
 * flow (sdmc:/switch/dkc1/rom.smc), fixed handheld settings, and no-op
 * native menus/launchers. Only compiled on __SWITCH__; desktop builds
 * keep their own platform files.
 *
 * Only this translation unit and sdl_host.c's __SWITCH__ blocks know
 * about Horizon; everything else stays portable.
 */
#ifdef __SWITCH__

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desktop_graphics.h"
#include "desktop_input.h"
#include "dkc1_edge_policy.h"
#include "macos_controls.h"
#include "macos_file_picker.h"
#include "macos_metal_presenter.h"
#include "macos_pause_menu.h"

/* ROM and content pickers: the Switch build boots straight from the SD
 * app dir (see SwitchImpl_ResolveRom) and never prompts. */
char *Dkc1MacChooseRom(void) { return NULL; }
char *Dkc1MacChooseBabyKongRom(void) { return NULL; }
char *Dkc1MacSavedBabyKongRom(void) { return NULL; }
void Dkc1MacSetBabyKongRom(const char *path) { (void)path; }
int Dkc1MacSavedBabyKongEnabled(void) { return 0; }
void Dkc1MacSetBabyKongEnabled(int enabled) { (void)enabled; }
/* The Switch build enables HD from sdmc:/switch/dkc1/hd/ (dkc1_hd.c). */
char *Dkc1MacChooseHdPack(void) { return NULL; }
char *Dkc1MacSavedHdPack(void) { return NULL; }
void Dkc1MacSetHdPack(const char *path) { (void)path; }
int Dkc1MacSavedHdEnabled(void) { return 1; }
void Dkc1MacSetHdEnabled(int enabled) { (void)enabled; }
char *Dkc1MacChooseMsu1(void) { return NULL; }
/* No picker on Switch: a compressed pack (tools/msu1_compress.py) copied to
 * sdmc:/switch/dkc1/msu1/ enables replacement music. Only .ogg tracks play
 * here (no mmap for raw .pcm packs). */
char *Dkc1MacSavedMsu1(void) {
  static const char *const kProbes[] = {"msu1/track-1.ogg",
                                        "msu1/dkc_msu-1.ogg"};
  for (size_t i = 0; i < sizeof kProbes / sizeof kProbes[0]; i++) {
    FILE *probe = fopen(kProbes[i], "rb");
    if (probe) {
      fclose(probe);
      char *path = malloc(5);
      if (path) memcpy(path, "msu1", 5);
      return path;
    }
  }
  return NULL;
}
void Dkc1MacClearMsu1(void) {}

Dkc1MacFullscreenScaling Dkc1MacSavedFullscreenScaling(void) {
  return kDkc1MacFullscreenSharpBilinear;
}
void Dkc1MacSetFullscreenScaling(Dkc1MacFullscreenScaling scaling) {
  (void)scaling;
}
Dkc1EdgePolicy Dkc1MacSavedWidescreenEdge(void) { return kDkc1EdgeGlide; }
void Dkc1MacSetWidescreenEdge(Dkc1EdgePolicy policy) { (void)policy; }

/* Graphics settings: desktop defaults (16:9, bilinear buddy) with no
 * persistence yet. There is no settings UI on Switch, so there is
 * nothing to persist back. */
void Dkc1MacLoadGraphics(Dkc1GraphicsSettings *settings) {
  if (!settings) return;
  Dkc1GraphicsDefault(settings);
}
void Dkc1MacSaveGraphics(const Dkc1GraphicsSettings *settings) {
  (void)settings;
}
void Dkc1MacInstallMenu(void) {}
void Dkc1MacUpdateGraphicsMenuState(int display, int upscaler, int screen) {
  (void)display;
  (void)upscaler;
  (void)screen;
}
void Dkc1MacUpdateMenuState(int paused, int fullscreen,
                            Dkc1MacFullscreenScaling fullscreen_scaling,
                            Dkc1VideoAspect aspect, Dkc1EdgePolicy edge,
                            unsigned char layer_mask, int provenance,
                            int replacement_music, int baby_kong_enabled,
                            int baby_kong_ready, int hd_enabled,
                            int hd_ready) {
  (void)paused;
  (void)fullscreen;
  (void)fullscreen_scaling;
  (void)aspect;
  (void)edge;
  (void)layer_mask;
  (void)provenance;
  (void)replacement_music;
  (void)baby_kong_enabled;
  (void)baby_kong_ready;
  (void)hd_enabled;
  (void)hd_ready;
}

/* Fixed handheld controls: both players on gamepads with the straight
 * Switch layout (labels already sit at the SNES positions, unlike the
 * crossed Xbox-physical desktop defaults). ABXY at face value, ZL/ZR as
 * L/R triggers, Plus as Start, Minus as Select. Assist shortcuts are
 * cleared so triggers never double as rewind/fast-forward. */
void Dkc1MacLoadControls(Dkc1Controls *controls) {
  static const int kSwitchPads[12] = {
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_UP),
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_DOWN),
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_LEFT),
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_A),
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_B),
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_X),
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_Y),
      DKC1_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1),
      DKC1_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1),
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_START),
      DKC1_PAD_BUTTON(SDL_CONTROLLER_BUTTON_BACK),
  };
  int player;

  if (!controls) return;
  memset(controls, 0, sizeof *controls);
  controls->source[0] = 2;
  controls->source[1] = 2;
  for (player = 0; player < 2; player++) {
    controls->deadzone[player] = 25;
    memcpy(controls->pads[player], kSwitchPads, sizeof kSwitchPads);
  }
}
void Dkc1MacSaveControls(const Dkc1Controls *controls) {
  (void)controls;
}
int Dkc1MacEditControls(Dkc1Controls *controls) {
  (void)controls;
  return 0;
}

/* Pause menu: stubbed (no ImGui/natural UI on Switch). The host's pause
 * chord degrades to a no-op overlay check. */
int Dkc1MacShowPauseMenu(void *window, Dkc1GraphicsSettings *settings,
                         Dkc1Controls *controls, int graphics_page) {
  (void)window;
  (void)settings;
  (void)controls;
  (void)graphics_page;
  return 0;
}
int Dkc1MacPauseMenuIsOpen(void) { return 0; }

/* Display link + Metal presenter: unavailable (host-clock pacing and the
 * SDL GLES2 renderer own the frame instead). */
int Dkc1MacDisplayLinkStart(void *native_window, double preferred_fps) {
  (void)native_window;
  (void)preferred_fps;
  return 0;
}
int Dkc1MacDisplayLinkWait(unsigned long long after_callback_number,
                           double timeout_seconds, double *timestamp,
                           double *target_timestamp, double *duration,
                           unsigned long long *callback_number) {
  (void)after_callback_number;
  (void)timeout_seconds;
  (void)timestamp;
  (void)target_timestamp;
  (void)duration;
  (void)callback_number;
  return 0;
}
void Dkc1MacDisplayLinkStop(void) {}
int Dkc1MacMetalPresenterStart(void *native_window, double preferred_hz,
                               Dkc1MacFullscreenScaling scaling,
                               int fullscreen) {
  (void)native_window;
  (void)preferred_hz;
  (void)scaling;
  (void)fullscreen;
  return 0;
}
void Dkc1MacMetalPresenterQueueFrame(
    const uint32_t *pixels, int width, int height, int presentation_width,
    const Dkc1MacPresentationFrameInfo *info) {
  (void)pixels;
  (void)width;
  (void)height;
  (void)presentation_width;
  (void)info;
}
void Dkc1MacMetalPresenterQueueHdFrame(
    const uint32_t *pixels, int width, int height, int presentation_width,
    int logical_height, const Dkc1MacPresentationFrameInfo *info) {
  (void)pixels;
  (void)width;
  (void)height;
  (void)presentation_width;
  (void)logical_height;
  (void)info;
}
void Dkc1MacMetalPresenterSetGeometry(int presentation_width, int fullscreen) {
  (void)presentation_width;
  (void)fullscreen;
}
void Dkc1MacMetalPresenterSetScaling(Dkc1MacFullscreenScaling scaling) {
  (void)scaling;
}
void Dkc1MacMetalPresenterSetActive(int active) { (void)active; }
void Dkc1MacMetalPresenterFlush(void) {}
void Dkc1MacMetalPresenterStop(void) {}
void Dkc1MacMetalPresenterSetGraphics(const Dkc1GraphicsSettings *settings) {
  (void)settings;
}

#endif /* __SWITCH__ */
