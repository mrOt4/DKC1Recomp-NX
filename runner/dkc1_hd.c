#include "dkc1_hd.h"

#include "dkc1_video.h"
#include "snes/ppu.h"
#ifdef __SWITCH__
#include "switch_clock.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  kHdHeight = 224,
  kHdMaxScale = 4,
  kHdCharSlots = 0x8000 >> 3,  /* one per 8-word VRAM character row group */
  kHdCropRing = 16,
  kHdCropSize = 8 + 2 * kHdCropRing,  /* the tile plus a context ring */
  kHdPackEntrySize = 16,
  kHdPackHeaderSize = 64,
};

/* Same digest Dkc1ReadVerifiedRom accepts: DKC1 USA v1.0 headerless. */
static const uint8_t kRomSha256[32] = {
  0xfa, 0x8c, 0xac, 0xf5, 0xbb, 0xfc, 0x39, 0xee,
  0x6b, 0xba, 0xa5, 0x57, 0xad, 0xf8, 0x91, 0x33,
  0xd6, 0x0d, 0x42, 0xf6, 0xcf, 0x9e, 0x1d, 0xb3,
  0x0d, 0x5a, 0x36, 0xa4, 0x69, 0xf7, 0x4d, 0x15,
};

static Dkc1HdSource s_source;
static bool s_user_enabled = true;  /* Mods > HD Textures */
static int s_scale = kHdMaxScale;
static char s_status[192] = "HD textures off";

static PpuIdentityPixel s_gbuf[kHdHeight * kPpuBufWidth];
static PpuIdentityLine s_lines[kHdHeight];

static uint32_t *s_out;
static int s_out_width, s_out_height;
static size_t s_out_capacity;

static Dkc1HdStats s_stats;
static bool s_debug_misses;  /* DKC1_HD_DEBUG=misses: tint pack misses */
static bool s_deblock = true;  /* DKC1_HD_DEBLOCK=0 disables */
static int kHdSeamNativeStep = 96;
/* Track the layers under/over each pixel so HD silhouettes can differ from
 * the native ones. DKC1_HD_SHAPE=0 keeps native silhouettes (cheaper). */
static bool s_shape = true;

/* ---- HD pack (tiles.bin / tiles-<s>x.bin, written by tools/hd_pack.py) ---
 *   0  "DKC1HDP2"   8  u32 version (2)   12 u32 scale   16 u32 count
 *  20  u32 reserved 24 u8 rom_sha256[32] 56 u8 reserved[8]
 *  64  count x { u64 key, u32 data offset, u32 flags }, sorted by key
 *  ... tile data: (8*scale)^2 texels of 2 bytes each, in the stored
 *      character's orientation: byte 0 = i << 4 | j, two palette-relative
 *      indices (0 = transparent), byte 1 = w, the weight of j (0-255). A
 *      texel's color is i's blended toward j's by w/255, so HD edges and
 *      gradients fall between palette colors without leaving the palette
 *      (fades and palette animation still apply). As a little-endian u16:
 *      (i << 4 | j) | w << 8. */
typedef struct HdPack {
  uint8_t *blob;
  size_t size;
  uint32_t scale;      /* scale of `tiles` (possibly reduced at load) */
  uint32_t count;
  uint16_t *tiles;     /* count tiles of (8*scale)^2 texels, index order */
  uint64_t *keys;      /* count sorted character keys (PackFind) */
  uint64_t generation; /* changes whenever `tiles` changes (GPU upload) */
  char path[512];
} HdPack;

static HdPack s_pack;

typedef struct HdCharSlot {
  uint64_t stamp;   /* frame the fields below were computed for */
  uint64_t key;
  int32_t tile_index;   /* index in s_pack.tiles, -1 when the pack lacks it */
  uint64_t dump_stamp[2];  /* frame a main / sub occurrence was scored */
} HdCharSlot;

/* GPU-encode workers resolve slots concurrently. A slot's fields are
 * published by its stamp (release store after them, acquire load before
 * reading them); workers racing on one slot store identical values. MSVC
 * volatile has these semantics on x86/x64. */
#if defined(_MSC_VER) && !defined(__clang__)
#define HD_LOAD_ACQUIRE(p) (*(volatile const uint64_t *)(p))
#define HD_STORE_RELEASE(p, v) (*(volatile uint64_t *)(p) = (v))
#define HD_STORE_RELAXED(p, v) (*(p) = (v))
#else
#define HD_LOAD_ACQUIRE(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define HD_STORE_RELEASE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define HD_STORE_RELAXED(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#endif

static HdCharSlot s_chars[kHdCharSlots];

/* ---- Tile dump (DKC1_HD_DUMP) -------------------------------------------- */
typedef struct HdDumpRecord {
  uint64_t key;
  uint8_t depth, hflip, vflip, has_crop;
  float score;
  uint32_t count;
  uint16_t raw[16];
  uint8_t palette[16][3];
  uint8_t crop[kHdCropSize * kHdCropSize * 4];  /* RGBA */
} HdDumpRecord;

static char s_dump_dir[512];
static HdDumpRecord *s_dump;
static size_t s_dump_count, s_dump_capacity;
static int32_t *s_dump_table;  /* open addressing: record index + 1 */
static size_t s_dump_table_size;

static bool EnvironmentEnabled(const char *name) {
  const char *value = getenv(name);
  return value && *value && *value != '0';
}

static uint64_t CharKey(const uint16_t *vram, unsigned base, unsigned depth) {
  const unsigned words = depth == kPpuIdentDepth_2bpp ? 8 : 16;
  uint64_t hash = 0xcbf29ce484222325ull;
  for (unsigned i = 0; i < words; i++) {
    const uint16_t word = vram[(base + i) & 0x7fff];
    hash = (hash ^ (word & 0xff)) * 0x100000001b3ull;
    hash = (hash ^ (word >> 8)) * 0x100000001b3ull;
  }
  return (hash ^ depth) * 0x100000001b3ull;
}

static uint64_t ReadU64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
  return v;
}

static uint32_t ReadU32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static uint64_t s_pack_generation;

static void UnloadPack(void) {
  free(s_pack.blob);
  free(s_pack.tiles);
  free(s_pack.keys);
  memset(&s_pack, 0, sizeof s_pack);
  s_pack.generation = ++s_pack_generation;
}

/* Scale the HD tiles are composed at. DKC1_HD_SCALE overrides; Switch
 * defaults to 2x: a 4x pack is reduced at load, so one pack serves every
 * platform. A requested scale must divide the pack's scale. */
static uint32_t TargetScale(uint32_t pack_scale) {
  const char *text = getenv("DKC1_HD_SCALE");
  uint32_t want = text && *text ? (uint32_t)atoi(text) : 0;
#ifdef __SWITCH__
  if (!want) want = 2;
#endif
  if (!want || want > pack_scale || pack_scale % want)
    return pack_scale;
  return want;
}

static uint8_t *ReadWholeFile(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;
  uint8_t *blob = NULL;
  long length = -1;
  if (fseek(file, 0, SEEK_END) == 0)
    length = ftell(file);
  if (length > kHdPackHeaderSize && fseek(file, 0, SEEK_SET) == 0 &&
      (blob = malloc((size_t)length)) != NULL &&
      fread(blob, 1, (size_t)length, file) != (size_t)length) {
    free(blob);
    blob = NULL;
  }
  fclose(file);
  if (blob)
    *size = (size_t)length;
  return blob;
}

/* Loads tiles-<s>x.bin for the target scale when the pack has one (the
 * generator writes a properly resampled 2x file), else tiles.bin, reduced
 * by sampling if the target scale is smaller. */
static bool LoadPack(const char *dir, char *error, size_t error_size) {
  UnloadPack();
  char path[512];
  size_t size = 0;
  uint8_t *blob = NULL;
  const uint32_t want = TargetScale(kHdMaxScale);
  if (want != kHdMaxScale) {
    snprintf(path, sizeof path, "%s/tiles-%ux.bin", dir, (unsigned)want);
    blob = ReadWholeFile(path, &size);
  }
  if (!blob) {
    snprintf(path, sizeof path, "%s/tiles.bin", dir);
    blob = ReadWholeFile(path, &size);
  }
  if (!blob) {
    snprintf(error, error_size, "cannot read %s", path);
    return false;
  }
  const uint32_t scale = ReadU32(blob + 12), count = ReadU32(blob + 16);
  const size_t tile_bytes = (size_t)(8 * scale) * (8 * scale) * 2;
  const size_t index_end = kHdPackHeaderSize + (size_t)count * kHdPackEntrySize;
  const char *problem = NULL;
  if (!memcmp(blob, "DKC1HDP1", 8))
    problem = "old (v1) pack format: regenerate it with tools/hd_pack.py";
  else if (memcmp(blob, "DKC1HDP2", 8) || ReadU32(blob + 8) != 2)
    problem = "not a DKC1 HD pack (v2)";
  else if (memcmp(blob + 24, kRomSha256, sizeof kRomSha256))
    problem = "pack was built for a different ROM";
  else if (scale != 1 && scale != 2 && scale != 4)
    problem = "unsupported pack scale";
  else if (count > kDkc1HdGpuNoTile)
    problem = "too many tiles";
  else if (index_end > size)
    problem = "truncated tile index";
  for (uint32_t i = 0; !problem && i < count; i++) {
    const uint8_t *entry = blob + kHdPackHeaderSize + (size_t)i * kHdPackEntrySize;
    if ((size_t)ReadU32(entry + 8) + tile_bytes > size)
      problem = "tile data out of range";
    else if (i && ReadU64(entry) <= ReadU64(entry - kHdPackEntrySize))
      problem = "tile index not sorted";
  }
  if (problem) {
    free(blob);
    snprintf(error, error_size, "%s: %s", path, problem);
    return false;
  }
  /* Copy the tiles into index order, reducing the scale if needed by
   * taking each reduced texel's top-left source texel. */
  const uint32_t target = TargetScale(scale);
  const uint32_t factor = scale / target;
  const size_t out_span = 8 * target, in_span = 8 * scale;
  uint16_t *tiles = malloc((size_t)count * out_span * out_span * 2 + 2);
  uint64_t *keys = malloc((size_t)count * sizeof *keys + 8);
  if (!tiles || !keys) {
    free(tiles);
    free(keys);
    free(blob);
    snprintf(error, error_size, "%s: out of memory", path);
    return false;
  }
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *entry = blob + kHdPackHeaderSize + (size_t)i * kHdPackEntrySize;
    const uint8_t *src = blob + ReadU32(entry + 8);
    uint16_t *dst = tiles + (size_t)i * out_span * out_span;
    keys[i] = ReadU64(entry);
    for (size_t y = 0; y < out_span; y++)
      for (size_t x = 0; x < out_span; x++) {
        const uint8_t *t = src + ((y * factor) * in_span + x * factor) * 2;
        dst[y * out_span + x] = (uint16_t)(t[0] | t[1] << 8);
      }
  }
  /* Tiles and keys are copied out; the file image is not needed. */
  free(blob);
  s_pack.blob = NULL;
  s_pack.size = size;
  s_pack.scale = target;
  s_pack.count = count;
  s_pack.tiles = tiles;
  s_pack.keys = keys;
  s_pack.generation = ++s_pack_generation;
  snprintf(s_pack.path, sizeof s_pack.path, "%s", dir);
  return true;
}

