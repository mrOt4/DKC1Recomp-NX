#ifndef DKC1_OVERLAY_MENU_H
#define DKC1_OVERLAY_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* In-game menu drawn over the native frame, for hosts without native menus
 * (Switch). It only edits settings and reports actions; the host applies
 * them. The Switch host opens it with Plus + Minus or a right-stick click. */

typedef struct Dkc1MenuSettings {
  int slot;            /* save-state slot, 0..4 */
  bool rewind;         /* hold L to rewind, R to fast-forward */
  bool msu1;           /* replacement music (applies on restart) */
  bool hd;             /* HD textures */
  bool cheat_lives;    /* lives never drop below 5 */
  bool cheat_codes;    /* Pro Action Replay codes from cheats.txt */
  bool perf;           /* frame-time overlay and periodic log */
} Dkc1MenuSettings;

typedef enum Dkc1MenuAction {
  kDkc1MenuActionNone = 0,
  kDkc1MenuActionClose,
  kDkc1MenuActionSave,
  kDkc1MenuActionLoad,
  kDkc1MenuActionChanged,  /* a setting changed: apply and persist */
  kDkc1MenuActionQuit,
} Dkc1MenuAction;

void Dkc1MenuDefaults(Dkc1MenuSettings *settings);
/* key=value text file; missing keys keep their defaults. */
void Dkc1MenuLoadSettings(Dkc1MenuSettings *settings, const char *path);
bool Dkc1MenuSaveSettings(const Dkc1MenuSettings *settings, const char *path);

void Dkc1MenuOpen(void);
void Dkc1MenuClose(void);
bool Dkc1MenuIsOpen(void);
/* `pressed`: kDkc1Gamepad* buttons that went down this frame. */
Dkc1MenuAction Dkc1MenuUpdate(Dkc1MenuSettings *settings, uint32_t pressed);
/* Draw the menu into a 0xAARRGGBB overlay (cleared to 0 = transparent by
 * the caller). `info` is a status line (e.g. the last frame-time figures);
 * `message` a transient notice. */
void Dkc1MenuDraw(const Dkc1MenuSettings *settings, uint32_t *pixels,
                  int width, int height, size_t pitch, const char *info,
                  const char *message);
/* Pro Action Replay codes (AAAAAAVV, WRAM banks 7E/7F only), one per line;
 * '#' starts a comment. Returns the number of codes loaded. */
enum { kDkc1MenuMaxCheats = 64 };
typedef struct Dkc1Cheat {
  uint32_t address;  /* offset into the 128 KiB WRAM */
  uint8_t value;
} Dkc1Cheat;
int Dkc1MenuLoadCheats(const char *path, Dkc1Cheat *cheats, int capacity);
/* Apply the enabled cheats to WRAM; call once per frame before the game. */
void Dkc1MenuApplyCheats(const Dkc1MenuSettings *settings, uint8_t *wram,
                         const Dkc1Cheat *cheats, int count);

/* 8x8 text (ASCII) with a 1-pixel shadow, opaque, into an overlay. */
void Dkc1MenuDrawText(uint32_t *pixels, int width, int height, size_t pitch,
                      int x, int y, const char *text, uint32_t color);

/* Alpha-blend an overlay onto 0x00RRGGBB pixels (hosts without GL). */
void Dkc1MenuBlend(uint32_t *dst, size_t dst_pitch, const uint32_t *overlay,
                   size_t overlay_pitch, int width, int height);

#endif
