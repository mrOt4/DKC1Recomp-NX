#include "dkc1_overlay_menu.h"

#include "desktop_input.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public-domain 8x8 font (third_party/font8x8); bit 0 is the left pixel. */
#include "../third_party/font8x8/font8x8_basic.h"

enum {
  kItemResume,
  kItemSlot,
  kItemSave,
  kItemLoad,
  kItemRewind,
  kItemMsu1,
  kItemAspect,
  kItemHd,
  kItemCheatLives,
  kItemCheatCodes,
  kItemPerf,
  kItemQuit,
  kItemCount,
};

static bool s_open;
static int s_cursor;

void Dkc1MenuDefaults(Dkc1MenuSettings *settings) {
  memset(settings, 0, sizeof *settings);
  settings->msu1 = true;
  settings->widescreen = true;
  settings->hd = true;
  settings->perf = false;
}

static bool *Toggle(Dkc1MenuSettings *settings, int item) {
  switch (item) {
    case kItemRewind: return &settings->rewind;
    case kItemMsu1: return &settings->msu1;
    case kItemAspect: return &settings->widescreen;
    case kItemHd: return &settings->hd;
    case kItemCheatLives: return &settings->cheat_lives;
    case kItemCheatCodes: return &settings->cheat_codes;
    case kItemPerf: return &settings->perf;
    default: return NULL;
  }
}

static const struct {
  const char *key;
  size_t offset;
} kKeys[] = {
  {"rewind", offsetof(Dkc1MenuSettings, rewind)},
  {"msu1", offsetof(Dkc1MenuSettings, msu1)},
  {"widescreen", offsetof(Dkc1MenuSettings, widescreen)},
  {"hd", offsetof(Dkc1MenuSettings, hd)},
  {"cheat_lives", offsetof(Dkc1MenuSettings, cheat_lives)},
  {"cheat_codes", offsetof(Dkc1MenuSettings, cheat_codes)},
  {"perf", offsetof(Dkc1MenuSettings, perf)},
};

void Dkc1MenuLoadSettings(Dkc1MenuSettings *settings, const char *path) {
  Dkc1MenuDefaults(settings);
  FILE *file = path ? fopen(path, "r") : NULL;
  if (!file)
    return;
  char line[128];
  while (fgets(line, sizeof line, file)) {
    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = 0;
    const int value = atoi(eq + 1);
    if (!strcmp(line, "slot") && value >= 0 && value < 5)
      settings->slot = value;
    for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
      if (!strcmp(line, kKeys[i].key))
        *(bool *)((char *)settings + kKeys[i].offset) = value != 0;
  }
  fclose(file);
}

bool Dkc1MenuSaveSettings(const Dkc1MenuSettings *settings, const char *path) {
  FILE *file = path ? fopen(path, "w") : NULL;
  if (!file)
    return false;
  fprintf(file, "slot=%d\n", settings->slot);
  for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
    fprintf(file, "%s=%d\n", kKeys[i].key,
            *(const bool *)((const char *)settings + kKeys[i].offset) ? 1 : 0);
  return fclose(file) == 0;
}

void Dkc1MenuOpen(void) {
  s_open = true;
  s_cursor = kItemResume;
}

void Dkc1MenuClose(void) {
  s_open = false;
}

bool Dkc1MenuIsOpen(void) {
  return s_open;
}