/* Index of a character's HD tile in s_pack.tiles, or -1. */
static int32_t PackFind(uint64_t key) {
  size_t lo = 0, hi = s_pack.count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const uint64_t k = s_pack.keys[mid];
    if (k == key)
      return (int32_t)mid;
    if (k < key) lo = mid + 1; else hi = mid;
  }
  return -1;
}

void Dkc1HdSetSource(Dkc1HdSource source, int scale) {
  if (source == kDkc1HdSourcePack && !s_pack.tiles)
    source = kDkc1HdSourceNone;
  if (source == kDkc1HdSourcePack)
    scale = (int)s_pack.scale;
  if (scale < 1) scale = 1;
  if (scale > kHdMaxScale) scale = kHdMaxScale;
  s_source = source;
  s_scale = scale;
  memset(&s_stats, 0, sizeof s_stats);
  memset(s_chars, 0, sizeof s_chars);
  static const char *const kNames[] = {"off", "identity", "grid", "pack"};
  if (source == kDkc1HdSourceNone)
    snprintf(s_status, sizeof s_status, "HD textures off");
  else if (source == kDkc1HdSourcePack)
    snprintf(s_status, sizeof s_status, "HD textures %dx | %u tiles",
             scale, s_pack.count);
  else
    snprintf(s_status, sizeof s_status, "HD textures %s %dx",
             kNames[source], scale);
}

bool Dkc1HdLoadPack(const char *dir, char *error, size_t error_size) {
  if (!LoadPack(dir, error, error_size)) {
    Dkc1HdSetSource(kDkc1HdSourceNone, s_scale);
    return false;
  }
  Dkc1HdSetSource(kDkc1HdSourcePack, (int)s_pack.scale);
  return true;
}

static void WriteDump(void);

void Dkc1HdInitializeFromEnvironment(void) {
  Dkc1HdSource source = kDkc1HdSourceNone;
  const char *scale_text = getenv("DKC1_HD_SCALE");
  int scale = scale_text && *scale_text ? atoi(scale_text) : kHdMaxScale;
  const char *pack = getenv("DKC1_HD_PACK");
#ifdef __SWITCH__
  /* No menu on Switch: a pack in the app directory enables the mod. The
   * 2x file alone is enough (it is what the Switch loads). */
  if (!pack || !*pack) {
    static const char *const kProbes[] = {"hd/tiles-2x.bin", "hd/tiles.bin"};
    for (size_t i = 0; i < sizeof kProbes / sizeof kProbes[0] && !pack; i++) {
      FILE *probe = fopen(kProbes[i], "rb");
      if (probe) {
        fclose(probe);
        pack = "hd";
      }
    }
  }
#endif
  const char *debug = getenv("DKC1_HD_DEBUG");
  s_debug_misses = debug && strcmp(debug, "misses") == 0;
  const char *deblock = getenv("DKC1_HD_DEBLOCK");
  s_deblock = !deblock || *deblock != '0';
  if (getenv("DKC1_HD_SEAM_STEP"))
    kHdSeamNativeStep = atoi(getenv("DKC1_HD_SEAM_STEP"));
  const char *shape = getenv("DKC1_HD_SHAPE");
  s_shape = !shape || *shape != '0';
  if (debug && strcmp(debug, "grid") == 0) {
    source = kDkc1HdSourceGrid;
  } else if (EnvironmentEnabled("DKC1_HD_IDENTITY")) {
    source = kDkc1HdSourceIdentity;
  } else if (pack && *pack) {
    char error[600];
    if (LoadPack(pack, error, sizeof error))
      source = kDkc1HdSourcePack;
    else
      fprintf(stderr, "[hd] %s\n", error);
  }
  const char *dump = getenv("DKC1_HD_DUMP");
  if (dump && *dump) {
    snprintf(s_dump_dir, sizeof s_dump_dir, "%s", dump);
    if (source == kDkc1HdSourceNone)
      source = kDkc1HdSourceIdentity;  /* the dump reads the capture */
    atexit(WriteDump);
  }
  if (EnvironmentEnabled("DKC1_HD_DISABLE"))
    source = kDkc1HdSourceNone;
  Dkc1HdSetSource(source, scale);
}

/* HD is a widescreen remaster: the native 4:3 presentation stays stock. */
bool Dkc1HdEnabled(void) {
  return s_user_enabled && s_source != kDkc1HdSourceNone &&
         Dkc1VideoGetAspect() != kDkc1VideoAspectNative;
}

bool Dkc1HdReady(void) {
  return s_source != kDkc1HdSourceNone;
}

void Dkc1HdSetEnabled(bool enabled) {
  s_user_enabled = enabled;
}

bool Dkc1HdUserEnabled(void) {
  return s_user_enabled && Dkc1HdReady();
}

const char *Dkc1HdStatus(void) {
  static char status[256];
  if (Dkc1HdReady() && !s_user_enabled)
    return "HD textures disabled";
  if (Dkc1HdReady() && Dkc1VideoGetAspect() == kDkc1VideoAspectNative) {
    snprintf(status, sizeof status, "%s (needs 16:9 or 16:10)", s_status);
    return status;
  }
  return s_status;
}

void Dkc1HdPrepareFrame(Ppu *ppu) {
  if (!Dkc1HdEnabled()) {
    PpuSetIdentityCapture(ppu, NULL, 0, NULL);
    return;
  }
  /* The PPU rewrites every identity row it draws (including the columns it
   * does not compose); only the per-line records need a fresh start. */
  memset(s_lines, 0, sizeof s_lines);
  PpuSetIdentityCapture(ppu, s_gbuf, kPpuBufWidth, s_lines);
  PpuSetIdentityShape(ppu, s_shape);
}

typedef struct HdLineMaps {
  uint8_t mult[63];
  uint8_t half[64];
} HdLineMaps;

/* Mirrors ppu_runLine's brightness tables. */
/* Walks one HD tile's texels for a native pixel: texel (u, v) of the block
 * is p[v * dv + u * du], with the tile's flips folded into the steps. */
typedef struct HdTexelWalk {
  const uint16_t *p;
  int du, dv;
} HdTexelWalk;

static bool TexelWalk(const uint16_t *tile, uint32_t ref, HdTexelWalk *walk) {
  if (!tile)
    return false;
  const int s = s_scale, span = 8 * s;
  const int col = (ref >> kPpuIdentRef_ColShift) & 7, row = ref & 7;
  const bool hflip = (ref & kPpuIdentRef_HFlip) != 0;
  const bool vflip = (ref & kPpuIdentRef_VFlip) != 0;
  walk->du = hflip ? -1 : 1;
  walk->dv = vflip ? -span : span;
  walk->p = tile + (row * s + (vflip ? s - 1 : 0)) * span + col * s +
            (hflip ? s - 1 : 0);
  return true;
}

static void BuildLineMaps(uint8_t brightness, HdLineMaps *maps) {
  for (int i = 0; i < 32; i++)
    maps->mult[i] = (uint8_t)(((i << 3) | (i >> 2)) * brightness / 15);
  memset(&maps->mult[32], maps->mult[31], 31);
  for (int i = 0; i < 32; i++)
    maps->half[i * 2] = maps->half[i * 2 + 1] = maps->mult[i];
}

/* Mirrors PpuDrawWholeLine's composition for one pixel. */
static uint32_t ComposeColor(const PpuIdentityLine *line,
                             const HdLineMaps *maps, uint8_t main_index,
                             uint8_t sub_index, uint8_t flags) {
  if (flags & kPpuIdentFlag_Black)
    return 0;
  const uint32_t mask = (flags & kPpuIdentFlag_Clip) ? 0 : 0x1f;
  const uint32_t color = line->cgram[main_index];
  uint32_t r = color & mask, g = (color >> 5) & mask, b = (color >> 10) & mask;
  const uint8_t *map = maps->mult;
  if (flags & kPpuIdentFlag_Math) {
    const uint32_t color2 = (flags & kPpuIdentFlag_SubFixed)
                                ? line->fixed_color
                                : line->cgram[sub_index];
    if (flags & kPpuIdentFlag_Half)
      map = maps->half;
    const uint32_t r2 = color2 & 0x1f, g2 = (color2 >> 5) & 0x1f,
                   b2 = (color2 >> 10) & 0x1f;
    if (flags & kPpuIdentFlag_Subtract) {
      r = r >= r2 ? r - r2 : 0;
      g = g >= g2 ? g - g2 : 0;
      b = b >= b2 ? b - b2 : 0;
    } else {
      r += r2, g += g2, b += b2;
    }
  }
  return (uint32_t)map[b] | (uint32_t)map[g] << 8 | (uint32_t)map[r] << 16;
}

