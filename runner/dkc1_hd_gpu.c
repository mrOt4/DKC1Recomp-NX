/* GPU composition of HD texture frames (see dkc1_hd.h, docs/HD_REMASTER.md).
 *
 * The CPU only resolves which pack tile every native pixel shows
 * (Dkc1HdGpuFrame); this module uploads that G-buffer and the per-line
 * CGRAM, keeps the pack atlas, and runs two passes:
 *   1. compose - every HD subpixel: blend the texel's two palette colors,
 *                then the same color math and brightness as the CPU path
 *                (dkc1_hd.c FinishColor), in the same float operations;
 *   2. deblock - spread the step across tile seams the CPU marked (the
 *                same integer ramp as dkc1_hd.c DeblockFrame);
 *   3. present - fit to the viewport (sharp bilinear up, 4 taps down).
 * DKC1_HD_GPU_VERIFY reads pass 1 back and compares it with the CPU.
 *
 * Built for the Switch's Tegra: one RGBA32UI input texel per native pixel
 * (16 bytes), an atlas of 256 tiles per row so tile addresses are shifts,
 * power-of-two scales, no integer division in the shader, and three
 * rotating sets of input textures so an upload never waits for the GPU to
 * finish reading the previous frame's.
 *
 * Every GL entry point is loaded through SDL_GL_GetProcAddress, so the same
 * code runs on desktop GL 3.3 core (Windows) and GL / GLES 3 (Switch). */
#include "dkc1_hd_gpu.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned int GLbitfield;

enum {
  HD_GL_TEXTURE_2D = 0x0DE1,
  HD_GL_TEXTURE0 = 0x84C0,
  HD_GL_TEXTURE_MIN_FILTER = 0x2801,
  HD_GL_TEXTURE_MAG_FILTER = 0x2800,
  HD_GL_TEXTURE_WRAP_S = 0x2802,
  HD_GL_TEXTURE_WRAP_T = 0x2803,
  HD_GL_NEAREST = 0x2600,
  HD_GL_LINEAR = 0x2601,
  HD_GL_CLAMP_TO_EDGE = 0x812F,
  HD_GL_UNPACK_ALIGNMENT = 0x0CF5,
  HD_GL_PACK_ALIGNMENT = 0x0D05,
  HD_GL_UNPACK_ROW_LENGTH = 0x0CF2,
  HD_GL_RGBA = 0x1908,
  HD_GL_RGBA8 = 0x8058,
  HD_GL_RGBA32UI = 0x8D70,
  HD_GL_RGBA_INTEGER = 0x8D99,
  HD_GL_R8UI = 0x8232,
  HD_GL_RG8UI = 0x8238,
  HD_GL_RG_INTEGER = 0x8228,
  HD_GL_R16UI = 0x8234,
  HD_GL_RED_INTEGER = 0x8D94,
  HD_GL_UNSIGNED_BYTE = 0x1401,
  HD_GL_UNSIGNED_SHORT = 0x1403,
  HD_GL_UNSIGNED_INT = 0x1405,
  HD_GL_FRAGMENT_SHADER = 0x8B30,
  HD_GL_VERTEX_SHADER = 0x8B31,
  HD_GL_COMPILE_STATUS = 0x8B81,
  HD_GL_LINK_STATUS = 0x8B82,
  HD_GL_FRAMEBUFFER = 0x8D40,
  HD_GL_COLOR_ATTACHMENT0 = 0x8CE0,
  HD_GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
  HD_GL_TRIANGLE_STRIP = 0x0005,
  HD_GL_MAX_TEXTURE_SIZE = 0x0D33,
  HD_GL_COLOR_BUFFER_BIT = 0x4000,
  HD_GL_BLEND = 0x0BE2,
  HD_GL_SRC_ALPHA = 0x0302,
  HD_GL_ONE_MINUS_SRC_ALPHA = 0x0303,
  HD_GL_DEPTH_TEST = 0x0B71,
  HD_GL_SCISSOR_TEST = 0x0C11,
  HD_GL_FRAMEBUFFER_BINDING = 0x8CA6,
  HD_GL_NO_ERROR = 0,
};

