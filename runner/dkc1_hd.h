#ifndef DKC1_HD_H
#define DKC1_HD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Ppu Ppu;

/* HD texture mod (docs/HD_REMASTER.md). Presentation only: it reads the PPU's
 * per-pixel tile identity and recomposes the frame at an integer scale with
 * the native renderer's exact palette, color math and brightness. Nothing it
 * does reaches WRAM, VRAM, CGRAM, OAM, renderBuffer or a save state.
 *
 * Phase 1 texture sources need no external pack:
 *   identity  each HD texel is the native texel (nearest). The recomposed
 *             frame must reduce to the native frame exactly; any difference
 *             is a capture or recomposition bug.
 *   grid      identity with every tile's top/left HD edge darkened, to show
 *             tile alignment on screen.
 * Phase 2 adds external packs (tools/hd_pack.py): HD tiles keyed by a hash
 * of the character's contents, stored as palette-relative indices so the
 * live CGRAM, fades and color math still apply.
 */
typedef enum Dkc1HdSource {
  kDkc1HdSourceNone = 0,
  kDkc1HdSourceIdentity,
  kDkc1HdSourceGrid,
  kDkc1HdSourcePack,
} Dkc1HdSource;

typedef struct Dkc1HdStats {
  uint64_t frames;
  uint64_t pixels;           /* native pixels examined */
  uint64_t uncomposed;       /* no compositor record (left as native) */
  uint64_t black;            /* outside the live world span */
  uint64_t self_check_fail;  /* record disagrees with renderBuffer (host overdraw) */
  uint64_t main_identity;    /* pixels recomposed from a main-screen texel */
  uint64_t sub_identity;     /* ... whose math operand was also a texel */
  uint64_t ref_mismatch;     /* identity texel disagrees with the native index */
  uint64_t equiv_mismatch;   /* identity/grid-free HD block != native pixel */
  uint64_t pack_hits;        /* main texels served by the pack */
  uint64_t pack_misses;      /* main texels whose character the pack lacks */
  uint64_t under_checked;    /* identity: under refs checked against VRAM */
  uint64_t under_mismatch;   /* ... that were not the recorded opaque texel */
  uint64_t cover_checked;    /* identity: cover refs checked against VRAM */
  uint64_t cover_mismatch;   /* ... that were not transparent */
  uint64_t compose_ns;       /* wall time spent in Dkc1HdFinishFrame */
} Dkc1HdStats;

/* Active only with a widescreen aspect (16:10 or 16:9); native 4:3 always
 * presents stock. DKC1_HD_DISABLE=1 forces stock. DKC1_HD_IDENTITY=1 or DKC1_HD_DEBUG=grid
 * selects a Phase 1 source; DKC1_HD_SCALE=1..4 picks the scale (default 4).
 * DKC1_HD_DEBUG=misses tints texels the pack lacks.
 * DKC1_HD_PACK=<dir> loads a pack (on Switch, sdmc:/switch/dkc1/hd/ is used
 * when present). DKC1_HD_DUMP=<dir> writes <dir>/dump.bin at exit with every
 * main-screen character seen and its best on-screen context. */
void Dkc1HdInitializeFromEnvironment(void);
void Dkc1HdSetSource(Dkc1HdSource source, int scale);
/* Load <dir>/tiles.bin (ROM digest and scale checked) and select it. */
bool Dkc1HdLoadPack(const char *dir, char *error, size_t error_size);
/* True while HD frames are produced: a source is loaded, the user toggle is
 * on and the aspect is widescreen. */
bool Dkc1HdEnabled(void);
/* A pack (or Phase 1 source) is loaded. */
bool Dkc1HdReady(void);
/* Mods > HD Textures. Independent of loading; on by default. */
void Dkc1HdSetEnabled(bool enabled);
bool Dkc1HdUserEnabled(void);
const char *Dkc1HdStatus(void);

/* Frame hooks around the PPU scan (dkc1_game.c). */
void Dkc1HdPrepareFrame(Ppu *ppu);
void Dkc1HdFinishFrame(Ppu *ppu, int width);

/* GPU composition (dkc1_hd_gpu.c). A host that can compose on the GPU
 * enables it; pack frames are then encoded for the shader instead of being
 * composed on the CPU, and Dkc1HdOutput returns NULL. DKC1_HD_GPU_VERIFY=1
 * also keeps the CPU composition so the two can be compared. */
enum {
  kDkc1HdGpuNoTile = 0x1ffff,
  kDkc1HdGpuLineStride = 258,  /* CGRAM[256], fixed colour, brightness */
  kDkc1HdGpuModeCompose = 0,
  kDkc1HdGpuModeNative = 1,
  kDkc1HdGpuModeMagenta = 2,
};

typedef struct Dkc1HdGpuInputs {
  int width, height, scale;  /* scale: 1, 2 or 4 */
  const uint32_t *g;         /* width * height RGBA32UI texels (dkc1_hd.c) */
  const uint16_t *lines;     /* height * kDkc1HdGpuLineStride */
  const uint16_t *tiles;     /* tile_count tiles of (8*scale)^2 texels */
  uint32_t tile_count;
  uint64_t tiles_generation; /* re-upload the atlas when this changes */
} Dkc1HdGpuInputs;

void Dkc1HdSetGpuComposition(bool enabled);
bool Dkc1HdGpuComposition(void);
/* Inputs for the current frame, or NULL when nothing is to be composed. */
const Dkc1HdGpuInputs *Dkc1HdGpuFrame(void);

/* Latest recomposed frame, 0x00RRGGBB, or NULL when disabled. */
const uint32_t *Dkc1HdOutput(int *width, int *height, size_t *pitch_pixels);
void Dkc1HdGetStats(Dkc1HdStats *stats);
bool Dkc1HdWritePpm(const char *path);
/* Raw PpuIdentityPixel rows of the last frame (224 x kPpuBufWidth). */
bool Dkc1HdWriteGbuffer(const char *path);

#endif