/* Native texel value (palette-relative, 0 = transparent) of a character. */
static unsigned DecodeCharTexel(const uint16_t *words, unsigned depth,
                                unsigned col, unsigned row) {
  const unsigned bit = 7 - col;
  const uint16_t p01 = words[row];
  unsigned value = ((p01 >> bit) & 1) | (((p01 >> (bit + 8)) & 1) << 1);
  if (depth >= kPpuIdentDepth_4bpp) {
    const uint16_t p23 = words[row + 8];
    value |= (((p23 >> bit) & 1) << 2) | (((p23 >> (bit + 8)) & 1) << 3);
  }
  return value;
}

/* Native texel value at a capture ref, read from live VRAM. */
static unsigned DecodeTexel(const uint16_t *vram, uint32_t ref) {
  const unsigned addr = ref & 0x7fff;
  uint16_t words[16];
  const unsigned base = addr & ~7u;
  for (unsigned i = 0; i < 16; i++) words[i] = vram[(base + i) & 0x7fff];
  return DecodeCharTexel(words, (ref >> kPpuIdentRef_DepthShift) & 3,
                         (ref >> kPpuIdentRef_ColShift) & 7, addr & 7);
}

static unsigned RefColors(uint32_t ref) {
  return ((ref >> kPpuIdentRef_DepthShift) & 3) == kPpuIdentDepth_2bpp ? 4
                                                                        : 16;
}

/* Identity/grid sources reproduce the native texel, which also validates
 * every capture ref against VRAM. Unknown or transparent texels keep the
 * native index. */
static uint8_t IdentityIndex(const uint16_t *vram, uint32_t ref,
                             uint8_t native_index, bool *mismatch) {
  if (!(ref & kPpuIdentRef_Valid) || !native_index)
    return native_index;
  const unsigned value = DecodeTexel(vram, ref);
  const uint8_t index =
      (uint8_t)((native_index & ~(RefColors(ref) - 1)) | value);
  if (!value || index != native_index) {
    *mismatch = true;
    return native_index;
  }
  return index;
}

/* Pack tile for a capture ref, hashed once per character per frame. */
/* Pack tile index of the character a ref points at, -1 when absent. */
static int32_t PackTileIndex(const uint16_t *vram, uint32_t ref) {
  if (!(ref & kPpuIdentRef_Valid))
    return -1;
  const unsigned base = (ref & 0x7fff) & ~7u;
  HdCharSlot *slot = &s_chars[base >> 3];
  if (HD_LOAD_ACQUIRE(&slot->stamp) == s_stats.frames)
    return slot->tile_index;
  const uint64_t key =
      CharKey(vram, base, (ref >> kPpuIdentRef_DepthShift) & 3);
  const int32_t index = PackFind(key);
  HD_STORE_RELAXED(&slot->key, key);
  HD_STORE_RELAXED(&slot->tile_index, index);
  HD_STORE_RELEASE(&slot->stamp, s_stats.frames);
  return index;
}

static const uint16_t *PackTile(const uint16_t *vram, uint32_t ref) {
  const int32_t index = PackTileIndex(vram, ref);
  return index < 0 ? NULL
                   : s_pack.tiles + (size_t)index * (8 * s_pack.scale) *
                                        (8 * s_pack.scale);
}

static FORCEINLINE uint8_t PaletteBase(uint8_t index, uint32_t ref) {
  return (uint8_t)(index & ~(RefColors(ref) - 1));
}

/* ---- v2 texel colors ------------------------------------------------------
 * Channels are kept as floats in 5-bit units through the blend and the
 * color math, then expanded and dimmed as the PPU does. The GPU compose
 * pass (dkc1_hd_gpu.c) runs the same operations in the same order. With
 * whole weights (0 or 255) every step equals the PPU's integer one. */
typedef struct HdRgb {
  float r, g, b;
} HdRgb;

static FORCEINLINE HdRgb Rgb15(uint16_t c) {
  return (HdRgb){(float)(c & 31), (float)((c >> 5) & 31),
                 (float)((c >> 10) & 31)};
}

/* An HD texel's color: index i's blended toward j's by w/255, where a
 * transparent index (0) stands for `under`, whatever lies below. */
static FORCEINLINE HdRgb BlendTexel(const PpuIdentityLine *line,
                                    uint16_t texel, unsigned base,
                                    HdRgb under) {
  const unsigned i = (texel >> 4) & 15, j = texel & 15;
  const float w = (float)(texel >> 8) / 255.0f;
  const HdRgb a = i ? Rgb15(line->cgram[(base | i) & 255]) : under;
  const HdRgb b = j ? Rgb15(line->cgram[(base | j) & 255]) : under;
  return (HdRgb){a.r + (b.r - a.r) * w, a.g + (b.g - a.g) * w,
                 a.b + (b.b - a.b) * w};
}

/* (c << 3) | (c >> 2) for 5-bit c, interpolated between whole values. */
static FORCEINLINE float Expand5(float c) {
  const int lo = (int)c;
  const int hi = lo < 31 ? lo + 1 : 31;
  const float e0 = (float)((lo << 3) | (lo >> 2));
  const float e1 = (float)((hi << 3) | (hi >> 2));
  return e0 + (e1 - e0) * (c - (float)lo);
}

/* The PPU halves whole values with a shift (floor). A blend that lands on
 * a whole value only within float noise, which differs between the CPU and
 * a GPU that fuses multiply-adds, must take the same branch on both. */
static FORCEINLINE float HalfChannel(float c) {
  const float whole = floorf(c + 0.5f);
  return fabsf(c - whole) < 1e-3f ? floorf(whole * 0.5f) : c * 0.5f;
}

/* Color math, clamping and brightness: PpuDrawWholeLine for one subpixel. */
static uint32_t FinishColor(const PpuIdentityLine *line, uint8_t flags,
                            HdRgb m, HdRgb sub) {
  if (flags & kPpuIdentFlag_Clip)
    m = (HdRgb){0, 0, 0};
  if (flags & kPpuIdentFlag_Math) {
    if (flags & kPpuIdentFlag_Subtract) {
      m.r = fmaxf(m.r - sub.r, 0.0f);
      m.g = fmaxf(m.g - sub.g, 0.0f);
      m.b = fmaxf(m.b - sub.b, 0.0f);
    } else {
      m.r += sub.r;
      m.g += sub.g;
      m.b += sub.b;
    }
    if (flags & kPpuIdentFlag_Half) {
      m.r = HalfChannel(m.r);
      m.g = HalfChannel(m.g);
      m.b = HalfChannel(m.b);
    }
  }
  const float bright = (float)line->brightness;
  const float c[3] = {fminf(m.r, 31.0f), fminf(m.g, 31.0f), fminf(m.b, 31.0f)};
  uint32_t out = 0;
  for (int k = 0; k < 3; k++)
    out |= (uint32_t)floorf(Expand5(c[k]) * bright / 15.0f) << (16 - 8 * k);
  return out;
}

static bool EnsureOutput(int width) {
  const size_t need = (size_t)width * s_scale * kHdHeight * s_scale;
  if (need > s_out_capacity) {
    uint32_t *grown = realloc(s_out, need * sizeof *s_out);
    if (!grown)
      return false;
    s_out = grown;
    s_out_capacity = need;
  }
  s_out_width = width * s_scale;
  s_out_height = kHdHeight * s_scale;
  return true;
}

static FORCEINLINE uint32_t *BlockRow(int x, int y, int v) {
  return s_out + (size_t)(y * s_scale + v) * s_out_width + (size_t)x * s_scale;
}

static void FillBlock(int x, int y, uint32_t color) {
  for (int v = 0; v < s_scale; v++) {
    uint32_t *row = BlockRow(x, y, v);
    for (int u = 0; u < s_scale; u++)
      row[u] = color;
  }
}

/* Darken the HD subpixels on each tile's top and left edge (in the stored
 * character's orientation), leaving the rest of the block untouched. */
static void DrawGridEdges(int x, int y, uint32_t ref) {
  if (!(ref & kPpuIdentRef_Valid))
    return;
  const unsigned col = (ref >> kPpuIdentRef_ColShift) & 7;
  const unsigned row = ref & 7;
  if (col && row)
    return;
  const bool hflip = (ref & kPpuIdentRef_HFlip) != 0;
  const bool vflip = (ref & kPpuIdentRef_VFlip) != 0;
  for (int v = 0; v < s_scale; v++) {
    uint32_t *out = BlockRow(x, y, v);
    const int cv = vflip ? s_scale - 1 - v : v;
    for (int u = 0; u < s_scale; u++) {
      const int cu = hflip ? s_scale - 1 - u : u;
      if ((col == 0 && cu == 0) || (row == 0 && cv == 0))
        out[u] = (out[u] >> 1) & 0x7f7f7f;
    }
  }
}

/* ---- Dump ---------------------------------------------------------------- */

static HdDumpRecord *DumpRecord(uint64_t key) {
  if (s_dump_count * 2 >= s_dump_table_size) {
    const size_t size = s_dump_table_size ? s_dump_table_size * 2 : 1 << 15;
    int32_t *table = calloc(size, sizeof *table);
    if (!table)
      return NULL;
    for (size_t i = 0; i < s_dump_count; i++) {
      size_t h = (size_t)(s_dump[i].key % size);
      while (table[h]) h = (h + 1) % size;
      table[h] = (int32_t)i + 1;
    }
    free(s_dump_table);
    s_dump_table = table;
    s_dump_table_size = size;
  }
  size_t h = (size_t)(key % s_dump_table_size);
  while (s_dump_table[h]) {
    HdDumpRecord *record = &s_dump[s_dump_table[h] - 1];
    if (record->key == key)
      return record;
    h = (h + 1) % s_dump_table_size;
  }
  if (s_dump_count == s_dump_capacity) {
    const size_t capacity = s_dump_capacity ? s_dump_capacity * 2 : 4096;
    HdDumpRecord *grown = realloc(s_dump, capacity * sizeof *grown);
    if (!grown)
      return NULL;
    s_dump = grown;
    s_dump_capacity = capacity;
  }
  HdDumpRecord *record = &s_dump[s_dump_count];
  memset(record, 0, sizeof *record);
  record->key = key;
  record->score = -1.0f;
  s_dump_table[h] = (int32_t)++s_dump_count;
  return record;
}

