#ifndef DKC1_HD_GPU_H
#define DKC1_HD_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dkc1_hd.h"

/* GPU compositor for HD texture frames (dkc1_hd_gpu.c). Call with the
 * host's GL context current. `gles` selects GLSL ES 3.00 over 3.30 core. */
bool Dkc1HdGpuInit(bool gles);
bool Dkc1HdGpuReady(void);
/* Compose a frame (Dkc1HdGpuFrame) into an internal texture. */
bool Dkc1HdGpuCompose(const Dkc1HdGpuInputs *inputs);
/* Draw the last composed frame into framebuffer's viewport. */
void Dkc1HdGpuPresent(unsigned framebuffer, int x, int y, int width,
                      int height);
/* Draw a native 0x00RRGGBB frame the same way (hosts without their own
 * GL presenter, e.g. Switch). */
void Dkc1HdGpuPresentNative(const uint32_t *pixels, int width, int height,
                            size_t pitch_pixels, unsigned framebuffer, int x,
                            int y, int view_width, int view_height);
/* Blend 0xAARRGGBB pixels (menu, HUD) over the presented frame. */
void Dkc1HdGpuPresentOverlay(const uint32_t *pixels, int width, int height,
                             size_t pitch_pixels, unsigned framebuffer, int x,
                             int y, int view_width, int view_height);
/* Clear the whole framebuffer to black: what a narrower picture (4:3, a
 * resized viewport) leaves around itself must not keep older frames. */
void Dkc1HdGpuClearScreen(unsigned framebuffer, int width, int height);
/* Diagnostics (Switch bench): time each GPU stage of the last composed
 * frame, cut-down compose shaders included, into `out` as " name<ms>"
 * pairs. Returns the text length (0 when unavailable). */
int Dkc1HdGpuProfile(const Dkc1HdGpuInputs *inputs, int view_width,
                     int view_height, char *out, size_t out_size);
/* Wait for the GPU to finish all submitted work (timing diagnostics). */
void Dkc1HdGpuFinish(void);
/* Read the last composed frame back as 0x00RRGGBB rows (verification). */
bool Dkc1HdGpuReadComposite(uint32_t *out, int *width, int *height);
void Dkc1HdGpuShutdown(void);

#endif
