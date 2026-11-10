/* ps3_rsx.h -- the drawing backend: triangles in clip space, with the N64
 * colour combiner evaluated per pixel by one fragment program.
 *
 * Plain C, no PSL1GHT and no N64 headers: the display list interpreter
 * (source/game/ps3_gfx.c) is compiled against ultra64.h and the backend
 * (source/ps3_rsx.c) against PSL1GHT, and both include this. The host test
 * (hosttest/) implements the same interface with a software rasterizer.
 *
 * The screen is the N64's: 320x240, 4:3. The backend scales it to the output
 * resolution.
 */

#ifndef PS3_RSX_H
#define PS3_RSX_H

#include <stdint.h>

#define PS3_SCREEN_W 320
#define PS3_SCREEN_H 240

/* The N64 combiner computes (A - B) * C + D, separately for colour and for
 * alpha. Each input is either the texel or something constant over the pixel
 * (primitive/environment colour, the vertex shade, 0, 1...). Per vertex:
 *
 *   c[0..3]   the constant part of A, B, C, D (r, g, b, a; 0 when the input
 *             is the texel)
 *   wrgb[2k], wrgb[2k+1]   255 to add texel.rgb / texel.aaa to the colour of
 *             input k (A, B, C, D), else 0
 *   wa[k]     255 to add texel.a to the alpha of input k, else 0
 *   fog       fog colour and, in fog[3], how much of it (0 = no fog)
 *
 * 56 bytes; the vertex program expects exactly this layout. */
typedef struct {
    float   x, y, z, w;        /* clip space: x right, y up, z -w..w */
    float   u, v;              /* 0..1 over the texture */
    uint8_t c[4][4];
    uint8_t wrgb[8];
    uint8_t wa[4];
    uint8_t fog[4];
} ps3_rvert_t;

/* A texture already uploaded (A8R8G8B8). Opaque. */
typedef struct ps3_rtex ps3_rtex_t;

enum { PS3_WRAP_REPEAT = 0, PS3_WRAP_CLAMP = 1, PS3_WRAP_MIRROR = 2 };

typedef struct {
    ps3_rtex_t *tex;           /* NULL = no texture (the weights are all 0) */
    uint8_t filter;            /* 0 = nearest, 1 = bilinear */
    uint8_t wrap_s, wrap_t;    /* PS3_WRAP_* */
    uint8_t blend;             /* 0 = opaque, 1 = src*a + dst*(1-a) */
    uint8_t depth_test;        /* 0/1, "nearer or equal" */
    uint8_t depth_write;       /* 0/1 */
    uint8_t decal;             /* 1 = pull towards the camera (decal surfaces) */
    uint8_t alpha_ref;         /* 0 = no alpha test, else discard alpha < ref */
    int16_t sx, sy, sw, sh;    /* scissor, in N64 pixels */
    uint8_t wide;              /* 16:9: uses the whole width (3D), not the 4:3 middle (2D) */
    int8_t anchor;             /* 16:9, 2D: -1 against the left edge, 1 the right, 0 the middle */
    uint8_t fullsc;            /* 16:9, 2D: the scissor is the whole screen, side bars included */
} ps3_rstate_t;

/* 0 = ok. After the video is up. */
int  PS3_Rsx_Init(void);
void PS3_Rsx_Shutdown(void);

/* One frame: BeginFrame clears to black, EndFrame shows it. */
void PS3_Rsx_BeginFrame(void);
/* 16:9: the 3D uses the whole screen and the 2D the 4:3 strip in the middle
 * (the interpreter scales 2D x by 3/4). 0: everything in the 4:3 area. */
void PS3_Rsx_SetWide(int on);
/* Screen fit (overscan): the picture is drawn in this percentage of the
 * screen, centred (100 = all of it). */
void PS3_Rsx_SetScreenFit(int percent);
void PS3_Rsx_EndFrame(void);

/* w*h pixels 0xAARRGGBB. NULL if out of memory. */
ps3_rtex_t *PS3_Rsx_TexCreate(int w, int h, const uint32_t *argb);
/* Freed once the RSX can no longer be reading it (a few frames later). */
void PS3_Rsx_TexRelease(ps3_rtex_t *t);

/* n vertices (a multiple of 3), loose triangles, one state. */
void PS3_Rsx_Draw(const ps3_rstate_t *st, const ps3_rvert_t *v, int n);

/* Clears the whole depth buffer. */
void PS3_Rsx_ClearDepth(void);

/* Diagnostics (R3 cycles the mode, L3 logs every draw of the next frame).
 * Each mode switches one part of the pipeline off, to find which one hides
 * something on the console. */
enum {
    PS3_RDBG_NORMAL = 0,
    PS3_RDBG_NO_ALPHA_TEST,
    PS3_RDBG_NO_DEPTH,
    PS3_RDBG_NO_BLEND,
    PS3_RDBG_NO_TEXTURE,        /* texture replaced by white */
    PS3_RDBG_NO_SCISSOR,
    PS3_RDBG_NO_CULL,           /* handled by the interpreter */
    PS3_RDBG_SHADER_A,          /* the first vertex layout and fragment program */
    PS3_RDBG_SHOW_ALPHA,        /* the combined alpha as grey */
    PS3_RDBG_COUNT
};
void PS3_Rsx_SetDebug(int mode);
int  PS3_Rsx_GetDebug(void);
const char *PS3_Rsx_DebugName(int mode);
void PS3_Rsx_LogNextFrame(void);

#endif /* PS3_RSX_H */