/* ---- Layer-isolated dump context ------------------------------------------
 * The upscaler must see a tile with the neighbours it really has on its own
 * layer, not whatever other layers or sprites covered it on screen. BG
 * layers are rendered whole from their VRAM tilemap; OBJ from the full OAM.
 * Built lazily, at most once per frame and layer. */
enum {
  kHdCanvasW = 512,
  kHdBgCanvasH = 512,
  kHdObjCanvasH = 256,
};

static uint8_t s_bg_canvas[3][kHdCanvasW * kHdBgCanvasH * 4];
static int s_bg_w[3], s_bg_h[3];
static uint64_t s_bg_stamp[3], s_obj_stamp;
static uint8_t s_obj_canvas[kHdCanvasW * kHdObjCanvasH * 4];

static void PutColor(uint8_t *out, uint16_t color) {
  const unsigned rgb[3] = {color & 31u, (color >> 5) & 31u,
                           (color >> 10) & 31u};
  for (int k = 0; k < 3; k++)
    out[k] = (uint8_t)((rgb[k] << 3) | (rgb[k] >> 2));
  out[3] = 255;
}

static unsigned CharTexelAt(const uint16_t *vram, unsigned base,
                            unsigned depth, unsigned col, unsigned row) {
  uint16_t words[16];
  for (unsigned i = 0; i < 16; i++) words[i] = vram[(base + i) & 0x7fff];
  return DecodeCharTexel(words, depth, col, row);
}

static unsigned BgEntryAddress(const Ppu *ppu, int layer, int tx, int ty) {
  unsigned adr = PPU_bgTilemapAdr(ppu, layer) + (unsigned)(ty & 31) * 32 +
                 (unsigned)(tx & 31);
  if ((tx & 32) && PPU_bgTilemapWider(ppu, layer)) adr += 0x400;
  if ((ty & 32) && PPU_bgTilemapHigher(ppu, layer))
    adr += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  return adr & 0x7fff;
}

static unsigned BgCharBase(const Ppu *ppu, int layer, uint16_t entry) {
  const unsigned words = layer < 2 ? 16 : 8;
  return (PPU_bgTileAdr(ppu, layer) + (entry & 0x3ffu) * words) & 0x7fff;
}

static void BuildBgCanvas(const Ppu *ppu, int layer) {
  if (s_bg_stamp[layer] == s_stats.frames)
    return;
  s_bg_stamp[layer] = s_stats.frames;
  const int tiles_w = PPU_bgTilemapWider(ppu, layer) ? 64 : 32;
  const int tiles_h = PPU_bgTilemapHigher(ppu, layer) ? 64 : 32;
  const unsigned depth = layer < 2 ? kPpuIdentDepth_4bpp : kPpuIdentDepth_2bpp;
  const unsigned colors = layer < 2 ? 16 : 4;
  s_bg_w[layer] = tiles_w * 8;
  s_bg_h[layer] = tiles_h * 8;
  uint8_t *canvas = s_bg_canvas[layer];
  for (int ty = 0; ty < tiles_h; ty++) {
    for (int tx = 0; tx < tiles_w; tx++) {
      const uint16_t entry = ppu->vram[BgEntryAddress(ppu, layer, tx, ty)];
      const unsigned base = BgCharBase(ppu, layer, entry);
      const unsigned palette = ((entry >> 10) & 7) * colors;
      for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {
          const unsigned u = (entry & 0x4000) ? 7 - px : px;
          const unsigned v = (entry & 0x8000) ? 7 - py : py;
          const unsigned value = CharTexelAt(ppu->vram, base, depth, u, v);
          uint8_t *out = &canvas[((size_t)(ty * 8 + py) * kHdCanvasW +
                                  tx * 8 + px) * 4];
          if (value) PutColor(out, ppu->cgram[palette + value]);
          else memset(out, 0, 4);
        }
      }
    }
  }
}

typedef struct HdSprite {
  int x, y, size;
  uint16_t attr;
  unsigned obj_base;
} HdSprite;