#define HD_GL_FUNCS(X) \
  X(void, GetIntegerv, (GLenum, GLint *)) \
  X(GLenum, GetError, (void)) \
  X(void, GenTextures, (GLsizei, GLuint *)) \
  X(void, DeleteTextures, (GLsizei, const GLuint *)) \
  X(void, BindTexture, (GLenum, GLuint)) \
  X(void, TexParameteri, (GLenum, GLenum, GLint)) \
  X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, \
                       GLenum, const void *)) \
  X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, \
                          GLenum, GLenum, const void *)) \
  X(void, PixelStorei, (GLenum, GLint)) \
  X(void, ActiveTexture, (GLenum)) \
  X(GLuint, CreateShader, (GLenum)) \
  X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, \
                         const GLint *)) \
  X(void, CompileShader, (GLuint)) \
  X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
  X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
  X(void, DeleteShader, (GLuint)) \
  X(GLuint, CreateProgram, (void)) \
  X(void, AttachShader, (GLuint, GLuint)) \
  X(void, LinkProgram, (GLuint)) \
  X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
  X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
  X(void, DeleteProgram, (GLuint)) \
  X(void, UseProgram, (GLuint)) \
  X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
  X(void, Uniform1i, (GLint, GLint)) \
  X(void, Uniform4i, (GLint, GLint, GLint, GLint, GLint)) \
  X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
  X(void, GenVertexArrays, (GLsizei, GLuint *)) \
  X(void, DeleteVertexArrays, (GLsizei, const GLuint *)) \
  X(void, BindVertexArray, (GLuint)) \
  X(void, GenFramebuffers, (GLsizei, GLuint *)) \
  X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
  X(void, BindFramebuffer, (GLenum, GLuint)) \
  X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
  X(GLenum, CheckFramebufferStatus, (GLenum)) \
  X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
  X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
  X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
  X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
  X(void, Clear, (GLbitfield)) \
  X(void, Disable, (GLenum)) \
  X(void, Enable, (GLenum)) \
  X(void, Finish, (void)) \
  X(void, BlendFunc, (GLenum, GLenum))

#if defined(_WIN32)
#define HD_APIENTRY __stdcall
#else
#define HD_APIENTRY
#endif

