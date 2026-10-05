#ifndef DKC1_WINDOWS_PLATFORM_H
#define DKC1_WINDOWS_PLATFORM_H
#include <SDL.h>
#include "desktop_graphics.h"
void Dkc1WindowsAttach(SDL_Window *window);
void Dkc1WindowsEvent(const SDL_Event *event);
void Dkc1WindowsDetach(void);
bool Dkc1WindowsGraphicsInit(SDL_Window *window);
void Dkc1WindowsGraphicsDraw(const uint32_t *pixels,int w,int h,int display_width,
                             const Dkc1GraphicsSettings *settings);
/* Presents an HD texture surface of w x h pixels as a logical
 * display_width x logical_height picture (dkc1_hd.c). */
void Dkc1WindowsGraphicsDrawHd(const uint32_t *pixels,int w,int h,int display_width,int logical_height);
void Dkc1WindowsGraphicsSwap(void);
void Dkc1WindowsGraphicsClose(void);
int Dkc1WindowsGraphicsTest(void);
int Dkc1WindowsPlatformTest(const char *directory);
#endif