static HdSprite DecodeSprite(const Ppu *ppu, int slot) {
  static const uint8_t kSizes[8][2] = {
    {8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32},
    {16, 32}};
  const int index = slot * 2;
  HdSprite sprite;
  sprite.x = (ppu->oam[index] & 0xff) |
             ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
  sprite.y = ppu->oam[index] >> 8;
  sprite.size = kSizes[PPU_objSize(ppu)]
                      [(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
  sprite.attr = ppu->oam[index + 1];
  sprite.obj_base = (sprite.attr & 0x100) ? PPU_objTileAdr2(ppu)
                                          : PPU_objTileAdr1(ppu);
  return sprite;
}

static unsigned SpriteCharBase(const HdSprite *sprite, int col, int row) {
  const unsigned tile = ((((sprite->attr & 0xff) >> 4) + (unsigned)row) << 4) |
                        (((sprite->attr & 0xf) + (unsigned)col) & 0xf);
  return (sprite->obj_base + tile * 16) & 0x7fff;
}

static void BuildObjCanvas(const Ppu *ppu) {
  if (s_obj_stamp == s_stats.frames)
    return;
  s_obj_stamp = s_stats.frames;
  memset(s_obj_canvas, 0, sizeof s_obj_canvas);
  /* Lower OAM slots win, so draw them last. */
  for (int slot = 127; slot >= 0; slot--) {
    const HdSprite sprite = DecodeSprite(ppu, slot);
    const unsigned palette = 0x80 + 16 * ((sprite.attr >> 9) & 7);
    for (int ly = 0; ly < sprite.size; ly++) {
      for (int lx = 0; lx < sprite.size; lx++) {
        const int ux = (sprite.attr & 0x4000) ? sprite.size - 1 - lx : lx;
        const int uy = (sprite.attr & 0x8000) ? sprite.size - 1 - ly : ly;
        const unsigned value = CharTexelAt(
            ppu->vram, SpriteCharBase(&sprite, ux >> 3, uy >> 3),
            kPpuIdentDepth_4bpp, (unsigned)(ux & 7), (unsigned)(uy & 7));
        if (!value)
          continue;
        PutColor(&s_obj_canvas[((size_t)((sprite.y + ly) & 255) * kHdCanvasW +
                                ((sprite.x + lx) & 511)) * 4],
                 ppu->cgram[palette + value]);
      }
    }
  }
}

static int TorusDistance(int a, int b, int period) {
  int d = abs(a - b) % period;
  return d < period - d ? d : period - d;
}

static void CropCanvas(const uint8_t *canvas, int w, int h, int left, int top,
                       uint8_t *crop) {
  for (int cy = 0; cy < kHdCropSize; cy++) {
    const int sy = ((top - kHdCropRing + cy) % h + h) % h;
    for (int cx = 0; cx < kHdCropSize; cx++) {
      const int sx = ((left - kHdCropRing + cx) % w + w) % w;
      memcpy(&crop[(cy * kHdCropSize + cx) * 4],
             &canvas[((size_t)sy * kHdCanvasW + sx) * 4], 4);
    }
  }
}

/* Crop the character's own layer around the cell nearest its on-screen
 * position. x0/y0 are SNES screen coordinates of the tile's top-left. */
static bool LayerContext(const Ppu *ppu, uint8_t layer_type, unsigned base,
                         unsigned depth, bool hflip, bool vflip, int x0,
                         int y0, uint8_t *crop) {
  const uint16_t flips = (hflip ? 0x4000 : 0) | (vflip ? 0x8000 : 0);
  if (layer_type <= 2) {
    const int layer = layer_type;
    if (PPU_mode(ppu) != 1 || PPU_bigTiles(ppu, layer) ||
        (layer < 2) != (depth == kPpuIdentDepth_4bpp))
      return false;
    BuildBgCanvas(ppu, layer);
    const int w = s_bg_w[layer], h = s_bg_h[layer];
    const int ex = x0 + ppu->hScroll[layer], ey = y0 + ppu->vScroll[layer];
    int best = -1, best_x = 0, best_y = 0;
    for (int ty = 0; ty < h / 8; ty++) {
      for (int tx = 0; tx < w / 8; tx++) {
        const uint16_t entry = ppu->vram[BgEntryAddress(ppu, layer, tx, ty)];
        if ((entry & 0xc000) != flips || BgCharBase(ppu, layer, entry) != base)
          continue;
        const int d = TorusDistance(tx * 8, ex, w) + TorusDistance(ty * 8, ey, h);
        if (best < 0 || d < best) best = d, best_x = tx * 8, best_y = ty * 8;
      }
    }
    if (best < 0)
      return false;
    CropCanvas(s_bg_canvas[layer], w, h, best_x, best_y, crop);
    return true;
  }
  if ((layer_type != 4 && layer_type != 6) || depth != kPpuIdentDepth_4bpp)
    return false;
  BuildObjCanvas(ppu);
  int best = -1, best_x = 0, best_y = 0;
  for (int slot = 0; slot < 128; slot++) {
    const HdSprite sprite = DecodeSprite(ppu, slot);
    if ((sprite.attr & 0xc000) != flips)
      continue;
    for (int row = 0; row < sprite.size / 8; row++) {
      for (int col = 0; col < sprite.size / 8; col++) {
        if (SpriteCharBase(&sprite, col, row) != base)
          continue;
        const int lx = hflip ? sprite.size - 8 - col * 8 : col * 8;
        const int ly = vflip ? sprite.size - 8 - row * 8 : row * 8;
        const int cx = (sprite.x + lx) & 511, cy = (sprite.y + ly) & 255;
        const int d = TorusDistance(cx, x0 & 511, kHdCanvasW) +
                      TorusDistance(cy, y0 & 255, kHdObjCanvasH);
        if (best < 0 || d < best) best = d, best_x = cx, best_y = cy;
      }
    }
  }
  if (best < 0)
    return false;
  CropCanvas(s_obj_canvas, kHdCanvasW, kHdObjCanvasH, best_x, best_y, crop);
  return true;
}

/* Fallback context: the composed frame around the tile, fully opaque. */
static void FrameContext(const Ppu *ppu, int width, int x0, int y0,
                         uint8_t *crop) {
  for (int cy = 0; cy < kHdCropSize; cy++) {
    int sy = y0 - kHdCropRing + cy;
    sy = sy < 0 ? 0 : sy >= kHdHeight ? kHdHeight - 1 : sy;
    const uint32_t *native =
        (const uint32_t *)(ppu->renderBuffer + (size_t)sy * ppu->renderPitch);
    for (int cx = 0; cx < kHdCropSize; cx++) {
      int sx = x0 - kHdCropRing + cx;
      sx = sx < 0 ? 0 : sx >= width ? width - 1 : sx;
      uint8_t *out = &crop[(cy * kHdCropSize + cx) * 4];
      out[0] = (uint8_t)(native[sx] >> 16);
      out[1] = (uint8_t)(native[sx] >> 8);
      out[2] = (uint8_t)native[sx];
      out[3] = 255;
    }
  }
}

/* Score one on-screen occurrence of a main-screen character and keep the
 * best one: fully visible, unblended, full brightness, with in-frame
 * context. The crop comes from the character's own layer (LayerContext). */
static uint64_t s_dump_8bpp_sightings;

/* `sub`: score the sub-screen pixel instead of the main one. Some scenes
 * draw a layer on the main screen with a black palette and show it through
 * color math from the sub screen (Cranky's intro stage): only the sub-screen
 * sighting carries the character's real colors. */
static void DumpOccurrence(const Ppu *ppu, int width, int x, int y,
                           const PpuIdentityPixel *gp, bool sub) {
  const uint32_t ref = sub ? gp->sub_ref : gp->main_ref;
  const uint8_t pixel_index = sub ? gp->sub_index : gp->main_index;
  const uint8_t pixel_layer = sub ? gp->sub_layer : gp->main_layer;
  const unsigned base = (ref & 0x7fff) & ~7u;
  HdCharSlot *slot = &s_chars[base >> 3];
  if (slot->dump_stamp[sub] == s_stats.frames)
    return;
  slot->dump_stamp[sub] = s_stats.frames;
  const unsigned depth = (ref >> kPpuIdentRef_DepthShift) & 3;
  if (depth == kPpuIdentDepth_8bpp) {
    /* Records hold 16 words: an 8bpp character (32) cannot be dumped yet.
     * Counted so a dump shows whether the game uses any. */
    s_dump_8bpp_sightings++;
    return;
  }
  uint16_t words[16] = {0};
  const unsigned nwords = depth == kPpuIdentDepth_2bpp ? 8 : 16;
  for (unsigned i = 0; i < nwords; i++)
    words[i] = ppu->vram[(base + i) & 0x7fff];
  HdDumpRecord *record = DumpRecord(CharKey(ppu->vram, base, depth));
  if (!record)
    return;
  record->count++;
  if (s_lines[y].brightness != 15)
    return;
  const bool hflip = (ref & kPpuIdentRef_HFlip) != 0;
  const bool vflip = (ref & kPpuIdentRef_VFlip) != 0;
  const int col = (ref >> kPpuIdentRef_ColShift) & 7, row = ref & 7;
  const int x0 = x - (hflip ? 7 - col : col);
  const int y0 = y - (vflip ? 7 - row : row);
  int opaque = 0, visible = 0;
  for (int tr = 0; tr < 8; tr++) {
    for (int tc = 0; tc < 8; tc++) {
      if (!DecodeCharTexel(words, depth, (unsigned)tc, (unsigned)tr))
        continue;
      opaque++;
      const int sx = x0 + (hflip ? 7 - tc : tc);
      const int sy = y0 + (vflip ? 7 - tr : tr);
      if (sx < 0 || sx >= width || sy < 0 || sy >= kHdHeight)
        continue;
      const PpuIdentityPixel *other = &s_gbuf[(size_t)sy * kPpuBufWidth + sx];
      const uint32_t r = sub ? other->sub_ref : other->main_ref;
      if ((r & kPpuIdentRef_Valid) && ((r & 0x7fff) & ~7u) == base &&
          (int)((r >> kPpuIdentRef_ColShift) & 7) == tc && (int)(r & 7) == tr)
        visible++;
    }
  }
  if (!opaque)
    return;
  const bool inside = x0 >= kHdCropRing && x0 + 8 + kHdCropRing <= width &&
                      y0 >= kHdCropRing && y0 + 8 + kHdCropRing <= kHdHeight;
  /* Palette quality: how many distinct colors the character's own indices
   * show with this palette, and (tie-break) how bright they are. Without it
   * a scene that fades in by writing CGRAM kept the first, still-black
   * sighting of each character (equal geometry scores never replace the
   * first), and the pack got flat one-index tiles: the Nintendo logo. */
  const unsigned colors_n = depth == kPpuIdentDepth_2bpp ? 4 : 16;
  const unsigned base_index = pixel_index & ~(colors_n - 1);
  bool used_index[16] = {false};
  for (int tr = 0; tr < 8; tr++)
    for (int tc = 0; tc < 8; tc++)
      used_index[DecodeCharTexel(words, depth, (unsigned)tc, (unsigned)tr) &
                 15] = true;
  uint16_t distinct[16];
  int distinct_n = 0;
  unsigned luminance = 0;
  for (unsigned i = 1; i < colors_n; i++) {
    if (!used_index[i])
      continue;
    const uint16_t c = s_lines[y].cgram[base_index + i] & 0x7fff;
    luminance += (c & 31u) + ((c >> 5) & 31u) + ((c >> 10) & 31u);
    bool seen = false;
    for (int k = 0; k < distinct_n; k++)
      seen |= distinct[k] == c;
    if (!seen)
      distinct[distinct_n++] = c;
  }
  const float score = 4.0f * (float)visible / (float)opaque +
                      (inside ? 1.0f : 0.0f) +
                      ((gp->flags & kPpuIdentFlag_Math) ? 0.0f : 1.0f) +
                      0.5f * (float)distinct_n + luminance / 100000.0f;
  if (score <= record->score)
    return;
  record->score = score;
  record->depth = (uint8_t)depth;
  record->hflip = hflip;
  record->vflip = vflip;
  memcpy(record->raw, words, sizeof record->raw);
  const unsigned colors = depth == kPpuIdentDepth_2bpp ? 4 : 16;
  const unsigned palette_base = pixel_index & ~(colors - 1);
  for (unsigned i = 0; i < 16; i++) {
    const uint16_t c = i < colors ? s_lines[y].cgram[palette_base + i] : 0;
    const unsigned rgb[3] = {c & 31u, (c >> 5) & 31u, (c >> 10) & 31u};
    for (int k = 0; k < 3; k++)
      record->palette[i][k] = (uint8_t)((rgb[k] << 3) | (rgb[k] >> 2));
  }
  /* has_crop: 1 = layer-isolated context (its alpha is the layer's own
   * transparency), 2 = composed-frame fallback (alpha carries nothing). */
  record->has_crop = 1;
  if (!LayerContext(ppu, pixel_layer, base, depth, hflip, vflip,
                    x0 - (width - kPpuXPixels) / 2, y0, record->crop)) {
    FrameContext(ppu, width, x0, y0, record->crop);
    record->has_crop = 2;
  }
  /* Repaint the tile itself from its own palette: a texel another layer
   * covers in this occurrence, or one tinted by color math, must still
   * reach the upscaler as this character's art. */
  for (int tr = 0; tr < 8; tr++) {
    for (int tc = 0; tc < 8; tc++) {
      const unsigned value =
          DecodeCharTexel(words, depth, (unsigned)tc, (unsigned)tr);
      if (!value)
        continue;
      const int cx = kHdCropRing + (hflip ? 7 - tc : tc);
      const int cy = kHdCropRing + (vflip ? 7 - tr : tr);
      uint8_t *out = &record->crop[(cy * kHdCropSize + cx) * 4];
      memcpy(out, record->palette[value], 3);
      out[3] = 255;
    }
  }
}

/* dump.bin: "DKC1HDD1", u32 version 3, u32 count, then per record
 * u64 key, u8 depth, u8 hflip, u8 vflip, u8 has_crop, f32 score, u32 count,
 * u16 raw[16], u8 palette[16][3], u8 crop[40][40][4] (RGBA, alpha 0 =
 * transparent on the character's layer). Private: it holds
 * graphics decoded from the user's ROM and must never enter the repository. */
static void WriteDump(void) {
  if (!s_dump_dir[0] || !s_dump_count)
    return;
  char path[600];
  snprintf(path, sizeof path, "%s/dump.bin", s_dump_dir);
  FILE *file = fopen(path, "wb");
  if (!file) {
    fprintf(stderr, "[hd] cannot write %s\n", path);
    return;
  }
  const uint32_t header[2] = {3, (uint32_t)s_dump_count};
  fwrite("DKC1HDD1", 1, 8, file);
  fwrite(header, sizeof header, 1, file);
  for (size_t i = 0; i < s_dump_count; i++) {
    const HdDumpRecord *r = &s_dump[i];
    fwrite(&r->key, 8, 1, file);
    fwrite(&r->depth, 1, 4, file);
    fwrite(&r->score, 4, 1, file);
    fwrite(&r->count, 4, 1, file);
    fwrite(r->raw, 2, 16, file);
    fwrite(r->palette, 1, sizeof r->palette, file);
    fwrite(r->crop, 1, sizeof r->crop, file);
  }
  fclose(file);
  fprintf(stderr, "[hd] dumped %zu characters to %s (8bpp sightings skipped: %llu)\n",
          s_dump_count, path, (unsigned long long)s_dump_8bpp_sightings);
}

/* ---- Composition --------------------------------------------------------- */

/* Recompose native rows [y0, y1). Reads shared state only (pack slots are
 * resolved beforehand), except when dumping, which runs on one thread. */
static void ComposeRows(Ppu *ppu, int width, int y0, int y1,
                        Dkc1HdStats *stats) {
  const bool pack = s_source == kDkc1HdSourcePack;
  const bool dumping = s_dump_dir[0] != 0;
  for (int y = y0; y < y1; y++) {
    const uint32_t *native = (const uint32_t *)(ppu->renderBuffer +
                                                (size_t)y * ppu->renderPitch);
    const PpuIdentityLine *line = &s_lines[y];
    HdLineMaps maps;
    BuildLineMaps(line->brightness, &maps);
    for (int x = 0; x < width; x++) {
      const PpuIdentityPixel *gp = &s_gbuf[(size_t)y * kPpuBufWidth + x];
      const uint32_t native_rgb = native[x] & 0xffffff;
      stats->pixels++;
      if (!line->composed || !(gp->flags & kPpuIdentFlag_Composed)) {
        stats->uncomposed++;
        FillBlock(x, y, native_rgb);
        continue;
      }
      if (gp->flags & kPpuIdentFlag_Black) {
        stats->black++;
        FillBlock(x, y, native_rgb);
        continue;
      }
      /* Anything drawn over renderBuffer after the scan (Baby Kong, edge
       * bars, diagnostics) no longer matches its record: keep it native. */
      if (ComposeColor(line, &maps, gp->main_index, gp->sub_index,
                       gp->flags) != native_rgb) {
        stats->self_check_fail++;
        FillBlock(x, y, native_rgb);
        continue;
      }
      const bool sub_texel = (gp->flags & kPpuIdentFlag_Math) &&
                             !(gp->flags & kPpuIdentFlag_SubFixed);
      if (gp->main_ref & kPpuIdentRef_Valid) {
        stats->main_identity++;
        if (dumping) DumpOccurrence(ppu, width, x, y, gp, false);
      }
      if (sub_texel && (gp->sub_ref & kPpuIdentRef_Valid)) {
        stats->sub_identity++;
        if (dumping) DumpOccurrence(ppu, width, x, y, gp, true);
      }

      if (pack) {
        const uint16_t *main_tile = PackTile(ppu->vram, gp->main_ref);
        const uint16_t *sub_tile =
            sub_texel ? PackTile(ppu->vram, gp->sub_ref) : NULL;
        const uint16_t *cover_tile = PackTile(ppu->vram, gp->cover_ref);
        if (main_tile) stats->pack_hits++;
        else if (gp->main_ref & kPpuIdentRef_Valid) stats->pack_misses++;
        if (!main_tile && !sub_tile && !cover_tile) {
          FillBlock(x, y, s_debug_misses && (gp->main_ref & kPpuIdentRef_Valid)
                              ? 0xff00ff : native_rgb);
          continue;
        }
        HdTexelWalk mw, cw, sw;
        const bool has_main = TexelWalk(main_tile, gp->main_ref, &mw);
        const bool has_cover = TexelWalk(cover_tile, gp->cover_ref, &cw);
        const bool has_sub = TexelWalk(sub_tile, gp->sub_ref, &sw);
        const uint8_t main_base = PaletteBase(gp->main_index, gp->main_ref);
        const uint8_t sub_base = PaletteBase(gp->sub_index, gp->sub_ref);
        /* What a transparent main texel shows: the native main color when
         * nothing is known below it, else the backdrop or the native
         * color of the pixel underneath. */
        const HdRgb main_native = Rgb15(line->cgram[gp->main_index]);
        const HdRgb below = !(gp->under_ref & kPpuIdentRef_Valid)
                                ? main_native
                                : Rgb15(line->cgram[gp->under_index]);
        const HdRgb sub_native =
            Rgb15((gp->flags & kPpuIdentFlag_SubFixed)
                      ? line->fixed_color
                      : line->cgram[gp->sub_index]);
        for (int v = 0; v < s_scale; v++) {
          uint32_t *out = BlockRow(x, y, v);
          for (int u = 0; u < s_scale; u++) {
            HdRgb m = has_main
                ? BlendTexel(line, mw.p[v * mw.dv + u * mw.du], main_base,
                             below)
                : main_native;
            if (has_cover)
              m = BlendTexel(line, cw.p[v * cw.dv + u * cw.du],
                             gp->cover_base, m);
            const HdRgb sub = has_sub
                ? BlendTexel(line, sw.p[v * sw.dv + u * sw.du], sub_base,
                             sub_native)
                : sub_native;
            out[u] = FinishColor(line, gp->flags, m, sub);
          }
        }
        continue;
      }

      if (gp->under_index && (gp->under_ref & kPpuIdentRef_Valid)) {
        stats->under_checked++;
        const unsigned t = DecodeTexel(ppu->vram, gp->under_ref);
        if (!t || (PaletteBase(gp->under_index, gp->under_ref) | t) !=
                      gp->under_index)
          stats->under_mismatch++;
      }
      if (gp->cover_ref & kPpuIdentRef_Valid) {
        stats->cover_checked++;
        if (DecodeTexel(ppu->vram, gp->cover_ref))
          stats->cover_mismatch++;
      }
      bool mismatch = false;
      const uint8_t main_index =
          IdentityIndex(ppu->vram, gp->main_ref, gp->main_index, &mismatch);
      const uint8_t sub_index =
          sub_texel ? IdentityIndex(ppu->vram, gp->sub_ref, gp->sub_index,
                                    &mismatch)
                    : gp->sub_index;
      if (mismatch) stats->ref_mismatch++;
      const uint32_t hd =
          ComposeColor(line, &maps, main_index, sub_index, gp->flags);
      if (hd != native_rgb) stats->equiv_mismatch++;
      FillBlock(x, y, hd);
      if (s_source == kDkc1HdSourceGrid)
        DrawGridEdges(x, y, gp->main_ref);
    }
  }
}

/* ---- Tile-seam deblocking ----------------------------------------------
 * The pack stores one HD version per character, upscaled in one context;
 * the same character sits next to different neighbours elsewhere, and the
 * upscale's smooth interior makes the 8x8 edges stand out where the native
 * art's grain hid them. Where two pixels of the same layer meet across a
 * tile edge and the native picture has no real edge there (each channel
 * within kHdSeamNativeStep), the HD step across the edge is spread over a
 * ramp of one native pixel on each side. SeamBits marks the edges of every
 * native pixel; CPU (DeblockFrame) and GPU (deblock pass) apply the same
 * integer ramp, horizontal and vertical both from the composed frame. */
enum {
  kHdSeamUp = 1, kHdSeamRight = 2, kHdSeamDown = 4, kHdSeamLeft = 8,
};

static FORCEINLINE bool NativeClose(uint32_t a, uint32_t b) {
  for (int shift = 0; shift < 24; shift += 8) {
    const int d = (int)((a >> shift) & 255) - (int)((b >> shift) & 255);
    if (d > kHdSeamNativeStep || d < -kHdSeamNativeStep)
      return false;
  }
  return true;
}

/* Is there a seam between pixel a and the next pixel b (to its right, or
 * below when `vertical`)? */
static bool SeamBetween(const uint16_t *vram, const PpuIdentityPixel *a,
                        const PpuIdentityPixel *b, uint32_t native_a,
                        uint32_t native_b, bool vertical) {
  unsigned sa, sb;
  if (vertical) {
    sa = a->main_ref & 7;
    sb = b->main_ref & 7;
    if (a->main_ref & kPpuIdentRef_VFlip) sa = 7 - sa;
    if (b->main_ref & kPpuIdentRef_VFlip) sb = 7 - sb;
  } else {
    sa = (a->main_ref >> kPpuIdentRef_ColShift) & 7;
    sb = (b->main_ref >> kPpuIdentRef_ColShift) & 7;
    if (a->main_ref & kPpuIdentRef_HFlip) sa = 7 - sa;
    if (b->main_ref & kPpuIdentRef_HFlip) sb = 7 - sb;
  }
  if (sa != 7 || sb != 0)
    return false;
  const uint8_t need = kPpuIdentFlag_Composed;
  if ((a->flags & (need | kPpuIdentFlag_Black)) != need ||
      (b->flags & (need | kPpuIdentFlag_Black)) != need)
    return false;
  if (!(a->main_ref & kPpuIdentRef_Valid) ||
      !(b->main_ref & kPpuIdentRef_Valid) || a->main_layer != b->main_layer)
    return false;
  /* Both sides HD: a character missing from the pack stays native pixels,
   * whose edges are the art's own. */
  return NativeClose(native_a, native_b) &&
         PackTileIndex(vram, a->main_ref) >= 0 &&
         PackTileIndex(vram, b->main_ref) >= 0;
}

static unsigned SeamBits(const Ppu *ppu, int width, int x, int y) {
  const PpuIdentityPixel *row = &s_gbuf[(size_t)y * kPpuBufWidth];
  const uint32_t *native = (const uint32_t *)(ppu->renderBuffer +
                                              (size_t)y * ppu->renderPitch);
  unsigned bits = 0;
  if (x + 1 < width &&
      SeamBetween(ppu->vram, &row[x], &row[x + 1], native[x], native[x + 1],
                  false))
    bits |= kHdSeamRight;
  if (x > 0 &&
      SeamBetween(ppu->vram, &row[x - 1], &row[x], native[x - 1], native[x],
                  false))
    bits |= kHdSeamLeft;
  if (y + 1 < kHdHeight) {
    const uint32_t *below = (const uint32_t *)((const uint8_t *)native +
                                               ppu->renderPitch);
    if (SeamBetween(ppu->vram, &row[x], &row[x + kPpuBufWidth], native[x],
                    below[x], true))
      bits |= kHdSeamDown;
  }
  if (y > 0) {
    const uint32_t *above = (const uint32_t *)((const uint8_t *)native -
                                               ppu->renderPitch);
    if (SeamBetween(ppu->vram, &row[x - kPpuBufWidth], &row[x], above[x],
                    native[x], true))
      bits |= kHdSeamUp;
  }
  return bits;
}

/* RampSeam: shift = d * (n - i) / (2n + 1), truncated toward zero. */
static FORCEINLINE int Ramp(int d, int i, int n) {
  const int m = (d < 0 ? -d : d) * (n - i) / (2 * n + 1);
  return d < 0 ? -m : m;
}

static void DeblockFrame(const Ppu *ppu, int width) {
  const int s = s_scale, w = s_out_width;
  static uint32_t *source;
  static size_t capacity;
  const size_t pixels = (size_t)w * s_out_height;
  if (pixels > capacity) {
    uint32_t *grown = realloc(source, pixels * sizeof *grown);
    if (!grown)
      return;
    source = grown;
    capacity = pixels;
  }
  memcpy(source, s_out, pixels * sizeof *source);
  for (int y = 0; y < kHdHeight; y++) {
    for (int x = 0; x < width; x++) {
      const unsigned bits = SeamBits(ppu, width, x, y);
      if (!bits)
        continue;
      for (int v = 0; v < s; v++) {
        const int py = y * s + v;
        for (int u = 0; u < s; u++) {
          const int px = x * s + u;
          const uint32_t c = source[(size_t)py * w + px];
          int out[3];
          for (int k = 0; k < 3; k++) {
            const int shift = 16 - 8 * k;
            int value = (int)((c >> shift) & 255);
#define AT(X, Y) ((int)((source[(size_t)(Y) * w + (X)] >> shift) & 255))
            if (bits & kHdSeamRight)
              value += Ramp(AT((x + 1) * s, py) - AT((x + 1) * s - 1, py),
                            s - 1 - u, s);
            if (bits & kHdSeamLeft)
              value -= Ramp(AT(x * s, py) - AT(x * s - 1, py), u, s);
            if (bits & kHdSeamDown)
              value += Ramp(AT(px, (y + 1) * s) - AT(px, (y + 1) * s - 1),
                            s - 1 - v, s);
            if (bits & kHdSeamUp)
              value -= Ramp(AT(px, y * s) - AT(px, y * s - 1), v, s);
#undef AT
            out[k] = value < 0 ? 0 : value > 255 ? 255 : value;
          }
          s_out[(size_t)py * w + px] =
              (uint32_t)out[0] << 16 | (uint32_t)out[1] << 8 | (uint32_t)out[2];
        }
      }
    }
  }
}

/* ---- GPU composition inputs (dkc1_hd_gpu.c) -------------------------------
 * The same per-pixel decisions as ComposeRows' pack path, one RGBA32UI
 * texel per native pixel:
 *   ref (26 bits): tile index (17 bits, kDkc1HdGpuNoTile = none) |
 *                  col << 17 | row << 20 | hflip << 23 | vflip << 24 |
 *                  2bpp << 25
 *   x = main ref  | flags bits 0-4 << 27
 *   y = cover ref | flags bits 5-7 << 27 | below-known << 30
 *   z = sub ref (none unless the sub-screen texel shows) | mode << 27 |
 *       tile seams (deblocking): up << 26, right << 29, down << 30,
 *       left << 31
 *   w = main index | sub index << 8 | under index << 16 | cover base << 24,
 *       or for a native-mode pixel its 0x00RRGGBB color
 * Line rows: CGRAM[256], fixed color, brightness. */
static bool s_gpu;
static bool s_gpu_verify;
static uint32_t *s_gpu_g;
static uint16_t s_gpu_lines[kHdHeight * kDkc1HdGpuLineStride];
static Dkc1HdGpuInputs s_gpu_inputs;

static FORCEINLINE uint32_t GpuRef(uint32_t ref, int32_t index) {
  return (index < 0 ? (uint32_t)kDkc1HdGpuNoTile : (uint32_t)index) |
         ((ref >> kPpuIdentRef_ColShift) & 7) << 17 | (ref & 7) << 20 |
         ((ref & kPpuIdentRef_HFlip) ? 1u << 23 : 0) |
         ((ref & kPpuIdentRef_VFlip) ? 1u << 24 : 0) |
         (RefColors(ref) == 4 ? 1u << 25 : 0);
}

/* Pack tile index for a ref. Neighbouring pixels nearly always share a
 * character, so each role (main, cover, sub) remembers its last. */
typedef struct HdTileMemo {
  uint32_t base;
  int32_t index;
} HdTileMemo;

static FORCEINLINE int32_t MemoTile(const uint16_t *vram, uint32_t ref,
                                    HdTileMemo *memo) {
  if (!(ref & kPpuIdentRef_Valid))
    return -1;
  const uint32_t base = (ref & 0x7fff) & ~7u;
  if (base != memo->base) {
    memo->base = base;
    memo->index = PackTileIndex(vram, ref);
  }
  return memo->index;
}

static void EncodeGpuRows(Ppu *ppu, int width, int y0, int y1,
                          Dkc1HdStats *stats) {
  for (int y = y0; y < y1; y++) {
    const uint32_t *native = (const uint32_t *)(ppu->renderBuffer +
                                                (size_t)y * ppu->renderPitch);
    const PpuIdentityLine *line = &s_lines[y];
    uint16_t *line_out = &s_gpu_lines[y * kDkc1HdGpuLineStride];
    memcpy(line_out, line->cgram, sizeof line->cgram);
    line_out[256] = line->fixed_color;
    line_out[257] = line->brightness;
    HdLineMaps maps;
    BuildLineMaps(line->brightness, &maps);
    /* ComposeColor of a pixel without color math or clipping, per index:
     * the self-check below only computes the full path for math pixels. */
    uint32_t plain[256];
    for (int i = 0; i < 256; i++) {
      const uint16_t c = line->cgram[i];
      plain[i] = (uint32_t)maps.mult[(c >> 10) & 31] |
                 (uint32_t)maps.mult[(c >> 5) & 31] << 8 |
                 (uint32_t)maps.mult[c & 31] << 16;
    }
    HdTileMemo main_memo = {~0u, -1}, cover_memo = {~0u, -1},
               sub_memo = {~0u, -1};
    const PpuIdentityPixel *row = &s_gbuf[(size_t)y * kPpuBufWidth];
    uint32_t *g = &s_gpu_g[(size_t)y * width * 4];
    for (int x = 0; x < width; x++, g += 4) {
      const PpuIdentityPixel *gp = &row[x];
      const uint32_t native_rgb = native[x] & 0xffffff;
      /* Native mode unless the pixel is recomposed below. */
      g[0] = g[1] = kDkc1HdGpuNoTile;
      g[2] = kDkc1HdGpuNoTile | (uint32_t)kDkc1HdGpuModeNative << 27;
      g[3] = native_rgb;
      stats->pixels++;
      if (!line->composed || !(gp->flags & kPpuIdentFlag_Composed)) {
        stats->uncomposed++;
        continue;
      }
      if (gp->flags & kPpuIdentFlag_Black) {
        stats->black++;
        continue;
      }
      const uint32_t expected =
          (gp->flags & (kPpuIdentFlag_Math | kPpuIdentFlag_Clip))
              ? ComposeColor(line, &maps, gp->main_index, gp->sub_index,
                             gp->flags)
              : plain[gp->main_index];
      if (expected != native_rgb) {
        stats->self_check_fail++;
        continue;
      }
      const bool sub_texel = (gp->flags & kPpuIdentFlag_Math) &&
                             !(gp->flags & kPpuIdentFlag_SubFixed);
      if (gp->main_ref & kPpuIdentRef_Valid) stats->main_identity++;
      if (sub_texel && (gp->sub_ref & kPpuIdentRef_Valid))
        stats->sub_identity++;
      const int32_t main_index = MemoTile(ppu->vram, gp->main_ref, &main_memo);
      const int32_t sub_index =
          sub_texel ? MemoTile(ppu->vram, gp->sub_ref, &sub_memo) : -1;
      const int32_t cover_index =
          MemoTile(ppu->vram, gp->cover_ref, &cover_memo);
      if (main_index >= 0) stats->pack_hits++;
      else if (gp->main_ref & kPpuIdentRef_Valid) stats->pack_misses++;
      if (main_index < 0 && sub_index < 0 && cover_index < 0) {
        if (s_debug_misses && (gp->main_ref & kPpuIdentRef_Valid))
          g[2] = kDkc1HdGpuNoTile | (uint32_t)kDkc1HdGpuModeMagenta << 27;
        continue;
      }
      const uint32_t flags = gp->flags;
      const bool below_known = (gp->under_ref & kPpuIdentRef_Valid) != 0;
      g[0] = GpuRef(gp->main_ref, main_index) | (flags & 31u) << 27;
      g[1] = GpuRef(gp->cover_ref, cover_index) | ((flags >> 5) & 7u) << 27 |
             (below_known ? 1u << 30 : 0);
      const unsigned seams = SeamBits(ppu, width, x, y);
      g[2] = GpuRef(gp->sub_ref, sub_index) |
             (uint32_t)kDkc1HdGpuModeCompose << 27 |
             ((seams & kHdSeamUp) ? 1u << 26 : 0) |
             ((seams & kHdSeamRight) ? 1u << 29 : 0) |
             ((seams & kHdSeamDown) ? 1u << 30 : 0) |
             ((seams & kHdSeamLeft) ? 1u << 31 : 0);
      g[3] = gp->main_index | (uint32_t)gp->sub_index << 8 |
             (uint32_t)gp->under_index << 16 | (uint32_t)gp->cover_base << 24;
    }
  }
}

static bool EnsureGpuBuffers(int width) {
  const size_t need = (size_t)width * kHdHeight * 4;
  static size_t capacity;
  if (need > capacity) {
    uint32_t *g = realloc(s_gpu_g, need * sizeof *g);
    if (!g) return false;
    s_gpu_g = g;
    capacity = need;
  }
  return true;
}

void Dkc1HdSetGpuComposition(bool enabled) {
  s_gpu = enabled;
  const char *verify = getenv("DKC1_HD_GPU_VERIFY");
  s_gpu_verify = enabled && verify && *verify && *verify != '0';
}

bool Dkc1HdGpuComposition(void) {
  return s_gpu;
}

const Dkc1HdGpuInputs *Dkc1HdGpuFrame(void) {
  if (!s_gpu || !Dkc1HdEnabled() || s_source != kDkc1HdSourcePack ||
      !s_gpu_inputs.width)
    return NULL;
  return &s_gpu_inputs;
}

/* Hash every character the frame references once, before the parallel
 * pass, so workers only read s_chars. */
static void ResolvePackSlots(Ppu *ppu, int width) {
  for (int y = 0; y < kHdHeight; y++) {
    const PpuIdentityPixel *row = &s_gbuf[(size_t)y * kPpuBufWidth];
    for (int x = 0; x < width; x++) {
      (void)PackTile(ppu->vram, row[x].main_ref);
      (void)PackTile(ppu->vram, row[x].sub_ref);
      (void)PackTile(ppu->vram, row[x].cover_ref);
      if (row[x].under_index)
        (void)PackTile(ppu->vram, row[x].under_ref);
    }
  }
}

/* ---- Worker pool --------------------------------------------------------
 * Rows are independent, so the frame is split into bands. pthreads on
 * Linux/macOS/Switch, native threads on Windows; DKC1_HD_THREADS=N
 * overrides the count (1 = single-threaded). */
enum { kHdMaxWorkers = 8 };

/* MSVC uses native threads. MinGW-w64 uses winpthreads instead: its
 * <windows.h> inline helpers clash between translation units in C11. */
#if defined(_WIN32) && !defined(__MINGW32__)
#define HD_WIN32_THREADS 1
#else
#define HD_WIN32_THREADS 0
#endif

typedef struct HdBand {
  Ppu *ppu;
  int width, y0, y1;
  bool gpu;  /* encode GPU inputs instead of composing on the CPU */
  Dkc1HdStats stats;
} HdBand;

#if HD_WIN32_THREADS
#include <windows.h>
typedef HANDLE HdThread;
static CRITICAL_SECTION s_pool_lock;
static CONDITION_VARIABLE s_pool_wake, s_pool_done;
#define POOL_LOCK() EnterCriticalSection(&s_pool_lock)
#define POOL_UNLOCK() LeaveCriticalSection(&s_pool_lock)
#define POOL_WAIT(cv) SleepConditionVariableCS(&(cv), &s_pool_lock, INFINITE)
#define POOL_BROADCAST(cv) WakeAllConditionVariable(&(cv))
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t HdThread;
static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_pool_wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t s_pool_done = PTHREAD_COND_INITIALIZER;
#define POOL_LOCK() pthread_mutex_lock(&s_pool_lock)
#define POOL_UNLOCK() pthread_mutex_unlock(&s_pool_lock)
#define POOL_WAIT(cv) pthread_cond_wait(&(cv), &s_pool_lock)
#define POOL_BROADCAST(cv) pthread_cond_broadcast(&(cv))
#endif

static int s_workers = -1;  /* extra threads besides the caller */
static HdBand s_bands[kHdMaxWorkers + 1];
static uint64_t s_pool_generation;
static int s_pool_pending;

static void EncodeGpuRows(Ppu *ppu, int width, int y0, int y1,
                          Dkc1HdStats *stats);

static void RunBand(HdBand *band) {
  memset(&band->stats, 0, sizeof band->stats);
  if (band->gpu)
    EncodeGpuRows(band->ppu, band->width, band->y0, band->y1, &band->stats);
  else
    ComposeRows(band->ppu, band->width, band->y0, band->y1, &band->stats);
}

#if HD_WIN32_THREADS
static DWORD WINAPI WorkerMain(LPVOID arg)
#else
static void *WorkerMain(void *arg)
#endif
{
  const int index = (int)(intptr_t)arg;
#ifdef __SWITCH__
  Dkc1SwitchPinWorker(index);
#endif
  uint64_t seen = 0;
  for (;;) {
    POOL_LOCK();
    while (s_pool_generation == seen)
      POOL_WAIT(s_pool_wake);
    seen = s_pool_generation;
    POOL_UNLOCK();
    RunBand(&s_bands[index]);
    POOL_LOCK();
    if (--s_pool_pending == 0)
      POOL_BROADCAST(s_pool_done);
    POOL_UNLOCK();
  }
#if HD_WIN32_THREADS
  return 0;
#else
  return NULL;
#endif
}

static int CpuCount(void) {
#if HD_WIN32_THREADS
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return (int)info.dwNumberOfProcessors;
#elif defined(__MINGW32__)
  const int n = pthread_num_processors_np();
  return n > 0 ? n : 1;
#elif defined(__SWITCH__)
  return 3;  /* one core stays with the emulation thread */
#else
  const long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
#endif
}

static void StartWorkers(void) {
  const char *text = getenv("DKC1_HD_THREADS");
  int threads = text && *text ? atoi(text) : CpuCount();
  if (threads > 4 && !(text && *text)) threads = 4;
  if (threads < 1) threads = 1;
  if (threads > kHdMaxWorkers + 1) threads = kHdMaxWorkers + 1;
  s_workers = 0;
#if HD_WIN32_THREADS
  InitializeCriticalSection(&s_pool_lock);
  InitializeConditionVariable(&s_pool_wake);
  InitializeConditionVariable(&s_pool_done);
#endif
  for (int i = 1; i < threads; i++) {
    HdThread thread;
#if HD_WIN32_THREADS
    thread = CreateThread(NULL, 0, WorkerMain, (LPVOID)(intptr_t)i, 0, NULL);
    if (!thread) break;
    CloseHandle(thread);
#else
    if (pthread_create(&thread, NULL, WorkerMain, (void *)(intptr_t)i) != 0)
      break;
    pthread_detach(thread);
#endif
    s_workers++;
  }
}

/* Monotonic clock for the compositor timing statistic. */
static uint64_t NowNs(void) {
#if HD_WIN32_THREADS
  LARGE_INTEGER counter, frequency;
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return (uint64_t)(counter.QuadPart * 1000000000.0 / frequency.QuadPart);
#else
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
#endif
}

static void AddStats(Dkc1HdStats *total, const Dkc1HdStats *part) {
  uint64_t *dst = (uint64_t *)total;
  const uint64_t *src = (const uint64_t *)part;
  for (size_t i = 0; i < sizeof *total / sizeof(uint64_t); i++)
    dst[i] += src[i];
}

/* Compose (or GPU-encode) the frame in row bands across the pool. */
static void RunBands(Ppu *ppu, int width, bool gpu) {
  if (s_workers < 0)
    StartWorkers();
  if (s_dump_dir[0] || s_workers == 0) {
    if (gpu)
      EncodeGpuRows(ppu, width, 0, kHdHeight, &s_stats);
    else
      ComposeRows(ppu, width, 0, kHdHeight, &s_stats);
    return;
  }
  const int bands = s_workers + 1;
  for (int i = 0; i < bands; i++) {
    s_bands[i].ppu = ppu;
    s_bands[i].width = width;
    s_bands[i].gpu = gpu;
    s_bands[i].y0 = kHdHeight * i / bands;
    s_bands[i].y1 = kHdHeight * (i + 1) / bands;
  }
  POOL_LOCK();
  s_pool_pending = s_workers;
  s_pool_generation++;
  POOL_BROADCAST(s_pool_wake);
  POOL_UNLOCK();
  RunBand(&s_bands[0]);
  POOL_LOCK();
  while (s_pool_pending)
    POOL_WAIT(s_pool_done);
  POOL_UNLOCK();
  for (int i = 0; i < bands; i++)
    AddStats(&s_stats, &s_bands[i].stats);
}

static void ComposeFrame(Ppu *ppu, int width) {
  RunBands(ppu, width, false);
  if (s_source == kDkc1HdSourcePack && s_deblock)
    DeblockFrame(ppu, width);
}

void Dkc1HdFinishFrame(Ppu *ppu, int width) {
  PpuSetIdentityCapture(ppu, NULL, 0, NULL);
  if (!Dkc1HdEnabled() || !ppu->renderBuffer || width <= 0 ||
      width > kPpuBufWidth || !EnsureOutput(width))
    return;
  const uint64_t start = NowNs();
  s_stats.frames++;
  {
    /* DKC1_HD_CAPTURE_ONLY=1 (measurement): stop after the PPU capture. */
    static int capture_only = -1;
    if (capture_only < 0) {
      const char *v = getenv("DKC1_HD_CAPTURE_ONLY");
      capture_only = v && *v && *v != '0';
    }
    if (capture_only)
      return;
  }
  if (s_source == kDkc1HdSourcePack && !s_gpu)
    ResolvePackSlots(ppu, width);
  if (s_gpu && s_source == kDkc1HdSourcePack && EnsureGpuBuffers(width)) {
    RunBands(ppu, width, true);
    s_gpu_inputs = (Dkc1HdGpuInputs){
      .width = width,
      .height = kHdHeight,
      .scale = s_scale,
      .g = s_gpu_g,
      .lines = s_gpu_lines,
      .tiles = s_pack.tiles,
      .tile_count = s_pack.count,
      .tiles_generation = s_pack.generation,
      .deblock = s_deblock,
    };
    if (s_gpu_verify) {
      /* Reference CPU composition of the same frame for comparison. */
      Dkc1HdStats discard = s_stats;
      ComposeFrame(ppu, width);
      s_stats = discard;
    }
  } else {
    ComposeFrame(ppu, width);
  }
  s_stats.compose_ns += NowNs() - start;
}

const uint32_t *Dkc1HdOutput(int *width, int *height, size_t *pitch_pixels) {
  if (!Dkc1HdEnabled() || !s_out || !s_out_width)
    return NULL;
  if (s_gpu && s_source == kDkc1HdSourcePack && !s_gpu_verify)
    return NULL;  /* composed on the GPU; see Dkc1HdGpuFrame */
  if (width) *width = s_out_width;
  if (height) *height = s_out_height;
  if (pitch_pixels) *pitch_pixels = (size_t)s_out_width;
  return s_out;
}

void Dkc1HdGetStats(Dkc1HdStats *stats) {
  if (stats) *stats = s_stats;
}

bool Dkc1HdWriteGbuffer(const char *path) {
  FILE *file = path ? fopen(path, "wb") : NULL;
  if (!file)
    return false;
  fwrite(s_gbuf, sizeof s_gbuf[0], kHdHeight * kPpuBufWidth, file);
  return fclose(file) == 0;
}

bool Dkc1HdWritePpm(const char *path) {
  int width, height;
  const uint32_t *pixels = Dkc1HdOutput(&width, &height, NULL);
  if (!pixels || !path)
    return false;
  FILE *file = fopen(path, "wb");
  if (!file)
    return false;
  fprintf(file, "P6\n%d %d\n255\n", width, height);
  for (size_t i = 0; i < (size_t)width * height; i++) {
    const uint8_t rgb[3] = {(uint8_t)(pixels[i] >> 16),
                            (uint8_t)(pixels[i] >> 8), (uint8_t)pixels[i]};
    fwrite(rgb, 1, 3, file);
  }
  return fclose(file) == 0;
}