#define HD_DECLARE(ret, name, args) \
  typedef ret(HD_APIENTRY *PFN_hd##name) args; \
  static PFN_hd##name gl##name;
HD_GL_FUNCS(HD_DECLARE)
#undef HD_DECLARE

/* kPassNative aliases kPassPresent; every pass before it owns a program. */
enum {
  kPassCompose, kPassDeblock, kPassPresent, kPassOverlay, kPassNative,
  kPassCount
};
enum { kInputSets = 3, kAtlasTilesPerRow = 256 };

typedef struct HdTarget {
  GLuint texture, framebuffer;
  int width, height;
} HdTarget;

static bool s_ready;
static bool s_failed;
static GLuint s_programs[kPassCount];
static GLuint s_vao;
static GLuint s_g[kInputSets], s_lines[kInputSets];
static int s_g_width[kInputSets], s_g_height[kInputSets];
static int s_lines_width[kInputSets], s_lines_height[kInputSets];
static int s_input_set;
static GLuint s_native, s_atlas, s_overlay;
static int s_overlay_width, s_overlay_height;
static int s_native_width, s_native_height;
static uint64_t s_atlas_generation;
static int s_atlas_scale;
static HdTarget s_target, s_deblocked;
static int s_max_texture;

static const char *const kVertex =
    "out vec2 v_uv;\n"
    "uniform int flip_uv;\n"
    "void main() {\n"
    "  vec2 p[4] = vec2[](vec2(-1.0,-1.0), vec2(-1.0,1.0), vec2(1.0,-1.0),\n"
    "                     vec2(1.0,1.0));\n"
    "  vec2 t = p[gl_VertexID] * 0.5 + 0.5;\n"
    "  v_uv = flip_uv != 0 ? vec2(t.x, 1.0 - t.y) : t;\n"
    "  gl_Position = vec4(p[gl_VertexID], 0.0, 1.0);\n"
    "}\n";

/* Compose: one HD subpixel per fragment (see dkc1_hd.c for the encoding
 * and FinishColor for the arithmetic it mirrors). */
static const char *const kCompose =
    "uniform highp usampler2D gtex;\n"
    "uniform highp usampler2D linetex;\n"
    "uniform highp usampler2D atlas;\n"
    "uniform ivec4 dims;  /* log2 scale, native width, native height, 0 */\n"
    "out vec4 color;\n"
    "const uint kNoTile = 0x1ffffu;\n"
    "vec3 rgb15(uint c) {\n"
    "  return vec3(float(c & 31u), float((c >> 5) & 31u), float((c >> 10) & 31u));\n"
    "}\n"
    "vec3 cgram(int y, uint i) {\n"
    "  return rgb15(texelFetch(linetex, ivec2(int(i & 255u), y), 0).r);\n"
    "}\n"
    "uvec2 texel(uint ref, int u, int v) {\n"
    "  int sh = dims.x, s = 1 << sh;\n"
    "  uint t = ref & kNoTile;\n"
    "  int col = int((ref >> 17) & 7u), row = int((ref >> 20) & 7u);\n"
    "  int cu = ((ref >> 23) & 1u) != 0u ? s - 1 - u : u;\n"
    "  int cv = ((ref >> 24) & 1u) != 0u ? s - 1 - v : v;\n"
    "  int tx = (int(t & 255u) << (sh + 3)) + (col << sh) + cu;\n"
    "  int ty = (int(t >> 8) << (sh + 3)) + (row << sh) + cv;\n"
    "  return texelFetch(atlas, ivec2(tx, ty), 0).rg;\n"
    "}\n"
    "uint base_of(uint index, uint ref) {\n"
    "  return index & (((ref >> 25) & 1u) != 0u ? 0xfcu : 0xf0u);\n"
    "}\n"
    "vec3 blend(uvec2 t, uint base, vec3 under, int y) {\n"
    "  uint i = t.r >> 4, j = t.r & 15u;\n"
    "  float w = float(t.g) / 255.0;\n"
    "  vec3 a = i == 0u ? under : cgram(y, base | i);\n"
    "  vec3 b = j == 0u ? under : cgram(y, base | j);\n"
    "  return a + (b - a) * w;\n"
    "}\n"
    "float expand5(float c) {\n"
    "  int lo = int(c), hi = min(lo + 1, 31);\n"
    "  float e0 = float((lo << 3) | (lo >> 2)), e1 = float((hi << 3) | (hi >> 2));\n"
    "  return e0 + (e1 - e0) * (c - float(lo));\n"
    "}\n"
    "vec3 halve(vec3 c) {\n"
    "  vec3 whole = floor(c + 0.5);\n"
    "  return mix(c * 0.5, floor(whole * 0.5),\n"
    "             vec3(lessThan(abs(c - whole), vec3(1e-3))));\n"
    "}\n"
    "void main() {\n"
    "  int sh = dims.x, s = 1 << sh;\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  int nx = p.x >> sh, ny = p.y >> sh, u = p.x & (s - 1), v = p.y & (s - 1);\n"
    "  uvec4 g = texelFetch(gtex, ivec2(nx, ny), 0);\n"
    "  uint mode = (g.z >> 27) & 3u;\n"
    "  if (mode == 1u) {\n"
    "    color = vec4(float((g.w >> 16) & 255u), float((g.w >> 8) & 255u),\n"
    "                 float(g.w & 255u), 255.0) / 255.0;\n"
    "    return;\n"
    "  }\n"
    "  if (mode == 2u) { color = vec4(1.0, 0.0, 1.0, 1.0); return; }\n"
    "  uint flags = ((g.x >> 27) & 31u) | (((g.y >> 27) & 7u) << 5);\n"
    "  uint main_index = g.w & 255u, sub_index = (g.w >> 8) & 255u;\n"
    "  uint under_index = (g.w >> 16) & 255u, cover_base = g.w >> 24;\n"
    "  vec3 main_native = cgram(ny, main_index);\n"
    "  vec3 below = ((g.y >> 30) & 1u) != 0u ? cgram(ny, under_index) : main_native;\n"
    "  vec3 m = (g.x & kNoTile) != kNoTile\n"
    "      ? blend(texel(g.x, u, v), base_of(main_index, g.x), below, ny)\n"
    "      : main_native;\n"
    "  if ((g.y & kNoTile) != kNoTile)\n"
    "    m = blend(texel(g.y, u, v), cover_base, m, ny);\n"
    "  if ((flags & 4u) != 0u) m = vec3(0.0);\n"
    "  if ((flags & 8u) != 0u) {\n"
    "    vec3 sub = (flags & 64u) != 0u\n"
    "        ? rgb15(texelFetch(linetex, ivec2(256, ny), 0).r)\n"
    "        : cgram(ny, sub_index);\n"
    "    if ((g.z & kNoTile) != kNoTile)\n"
    "      sub = blend(texel(g.z, u, v), base_of(sub_index, g.z), sub, ny);\n"
    "    m = (flags & 16u) != 0u ? max(m - sub, vec3(0.0)) : m + sub;\n"
    "    if ((flags & 32u) != 0u) m = halve(m);\n"
    "  }\n"
    "  m = min(m, vec3(31.0));\n"
    "  float bright = float(texelFetch(linetex, ivec2(257, ny), 0).r);\n"
    "  color = vec4(floor(expand5(m.r) * bright / 15.0),\n"
    "               floor(expand5(m.g) * bright / 15.0),\n"
    "               floor(expand5(m.b) * bright / 15.0), 255.0) / 255.0;\n"
    "}\n";

/* Deblock: z bits 26/29/30/31 mark a tile seam above/right/below/left of
 * this native pixel; spread the HD step across it over one native pixel on
 * each side (dkc1_hd.c Ramp and DeblockFrame, in integers). */
static const char *const kDeblock =
    "uniform highp usampler2D gtex;\n"
    "uniform sampler2D srctex;\n"
    "uniform ivec4 dims;  /* log2 scale, native width, native height, 0 */\n"
    "out vec4 color;\n"
    "ivec3 at(int x, int y) {\n"
    "  return ivec3(floor(texelFetch(srctex, ivec2(x, y), 0).rgb * 255.0 + 0.5));\n"
    "}\n"
    "ivec3 ramp(ivec3 d, int i, int n) {\n"
    "  ivec3 m = abs(d) * (n - i) / (2 * n + 1);\n"
    "  return ivec3(d.x < 0 ? -m.x : m.x, d.y < 0 ? -m.y : m.y,\n"
    "               d.z < 0 ? -m.z : m.z);\n"
    "}\n"
    "void main() {\n"
    "  int sh = dims.x, s = 1 << sh;\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  int nx = p.x >> sh, ny = p.y >> sh, u = p.x & (s - 1), v = p.y & (s - 1);\n"
    "  uint z = texelFetch(gtex, ivec2(nx, ny), 0).z;\n"
    "  ivec3 c = at(p.x, p.y);\n"
    "  if ((z & 0xe4000000u) != 0u) {\n"
    "    int x0 = nx << sh, y0 = ny << sh;\n"
    "    if ((z & (1u << 29)) != 0u)\n"
    "      c += ramp(at(x0 + s, p.y) - at(x0 + s - 1, p.y), s - 1 - u, s);\n"
    "    if ((z & (1u << 31)) != 0u)\n"
    "      c -= ramp(at(x0, p.y) - at(x0 - 1, p.y), u, s);\n"
    "    if ((z & (1u << 30)) != 0u)\n"
    "      c += ramp(at(p.x, y0 + s) - at(p.x, y0 + s - 1), s - 1 - v, s);\n"
    "    if ((z & (1u << 26)) != 0u)\n"
    "      c -= ramp(at(p.x, y0) - at(p.x, y0 - 1), v, s);\n"
    "  }\n"
    "  color = vec4(vec3(clamp(c, 0, 255)) / 255.0, 1.0);\n"
    "}\n";

/* Present: the composed frame (or the native frame when `swizzle` is set,
 * whose bytes are B,G,R,x) fitted to the viewport. Same filter as the
 * dkc1_hd Metal/GLSL pass: sharp bilinear up, four taps down. */
static const char *const kPresent =
    "in vec2 v_uv;\n"
    "uniform sampler2D srctex;\n"
    "uniform vec4 sizes;  /* source w, h, viewport w, h */\n"
    "uniform int swizzle;\n"
    "out vec4 color;\n"
    "vec3 fetch(vec2 uv) {\n"
    "  vec4 c = texture(srctex, uv);\n"
    "  return swizzle != 0 ? c.bgr : c.rgb;\n"
    "}\n"
    "void main() {\n"
    "  vec2 size = sizes.xy, ratio = size / sizes.zw;\n"
    "  if (max(ratio.x, ratio.y) > 1.0) {\n"
    "    vec2 o = ratio * 0.25 / size;\n"
    "    color = vec4((fetch(v_uv + vec2(-o.x, -o.y)) + fetch(v_uv + vec2(o.x, -o.y)) +\n"
    "                  fetch(v_uv + vec2(-o.x, o.y)) + fetch(v_uv + vec2(o.x, o.y))) * 0.25,\n"
    "                 1.0);\n"
    "    return;\n"
    "  }\n"
    "  vec2 scale = 1.0 / ratio;\n"
    "  vec2 texel = v_uv * size - 0.5, base = floor(texel), fraction = fract(texel);\n"
    "  vec2 adjusted = clamp((fraction - (0.5 - 0.5 / scale)) * scale, 0.0, 1.0);\n"
    "  color = vec4(fetch((base + adjusted + 0.5) / size), 1.0);\n"
    "}\n";

/* Overlay: 0xAARRGGBB menu/HUD pixels (bytes B,G,R,A), nearest-sampled and
 * alpha-blended over whatever was presented. */
static const char *const kOverlay =
    "in vec2 v_uv;\n"
    "uniform sampler2D srctex;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "  vec4 c = texture(srctex, v_uv);\n"
    "  color = vec4(c.bgr, c.a);\n"
    "}\n";

static bool LoadFunctions(void) {
#define HD_LOAD(ret, name, args) \
  gl##name = (PFN_hd##name)SDL_GL_GetProcAddress("gl" #name); \
  if (!gl##name) { \
    fprintf(stderr, "[hd-gpu] missing gl" #name "\n"); \
    return false; \
  }
  HD_GL_FUNCS(HD_LOAD)
#undef HD_LOAD
  return true;
}

static GLuint Compile(GLenum type, const char *header, const char *a,
                      const char *b) {
  const GLchar *parts[3] = {header, a, b ? b : ""};
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 3, parts, NULL);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, HD_GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(shader, sizeof log, NULL, log);
    fprintf(stderr, "[hd-gpu] shader compile failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint Program(const char *header, const char *fa, const char *fb) {
  GLuint vertex = Compile(HD_GL_VERTEX_SHADER, header, kVertex, NULL);
  GLuint fragment = Compile(HD_GL_FRAGMENT_SHADER, header, fa, fb);
  if (!vertex || !fragment) {
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return 0;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ok = 0;
  glGetProgramiv(program, HD_GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(program, sizeof log, NULL, log);
    fprintf(stderr, "[hd-gpu] program link failed: %s\n", log);
    glDeleteProgram(program);
    return 0;
  }
  glUseProgram(program);
  /* Units: 0-2 compose inputs, 5 the source of present/overlay. */
  static const char *const kSamplers[] = {"gtex", "linetex", "atlas", "",
                                          "", "srctex"};
  for (int i = 0; i < 6; i++)
    if (kSamplers[i][0])
      glUniform1i(glGetUniformLocation(program, kSamplers[i]), i);
  return program;
}

bool Dkc1HdGpuInit(bool gles) {
  if (s_ready)
    return true;
  if (s_failed || !LoadFunctions()) {
    s_failed = true;
    return false;
  }
  const char *header = gles
      ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
        "precision highp usampler2D;\nprecision highp sampler2D;\n"
      : "#version 330 core\n";
  s_programs[kPassCompose] = Program(header, kCompose, NULL);
  s_programs[kPassDeblock] = Program(header, kDeblock, NULL);
  s_programs[kPassPresent] = Program(header, kPresent, NULL);
  s_programs[kPassOverlay] = Program(header, kOverlay, NULL);
  s_programs[kPassNative] = s_programs[kPassPresent];
  for (int i = 0; i < kPassCount; i++) {
    if (!s_programs[i]) {
      s_failed = true;
      return false;
    }
  }
  glGenVertexArrays(1, &s_vao);
  glGetIntegerv(HD_GL_MAX_TEXTURE_SIZE, &s_max_texture);
  s_ready = glGetError() == HD_GL_NO_ERROR;
  s_failed = !s_ready;
  if (s_ready)
    fprintf(stderr, "[hd-gpu] %s compositor ready (max texture %d)\n",
            gles ? "GLES 3" : "GL 3.3", s_max_texture);
  return s_ready;
}

bool Dkc1HdGpuReady(void) {
  return s_ready;
}

static void TextureParameters(bool linear) {
  glTexParameteri(HD_GL_TEXTURE_2D, HD_GL_TEXTURE_MIN_FILTER,
                  linear ? HD_GL_LINEAR : HD_GL_NEAREST);
  glTexParameteri(HD_GL_TEXTURE_2D, HD_GL_TEXTURE_MAG_FILTER,
                  linear ? HD_GL_LINEAR : HD_GL_NEAREST);
  glTexParameteri(HD_GL_TEXTURE_2D, HD_GL_TEXTURE_WRAP_S, HD_GL_CLAMP_TO_EDGE);
  glTexParameteri(HD_GL_TEXTURE_2D, HD_GL_TEXTURE_WRAP_T, HD_GL_CLAMP_TO_EDGE);
}

static void EnsureTexture(GLuint *texture, int *width, int *height, int w,
                          int h, GLenum internal, GLenum format, GLenum type) {
  if (*texture && *width == w && *height == h)
    return;
  if (!*texture)
    glGenTextures(1, texture);
  glBindTexture(HD_GL_TEXTURE_2D, *texture);
  TextureParameters(false);
  glTexImage2D(HD_GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, type,
               NULL);
  *width = w;
  *height = h;
}

static bool EnsureTarget(HdTarget *target, int w, int h) {
  if (target->texture && target->width == w && target->height == h)
    return true;
  if (!target->texture)
    glGenTextures(1, &target->texture);
  glBindTexture(HD_GL_TEXTURE_2D, target->texture);
  TextureParameters(true);
  glTexImage2D(HD_GL_TEXTURE_2D, 0, HD_GL_RGBA8, w, h, 0, HD_GL_RGBA,
               HD_GL_UNSIGNED_BYTE, NULL);
  if (!target->framebuffer)
    glGenFramebuffers(1, &target->framebuffer);
  glBindFramebuffer(HD_GL_FRAMEBUFFER, target->framebuffer);
  glFramebufferTexture2D(HD_GL_FRAMEBUFFER, HD_GL_COLOR_ATTACHMENT0,
                         HD_GL_TEXTURE_2D, target->texture, 0);
  target->width = w;
  target->height = h;
  return glCheckFramebufferStatus(HD_GL_FRAMEBUFFER) ==
         HD_GL_FRAMEBUFFER_COMPLETE;
}

/* Pack tiles in rows of kAtlasTilesPerRow, one RG8UI texture (byte 0 =
 * i << 4 | j, byte 1 = weight; see dkc1_hd.c). */
static bool UploadAtlas(const Dkc1HdGpuInputs *in) {
  if (s_atlas && s_atlas_generation == in->tiles_generation &&
      s_atlas_scale == in->scale)
    return true;
  const int span = 8 * in->scale;
  const int width = kAtlasTilesPerRow * span;
  const int rows =
      (int)((in->tile_count + kAtlasTilesPerRow - 1) / kAtlasTilesPerRow);
  const int height = rows * span;
  if (!in->tile_count || width > s_max_texture || height > s_max_texture) {
    fprintf(stderr, "[hd-gpu] pack does not fit a %d texture\n",
            s_max_texture);
    return false;
  }
  uint16_t *atlas = calloc((size_t)width * height, sizeof *atlas);
  if (!atlas)
    return false;
  for (uint32_t i = 0; i < in->tile_count; i++) {
    const uint16_t *tile = in->tiles + (size_t)i * span * span;
    const size_t x0 = (size_t)(i % kAtlasTilesPerRow) * span;
    const size_t y0 = (size_t)(i / kAtlasTilesPerRow) * span;
    for (int y = 0; y < span; y++)
      memcpy(&atlas[(y0 + y) * width + x0], &tile[y * span],
             span * sizeof *tile);
  }
  if (!s_atlas)
    glGenTextures(1, &s_atlas);
  glActiveTexture(HD_GL_TEXTURE0 + 2);
  glBindTexture(HD_GL_TEXTURE_2D, s_atlas);
  TextureParameters(false);
  glPixelStorei(HD_GL_UNPACK_ALIGNMENT, 2);
  /* uint16 texels in little-endian memory are the bytes (R, G). */
  glTexImage2D(HD_GL_TEXTURE_2D, 0, HD_GL_RG8UI, width, height, 0,
               HD_GL_RG_INTEGER, HD_GL_UNSIGNED_BYTE, atlas);
  free(atlas);
  s_atlas_generation = in->tiles_generation;
  s_atlas_scale = in->scale;
  return glGetError() == HD_GL_NO_ERROR;
}

static void UploadNative(const uint32_t *pixels, int w, int h, size_t pitch) {
  glActiveTexture(HD_GL_TEXTURE0 + 4);
  if (!s_native || s_native_width != w || s_native_height != h) {
    if (!s_native)
      glGenTextures(1, &s_native);
    glBindTexture(HD_GL_TEXTURE_2D, s_native);
    glTexImage2D(HD_GL_TEXTURE_2D, 0, HD_GL_RGBA8, w, h, 0, HD_GL_RGBA,
                 HD_GL_UNSIGNED_BYTE, NULL);
    s_native_width = w;
    s_native_height = h;
  }
  glBindTexture(HD_GL_TEXTURE_2D, s_native);
  TextureParameters(false);
  glPixelStorei(HD_GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(HD_GL_UNPACK_ROW_LENGTH, (GLint)pitch);
  glTexSubImage2D(HD_GL_TEXTURE_2D, 0, 0, 0, w, h, HD_GL_RGBA,
                  HD_GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(HD_GL_UNPACK_ROW_LENGTH, 0);
}

static void Bind(int unit, GLuint texture, bool linear) {
  glActiveTexture(HD_GL_TEXTURE0 + unit);
  glBindTexture(HD_GL_TEXTURE_2D, texture);
  TextureParameters(linear);
}

static void PresentTexture(GLuint texture, int w, int h, bool swizzle,
                           GLuint framebuffer, int vx, int vy, int vw,
                           int vh) {
  glBindFramebuffer(HD_GL_FRAMEBUFFER, framebuffer);
  glViewport(vx, vy, vw, vh);
  glUseProgram(s_programs[kPassPresent]);
  glUniform1i(glGetUniformLocation(s_programs[kPassPresent], "flip_uv"), 1);
  glUniform1i(glGetUniformLocation(s_programs[kPassPresent], "swizzle"),
              swizzle ? 1 : 0);
  glUniform4f(glGetUniformLocation(s_programs[kPassPresent], "sizes"),
              (float)w, (float)h, (float)vw, (float)vh);
  Bind(5, texture, true);
  glDrawArrays(HD_GL_TRIANGLE_STRIP, 0, 4);
}

static const HdTarget *s_last;

static int Log2Scale(int scale) {
  return scale >= 4 ? 2 : scale >= 2 ? 1 : 0;
}

bool Dkc1HdGpuCompose(const Dkc1HdGpuInputs *in) {
  if (!s_ready || !in || (in->scale != 1 && in->scale != 2 && in->scale != 4))
    return false;
  glDisable(HD_GL_BLEND);
  glDisable(HD_GL_DEPTH_TEST);
  glDisable(HD_GL_SCISSOR_TEST);
  glBindVertexArray(s_vao);
  if (!UploadAtlas(in))
    return false;
  /* This frame's inputs go to the next texture set; the GPU may still be
   * reading the previous frame's, and uploading over those would stall. */
  const int set = s_input_set;
  s_input_set = (s_input_set + 1) % kInputSets;
  glActiveTexture(HD_GL_TEXTURE0 + 0);
  EnsureTexture(&s_g[set], &s_g_width[set], &s_g_height[set], in->width,
                in->height, HD_GL_RGBA32UI, HD_GL_RGBA_INTEGER,
                HD_GL_UNSIGNED_INT);
  glBindTexture(HD_GL_TEXTURE_2D, s_g[set]);
  glPixelStorei(HD_GL_UNPACK_ALIGNMENT, 4);
  glTexSubImage2D(HD_GL_TEXTURE_2D, 0, 0, 0, in->width, in->height,
                  HD_GL_RGBA_INTEGER, HD_GL_UNSIGNED_INT, in->g);
  glActiveTexture(HD_GL_TEXTURE0 + 1);
  EnsureTexture(&s_lines[set], &s_lines_width[set], &s_lines_height[set],
                kDkc1HdGpuLineStride, in->height, HD_GL_R16UI,
                HD_GL_RED_INTEGER, HD_GL_UNSIGNED_SHORT);
  glBindTexture(HD_GL_TEXTURE_2D, s_lines[set]);
  glPixelStorei(HD_GL_UNPACK_ALIGNMENT, 2);
  glTexSubImage2D(HD_GL_TEXTURE_2D, 0, 0, 0, kDkc1HdGpuLineStride, in->height,
                  HD_GL_RED_INTEGER, HD_GL_UNSIGNED_SHORT, in->lines);

  /* Target first: creating it binds it on the active unit, which must not
   * displace an input bound below. */
  if (!EnsureTarget(&s_target, in->width * in->scale, in->height * in->scale))
    return false;
  Bind(0, s_g[set], false);
  Bind(1, s_lines[set], false);
  Bind(2, s_atlas, false);
  glBindFramebuffer(HD_GL_FRAMEBUFFER, s_target.framebuffer);
  glViewport(0, 0, s_target.width, s_target.height);
  const GLuint program = s_programs[kPassCompose];
  glUseProgram(program);
  glUniform4i(glGetUniformLocation(program, "dims"), Log2Scale(in->scale),
              in->width, in->height, 0);
  glUniform1i(glGetUniformLocation(program, "flip_uv"), 0);
  glDrawArrays(HD_GL_TRIANGLE_STRIP, 0, 4);
  s_last = &s_target;
  if (in->deblock) {
    if (!EnsureTarget(&s_deblocked, s_target.width, s_target.height))
      return false;
    Bind(0, s_g[set], false);
    Bind(5, s_target.texture, false);
    glBindFramebuffer(HD_GL_FRAMEBUFFER, s_deblocked.framebuffer);
    glViewport(0, 0, s_deblocked.width, s_deblocked.height);
    const GLuint deblock = s_programs[kPassDeblock];
    glUseProgram(deblock);
    glUniform4i(glGetUniformLocation(deblock, "dims"), Log2Scale(in->scale),
                in->width, in->height, 0);
    glUniform1i(glGetUniformLocation(deblock, "flip_uv"), 0);
    glDrawArrays(HD_GL_TRIANGLE_STRIP, 0, 4);
    s_last = &s_deblocked;
  }
  return glGetError() == HD_GL_NO_ERROR;
}

void Dkc1HdGpuPresent(unsigned framebuffer, int vx, int vy, int vw, int vh) {
  if (!s_ready || !s_last)
    return;
  PresentTexture(s_last->texture, s_last->width, s_last->height, false,
                 framebuffer, vx, vy, vw, vh);
}

void Dkc1HdGpuPresentNative(const uint32_t *pixels, int w, int h,
                            size_t pitch, unsigned framebuffer, int vx,
                            int vy, int vw, int vh) {
  if (!s_ready || !pixels)
    return;
  glBindVertexArray(s_vao);
  UploadNative(pixels, w, h, pitch);
  PresentTexture(s_native, w, h, true, framebuffer, vx, vy, vw, vh);
}

void Dkc1HdGpuPresentOverlay(const uint32_t *pixels, int w, int h,
                             size_t pitch, unsigned framebuffer, int vx,
                             int vy, int vw, int vh) {
  if (!s_ready || !pixels)
    return;
  glBindVertexArray(s_vao);
  glActiveTexture(HD_GL_TEXTURE0 + 5);
  if (!s_overlay || s_overlay_width != w || s_overlay_height != h) {
    if (!s_overlay)
      glGenTextures(1, &s_overlay);
    glBindTexture(HD_GL_TEXTURE_2D, s_overlay);
    glTexImage2D(HD_GL_TEXTURE_2D, 0, HD_GL_RGBA8, w, h, 0, HD_GL_RGBA,
                 HD_GL_UNSIGNED_BYTE, NULL);
    s_overlay_width = w;
    s_overlay_height = h;
  }
  glBindTexture(HD_GL_TEXTURE_2D, s_overlay);
  TextureParameters(false);
  glPixelStorei(HD_GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(HD_GL_UNPACK_ROW_LENGTH, (GLint)pitch);
  glTexSubImage2D(HD_GL_TEXTURE_2D, 0, 0, 0, w, h, HD_GL_RGBA,
                  HD_GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(HD_GL_UNPACK_ROW_LENGTH, 0);
  glBindFramebuffer(HD_GL_FRAMEBUFFER, framebuffer);
  glViewport(vx, vy, vw, vh);
  glUseProgram(s_programs[kPassOverlay]);
  glUniform1i(glGetUniformLocation(s_programs[kPassOverlay], "flip_uv"), 1);
  glEnable(HD_GL_BLEND);
  glBlendFunc(HD_GL_SRC_ALPHA, HD_GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(HD_GL_TRIANGLE_STRIP, 0, 4);
  glDisable(HD_GL_BLEND);
}

void Dkc1HdGpuClearScreen(unsigned framebuffer, int width, int height) {
  if (!s_ready)
    return;
  glBindFramebuffer(HD_GL_FRAMEBUFFER, framebuffer);
  glViewport(0, 0, width, height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(HD_GL_COLOR_BUFFER_BIT);
}

void Dkc1HdGpuFinish(void) {
  if (s_ready)
    glFinish();
}

bool Dkc1HdGpuReadComposite(uint32_t *out, int *width, int *height) {
  if (!s_ready || !s_last || !out)
    return false;
  const int w = s_last->width, h = s_last->height;
  uint8_t *rgba = malloc((size_t)w * h * 4);
  if (!rgba)
    return false;
  glBindFramebuffer(HD_GL_FRAMEBUFFER, s_last->framebuffer);
  glPixelStorei(HD_GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, w, h, HD_GL_RGBA, HD_GL_UNSIGNED_BYTE, rgba);
  /* Row r of the targets is image row r (compose writes top-first rows
   * from gl_FragCoord.y), which is also glReadPixels' row order. */
  for (size_t i = 0; i < (size_t)w * h; i++)
    out[i] = (uint32_t)rgba[i * 4] << 16 | (uint32_t)rgba[i * 4 + 1] << 8 |
             rgba[i * 4 + 2];
  free(rgba);
  if (width) *width = w;
  if (height) *height = h;
  return true;
}

void Dkc1HdGpuShutdown(void) {
  if (!s_ready)
    return;
  glDeleteFramebuffers(1, &s_target.framebuffer);
  glDeleteTextures(1, &s_target.texture);
  memset(&s_target, 0, sizeof s_target);
  glDeleteFramebuffers(1, &s_deblocked.framebuffer);
  glDeleteTextures(1, &s_deblocked.texture);
  memset(&s_deblocked, 0, sizeof s_deblocked);
  glDeleteTextures(kInputSets, s_g);
  glDeleteTextures(kInputSets, s_lines);
  memset(s_g, 0, sizeof s_g);
  memset(s_lines, 0, sizeof s_lines);
  memset(s_g_width, 0, sizeof s_g_width);
  memset(s_g_height, 0, sizeof s_g_height);
  memset(s_lines_width, 0, sizeof s_lines_width);
  memset(s_lines_height, 0, sizeof s_lines_height);
  GLuint textures[] = {s_native, s_atlas, s_overlay};
  glDeleteTextures(3, textures);
  s_native = s_atlas = s_overlay = 0;
  s_native_width = s_native_height = 0;
  s_overlay_width = s_overlay_height = 0;
  s_atlas_generation = 0;
  for (int i = 0; i < kPassNative; i++)
    glDeleteProgram(s_programs[i]);
  memset(s_programs, 0, sizeof s_programs);
  glDeleteVertexArrays(1, &s_vao);
  s_vao = 0;
  s_last = NULL;
  s_ready = false;
}