Dkc1MenuAction Dkc1MenuUpdate(Dkc1MenuSettings *settings, uint32_t pressed) {
  if (!s_open)
    return kDkc1MenuActionNone;
  if (pressed & kDkc1GamepadB) {
    s_open = false;
    return kDkc1MenuActionClose;
  }
  if (pressed & kDkc1GamepadDpadUp)
    s_cursor = (s_cursor + kItemCount - 1) % kItemCount;
  if (pressed & kDkc1GamepadDpadDown)
    s_cursor = (s_cursor + 1) % kItemCount;
  const int step = (pressed & kDkc1GamepadDpadRight) ? 1
                   : (pressed & kDkc1GamepadDpadLeft) ? -1 : 0;
  if (s_cursor == kItemSlot && step) {
    settings->slot = (settings->slot + 5 + step) % 5;
    return kDkc1MenuActionChanged;
  }
  bool *toggle = Toggle(settings, s_cursor);
  if (toggle && (step || (pressed & kDkc1GamepadA))) {
    /* HD only exists in 16:9. */
    if (s_cursor == kItemHd && (!settings->widescreen ||
                                !settings->hd_available))
      return kDkc1MenuActionNone;
    *toggle = !*toggle;
    /* 4:3 turns HD off; back in 16:9 a loaded pack comes back on. */
    if (s_cursor == kItemAspect)
      settings->hd = settings->widescreen && settings->hd_available;
    return kDkc1MenuActionChanged;
  }
  if (!(pressed & kDkc1GamepadA))
    return kDkc1MenuActionNone;
  switch (s_cursor) {
    case kItemResume:
      s_open = false;
      return kDkc1MenuActionClose;
    case kItemSave: return kDkc1MenuActionSave;
    case kItemLoad: return kDkc1MenuActionLoad;
    case kItemQuit: return kDkc1MenuActionQuit;
    default: return kDkc1MenuActionNone;
  }
}

static void Glyph(uint32_t *pixels, int width, int height, size_t pitch,
                  int x, int y, unsigned char c, uint32_t color) {
  if (c >= 128)
    c = '?';
  for (int row = 0; row < 8; row++) {
    const unsigned bits = (unsigned char)font8x8_basic[c][row];
    const int py = y + row;
    if (py < 0 || py >= height)
      continue;
    for (int col = 0; col < 8; col++) {
      const int px = x + col;
      if ((bits >> col) & 1 && px >= 0 && px < width)
        pixels[(size_t)py * pitch + px] = 0xff000000u | color;
    }
  }
}

void Dkc1MenuDrawText(uint32_t *pixels, int width, int height, size_t pitch,
                      int x, int y, const char *text, uint32_t color) {
  for (int i = 0; text[i]; i++) {
    Glyph(pixels, width, height, pitch, x + i * 8 + 1, y + 1,
          (unsigned char)text[i], 0x000000);
    Glyph(pixels, width, height, pitch, x + i * 8, y,
          (unsigned char)text[i], color);
  }
}

static void Shade(uint32_t *pixels, int width, int height, size_t pitch,
                  int x0, int y0, int x1, int y1) {
  for (int y = y0 < 0 ? 0 : y0; y < y1 && y < height; y++)
    for (int x = x0 < 0 ? 0 : x0; x < x1 && x < width; x++)
      pixels[(size_t)y * pitch + x] = 0xc0000000u;
}

void Dkc1MenuBlend(uint32_t *dst, size_t dst_pitch, const uint32_t *overlay,
                   size_t overlay_pitch, int width, int height) {
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++) {
      const uint32_t o = overlay[(size_t)y * overlay_pitch + x];
      const uint32_t a = o >> 24;
      if (!a)
        continue;
      uint32_t *d = &dst[(size_t)y * dst_pitch + x];
      uint32_t out = 0;
      for (int shift = 0; shift < 24; shift += 8) {
        const uint32_t dc = (*d >> shift) & 0xff, oc = (o >> shift) & 0xff;
        out |= ((oc * a + dc * (255 - a)) / 255) << shift;
      }
      *d = out;
    }
}

void Dkc1MenuDraw(const Dkc1MenuSettings *settings, uint32_t *pixels,
                  int width, int height, size_t pitch, const char *info,
                  const char *message) {
  if (!s_open)
    return;
  static const char *const kLabels[kItemCount] = {
    "Continuar",
    "Ranura",
    "Guardar estado",
    "Cargar estado",
    "Rebobinar L / Avanzar R",
    "Musica MSU-1 (reiniciar)",
    "Pantalla",
    "Texturas HD",
    "Truco: vidas infinitas",
    "Codigos (cheats.txt)",
    "Rendimiento en pantalla",
    "Salir del juego",
  };
  const int panel_w = 240, line_h = 12;
  const int panel_h = 34 + kItemCount * line_h + 22;
  const int x0 = (width - panel_w) / 2, y0 = (height - panel_h) / 2;
  Shade(pixels, width, height, pitch, x0, y0, x0 + panel_w, y0 + panel_h);
  Dkc1MenuDrawText(pixels, width, height, pitch, x0 + 8, y0 + 6,
                   "DKC1Recomp-NX", 0xffd040);
  Dkc1MenuDrawText(pixels, width, height, pitch, x0 + 8, y0 + 18,
                   "B / R3 / + y -: cerrar", 0x909090);
  for (int i = 0; i < kItemCount; i++) {
    const int y = y0 + 34 + i * line_h;
    const uint32_t color = i == s_cursor ? 0xffffff : 0xa0a0a0;
    if (i == s_cursor)
      Dkc1MenuDrawText(pixels, width, height, pitch, x0 + 4, y, ">", 0xffd040);
    Dkc1MenuDrawText(pixels, width, height, pitch, x0 + 14, y, kLabels[i],
                     color);
    char value[16] = "";
    const bool *toggle = Toggle((Dkc1MenuSettings *)settings, i);
    if (i == kItemAspect)
      snprintf(value, sizeof value, "< %s >",
               settings->widescreen ? "16:9" : "4:3");
    else if (i == kItemHd &&
             (!settings->widescreen || !settings->hd_available))
      snprintf(value, sizeof value, "--");
    else if (toggle)
      snprintf(value, sizeof value, "%s", *toggle ? "SI" : "NO");
    else if (i == kItemSlot)
      snprintf(value, sizeof value, "< %d >", settings->slot + 1);
    if (value[0])
      Dkc1MenuDrawText(pixels, width, height, pitch,
                       x0 + panel_w - 8 - (int)strlen(value) * 8, y, value,
                       color);
  }
  const int footer = y0 + 34 + kItemCount * line_h + 4;
  if (message && *message)
    Dkc1MenuDrawText(pixels, width, height, pitch, x0 + 8, footer, message,
                     0x80ff80);
  else if (info && *info)
    Dkc1MenuDrawText(pixels, width, height, pitch, x0 + 8, footer, info,
                     0x80c0ff);
}

int Dkc1MenuLoadCheats(const char *path, Dkc1Cheat *cheats, int capacity) {
  FILE *file = path ? fopen(path, "r") : NULL;
  if (!file)
    return 0;
  int count = 0;
  char line[160];
  while (count < capacity && fgets(line, sizeof line, file)) {
    char *comment = strchr(line, '#');
    if (comment)
      *comment = 0;
    char hex[9];
    int n = 0;
    for (char *c = line; *c && n < 9; c++)
      if (isxdigit((unsigned char)*c))
        hex[n++] = *c;
      else if (*c != '-' && *c != ':' && !isspace((unsigned char)*c))
        break;
    if (n != 8)
      continue;
    hex[8] = 0;
    const unsigned long code = strtoul(hex, NULL, 16);
    const uint32_t address = (uint32_t)(code >> 8);
    /* Only WRAM ($7E0000-$7FFFFF): cartridge ROM is never patched. */
    if (address < 0x7e0000u || address > 0x7fffffu)
      continue;
    cheats[count].address = address - 0x7e0000u;
    cheats[count].value = (uint8_t)code;
    count++;
  }
  fclose(file);
  return count;
}

void Dkc1MenuApplyCheats(const Dkc1MenuSettings *settings, uint8_t *wram,
                         const Dkc1Cheat *cheats, int count) {
  if (!wram)
    return;
  /* $0575: 16-bit lives counter (read-add-write site in bank B6). Keep it
   * from dropping below the starting five; never lower a higher count. */
  if (settings->cheat_lives && (wram[0x575] | wram[0x576] << 8) < 5) {
    wram[0x575] = 5;
    wram[0x576] = 0;
  }
  if (settings->cheat_codes)
    for (int i = 0; i < count; i++)
      wram[cheats[i].address] = cheats[i].value;
}
