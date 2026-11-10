/* ps3_gfx.c -- the display list interpreter: what the RSP microcode (F3DEX)
 * and the RDP do on the N64, up to triangles in clip space for ps3_rsx.h.
 *
 *   RSP side: matrix stack, vertex transform, lighting, texture coordinate
 *             generation, fog, culling, viewport.
 *   RDP side: tiles and texture loads (converted to A8R8G8B8 and cached),
 *             colour combiner (turned into per-vertex inputs of the one
 *             fragment program), render mode (blend, depth, alpha test),
 *             rectangles, scissor.
 *
 * The method (one pass, vertices lit and projected on the CPU, combiner
 * inputs resolved per vertex) is the usual one of the PC ports of N64 games.
 * The commands are decoded for the old F3DEX of Mario Kart 64 (F3D_OLD).
 */

#include <ultra64.h>
#include <PR/gbi.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "main.h"
#include "defines.h"
#include "code_800029B0.h"
#include <stddef.h>
#include "ps3_game.h"
#include "ps3_gfx_walk.h"
#include "ps3_rsx.h"
#include "ps3_plat.h"

ps3_gfx_stats_t gPs3GfxStats;

#define MAX_DL_DEPTH    18
#define MAX_COMMANDS    400000            /* a runaway list stops here */
#define MAX_ERRORS_LOGGED 40
#define MAX_VERTS       64
#define MAX_LIGHTS      8
#define BATCH_VERTS     3072

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    float x, y, z, w;                     /* clip space, before the viewport */
    float u, v;                           /* texels * 32 (s10.5) */
    u8 r, g, b, a;                        /* shade; a is the fog amount with G_FOG */
    u8 clip;
    u8 persp;                             /* loaded under a perspective projection */
    u8 wide;                              /* 16:9: its view goes across the whole width */
} vert_t;

typedef struct {
    u8 fmt, siz, palette, cms, cmt, masks, maskt, shifts, shiftt;
    u16 line, tmem;
    u16 uls, ult, lrs, lrt;               /* 10.2 */
} tile_t;

/* What a load command left at a TMEM address. */
typedef struct {
    const u8* addr;                       /* NULL = nothing loaded */
    u32 bytes;                            /* LOADBLOCK: bytes loaded */
    u8 isTile;                            /* LOADTILE: a rectangle of the image */
    u8 siz;                               /* texel size of the image it came from */
    u16 imgWidth;                         /* LOADTILE: width of the image, texels */
    u16 uls, ult, lrs, lrt;               /* LOADTILE: the rectangle, texels */
} load_t;

static struct {
    float mv[11][4][4];
    int mvTop;
    float proj[4][4];
    float mp[4][4];
    int mpDirty;

    vert_t v[MAX_VERTS];

    u32 geometryMode;
    u16 texScaleS, texScaleT;
    u8 texTile, texOn;
    s16 fogMul, fogOffset;

    int numLights;                        /* directional; the ambient one follows */
    Light_t lights[MAX_LIGHTS + 1];
    float lightDir[MAX_LIGHTS][3];        /* in model space */
    Light_t lookat[2];
    float lookatDir[2][3];
    int lightsDirty;

    float vpScaleX, vpScaleY, vpTransX, vpTransY;   /* pixels */
    uintptr_t segment[16];
} rsp;

static struct {
    tile_t tile[8];
    load_t load[512];
    const u8* palette[16];                /* per 16-entry slot of the upper TMEM half */

    const u8* timgAddr;
    u8 timgFmt, timgSiz;
    u16 timgWidth;

    u32 otherModeH, otherModeL;
    u32 combineHi, combineLo;
    u8 env[4], prim[4], blend[4], fog[4], fill[4];
    u8 primLodFrac;

    s16 scX, scY, scW, scH;
    uintptr_t colorImage, depthImage;
} rdp;

static u32 sFrame;
static int sFrameOpen;
int gPs3Wide;                             /* 16:9 (the option) */
static int sWide;                         /* 16:9 for this frame */
static int sWideRace;                     /* 16:9 and a 1P race going on */
static int sHudAnchor;                    /* 16:9: PS3_HUD_LEFT/MIDDLE/RIGHT, from the game's marks */

/* ---- 60 fps: the in-between frame ----------------------------------------
 * The game runs at 30 fps. In 60 fps mode its display list is drawn twice:
 * first with every matrix half-way between the previous frame's and this
 * one's, then as it is.
 * A matrix is paired with the one the previous frame had in the same place
 * of the gfx pool (there are two pools, used in turn, laid out the same):
 * karts and shadows have a place per player and screen, the camera one per
 * screen. Objects and effects take places in the order they are drawn, so
 * those pairs are checked: too far apart and the matrix is drawn as it is.
 * Matrices outside the pools are the game's constant data. */
#define POOL_MTX_END ((u32) offsetof(struct GfxPool, gfxPool))   /* the matrices come first */
#define POOL_SLOTS (POOL_MTX_END / sizeof(Mtx))
static float sPrevMtx[POOL_SLOTS][4][4];
static u32 sPrevStamp[POOL_SLOTS];           /* 1 + the real frame that loaded it */
static u32 sRealFrames;
static int sInterp;                   /* this pass draws the in-between frame */
static float sInterpT;
static const Gfx* sPendingDl;         /* 60 fps: the frame waiting to be drawn */
int gPs3Fps60;
ps3_interp_stats_t gPs3InterpStats;
ps3_prof_t gPs3Prof;
static int sErrorsLogged;

/* ------------------------------------------------------------------ */
/* Addresses                                                           */
/* ------------------------------------------------------------------ */

/* A real address on the PS3: not in the range reserved for segmented
 * addresses, and not in the first 64 KB (nothing is mapped there). */
static int plausible(uintptr_t a) {
    return a >= 0x10000 && (a < 0x01000000 || a >= 0x10000000) && a < 0xE0000000UL;
}

static uintptr_t resolve(u32 w1) {
    u32 seg = w1 >> 24;

    if (seg >= 1 && seg <= 15) {
        return rsp.segment[seg] + (w1 & 0x00FFFFFF);
    }
    return (uintptr_t) w1;
}

static void bad(const char* what, u32 w0, u32 w1) {
    gPs3GfxStats.errors++;
    if (sErrorsLogged < MAX_ERRORS_LOGGED) {
        sErrorsLogged++;
        ps3_logf("[gfx] frame %u: %s: w0 %08X w1 %08X -> 0x%lX", sFrame, what, w0, w1, (unsigned long) resolve(w1));
    }
}

/* ------------------------------------------------------------------ */
/* Texture cache                                                       */
/* ------------------------------------------------------------------ */

#define TEX_MAX      2048
#define TEX_HASH     8192                 /* > 2 * TEX_MAX, power of two */
#define TEX_MAX_DIM  512
#define TEX_KEEP_FRAMES 300

typedef struct {
    u32 key;                              /* hash of the parameters and the contents */
    u16 w, h;
    u32 lastUsed;
    ps3_rtex_t* tex;
} texent_t;

static texent_t sTex[TEX_MAX];
static int sTexCount;
static s16 sTexIndex[TEX_HASH];           /* -1 = empty */
static u32 sConv[TEX_MAX_DIM * TEX_MAX_DIM];

/* The texture a tile resolves to stays the same until a load or a tile
 * command: sTexGen counts those. */
static u32 sTexGen;
static struct {
    u32 gen;
    texent_t* ent;
} sTileTex[8];

static u32 fnv(u32 h, const u8* p, u32 n) {
    while (n--) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

/* The contents of a texture: eight bytes per step (a byte-wise FNV was the
 * biggest cost of the interpreter on the PPU). */
static u32 hash_data(u32 h, const u8* p, u32 n) {
    u64 x = h;

    while (n >= 8) {
        u64 w;

        memcpy(&w, p, 8);
        x = (x ^ w) * 0x100000001B3ULL;
        x ^= x >> 29;
        p += 8;
        n -= 8;
    }
    while (n--) {
        x = (x ^ *p++) * 0x100000001B3ULL;
    }
    return (u32) (x ^ (x >> 32));
}

/* Keys already worked out in this game frame. The game does not run while
 * its display list is drawn, so a texture at the same place with the same
 * parameters has the same contents until the next frame: the smoke loads
 * its texture again for every puff, and in 60 fps mode the list is drawn
 * twice. */
#define MEMO_SIZE 1024
typedef struct {
    const u8* src;
    const u8* pal;
    u32 bytes, params, params2, stamp, key;
} memo_t;
static memo_t sMemo[MEMO_SIZE];

static void tex_index_rebuild(void) {
    int i;

    memset(sTexIndex, 0xFF, sizeof(sTexIndex));
    for (i = 0; i < sTexCount; i++) {
        u32 slot = sTex[i].key & (TEX_HASH - 1);

        while (sTexIndex[slot] >= 0) {
            slot = (slot + 1) & (TEX_HASH - 1);
        }
        sTexIndex[slot] = (s16) i;
    }
}

/* Drops what has not been used for a while. Called between frames. */
static void tex_evict(int all) {
    int i, kept = 0;

    for (i = 0; i < sTexCount; i++) {
        if (all || sFrame - sTex[i].lastUsed > TEX_KEEP_FRAMES) {
            PS3_Rsx_TexRelease(sTex[i].tex);
        } else {
            sTex[kept++] = sTex[i];
        }
    }
    if (kept != sTexCount) {
        sTexCount = kept;
        tex_index_rebuild();
    }
    sTexGen++;                            /* cached pointers into sTex[] are stale */
}

static u32 rgba16_to_argb(u32 c) {
    u32 r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;

    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return ((c & 1) ? 0xFF000000u : 0) | (r << 16) | (g << 8) | b;
}

static u32 ia16_to_argb(u32 c) {
    u32 i = c >> 8;

    return ((c & 0xFF) << 24) | (i << 16) | (i << 8) | i;
}

/* Converts w*h texels starting at texel x0 of rows `stride` bytes apart. */
static void tex_convert(const u8* src, u32 stride, u32 x0, int w, int h, int fmt, int siz, const u8* pal,
                        int palIsIA) {
    int x, y;
    u32* dst = sConv;

    for (y = 0; y < h; y++, src += stride) {
        for (x = 0; x < w; x++) {
            u32 n = x0 + (u32) x, c, i, a;

            switch (siz) {
                case G_IM_SIZ_4b:
                    c = (src[n >> 1] >> ((n & 1) ? 0 : 4)) & 15;
                    break;
                case G_IM_SIZ_8b:
                    c = src[n];
                    break;
                case G_IM_SIZ_16b:
                    c = ((u32) src[n * 2] << 8) | src[n * 2 + 1];
                    break;
                default:
                    c = ((u32) src[n * 4] << 24) | ((u32) src[n * 4 + 1] << 16) | ((u32) src[n * 4 + 2] << 8) |
                        src[n * 4 + 3];
                    break;
            }

            if (fmt == G_IM_FMT_CI && pal != NULL) {
                c = ((u32) pal[c * 2] << 8) | pal[c * 2 + 1];
                *dst++ = palIsIA ? ia16_to_argb(c) : rgba16_to_argb(c);
            } else if (siz == G_IM_SIZ_32b) {
                *dst++ = (c >> 8) | (c << 24);        /* RGBA -> ARGB */
            } else if (fmt == G_IM_FMT_RGBA) {
                *dst++ = rgba16_to_argb(c);
            } else if (fmt == G_IM_FMT_IA) {
                if (siz == G_IM_SIZ_4b) {
                    i = (c >> 1) * 255 / 7;
                    a = (c & 1) ? 255 : 0;
                } else if (siz == G_IM_SIZ_8b) {
                    i = (c >> 4) * 17;
                    a = (c & 15) * 17;
                } else {
                    i = c >> 8;
                    a = c & 0xFF;
                }
                *dst++ = (a << 24) | (i << 16) | (i << 8) | i;
            } else {                                  /* I (and CI with no palette) */
                i = (siz == G_IM_SIZ_4b) ? c * 17 : (c & 0xFF);
                *dst++ = (i << 24) | (i << 16) | (i << 8) | i;
            }
        }
    }
}

/* The texture a render tile shows, converted and uploaded. NULL if nothing
 * usable is loaded for it. */
static texent_t* tex_for_tile(int t) {
    const tile_t* tile = &rdp.tile[t];
    const load_t* ld = &rdp.load[tile->tmem & 511];
    const u8* src;
    const u8* pal = NULL;
    u32 stride, x0 = 0, bytes, key, slot, palCount = 0;
    int w, h, bpp, palIsIA;
    texent_t* e;
    u64 t0;

    if (sTileTex[t].gen == sTexGen) {
        return sTileTex[t].ent;
    }
    sTileTex[t].gen = sTexGen;
    sTileTex[t].ent = NULL;

    if (ld->addr == NULL) {
        return NULL;
    }
    bpp = 4 << tile->siz;
    if (ld->isTile) {
        u32 imgBpp = 4u << ld->siz;

        /* The load counts texels of its own size, which can differ from the
         * render tile's: gDPLoadTextureTile_4b loads 4-bit textures as 8-bit,
         * two texels per load texel. Convert to render texels. */
        w = (int) (((u32) (ld->lrs - ld->uls + 1) * imgBpp) / (u32) bpp);
        h = ld->lrt - ld->ult + 1;
        stride = ((u32) ld->imgWidth * imgBpp) >> 3;
        src = ld->addr + (u32) ld->ult * stride;
        x0 = ((u32) ld->uls * imgBpp) / (u32) bpp;
    } else {
        u32 lineBytes = (u32) tile->line * 8;

        if (lineBytes == 0) {
            return NULL;
        }
        if (tile->siz == G_IM_SIZ_32b) {
            w = (int) (lineBytes / 2);            /* TMEM holds RGBA32 in two halves */
            stride = (u32) w * 4;
        } else {
            w = (int) (lineBytes * 8 / (u32) bpp);
            stride = lineBytes;
        }
        h = (int) (ld->bytes / stride);
        /* The tile can be smaller than what was loaded. */
        if (tile->lrt >= tile->ult && (int) ((tile->lrt >> 2) + 1) < h && tile->ult == 0) {
            int th = (tile->lrt >> 2) + 1;

            if (tile->maskt != 0 && th < (1 << tile->maskt)) {
                th = 1 << tile->maskt;
            }
            if (th < h) {
                h = th;
            }
        }
        src = ld->addr;
    }
    if (w <= 0 || h <= 0 || w > TEX_MAX_DIM || h > TEX_MAX_DIM) {
        return NULL;
    }

    palIsIA = ((rdp.otherModeH >> G_MDSFT_TEXTLUT) & 3) == 3;
    if (tile->fmt == G_IM_FMT_CI) {
        if (tile->siz == G_IM_SIZ_4b) {
            pal = rdp.palette[tile->palette & 15];
            palCount = 16;
        } else {
            pal = rdp.palette[0];
            palCount = 256;
        }
    }

    /* Key: the parameters and the contents. Games rewrite textures in place
     * (animations, the kart sprites), so the address alone is not enough. */
    bytes = (stride * (u32) (h - 1)) + ((x0 + (u32) w) * (u32) bpp + 7) / 8;
    t0 = ps3_tb();
    gPs3Prof.texLookups++;
    {
        u32 params = (u32) ((w << 20) | (h << 8) | (tile->fmt << 4) | tile->siz);
        u32 params2 = x0 | ((u32) palIsIA << 31) | (palCount << 16);
        u32 m = (u32) ((((uintptr_t) src >> 3) ^ params ^ (params2 * 31u) ^ ((uintptr_t) pal >> 2)) & (MEMO_SIZE - 1));
        memo_t* me = &sMemo[m];

        if (me->stamp == sRealFrames + 1 && me->src == src && me->pal == pal && me->bytes == bytes &&
            me->params == params && me->params2 == params2) {
            key = me->key;
        } else {
            gPs3Prof.texBytes += bytes;
            key = fnv(2166136261u, (const u8*) &ld->addr, sizeof(ld->addr));
            key = (key ^ params) * 16777619u;
            key = (key ^ (x0 | ((u32) palIsIA << 31))) * 16777619u;
            key = hash_data(key, src, bytes);
            if (pal != NULL) {
                key = hash_data(key, pal, palCount * 2);
            }
            me->src = src;
            me->pal = pal;
            me->bytes = bytes;
            me->params = params;
            me->params2 = params2;
            me->stamp = sRealFrames + 1;
            me->key = key;
        }
    }

    slot = key & (TEX_HASH - 1);
    while (sTexIndex[slot] >= 0) {
        e = &sTex[sTexIndex[slot]];
        if (e->key == key && e->w == w && e->h == h) {
            e->lastUsed = sFrame;
            sTileTex[t].ent = e;
            gPs3Prof.t[PROF_TEX_HASH] += ps3_tb() - t0;
            return e;
        }
        slot = (slot + 1) & (TEX_HASH - 1);
    }
    gPs3Prof.t[PROF_TEX_HASH] += ps3_tb() - t0;
    t0 = ps3_tb();
    gPs3Prof.texNew++;

    if (sTexCount >= TEX_MAX) {
        return NULL;                      /* evicted between frames */
    }
    tex_convert(src, stride, x0, w, h, tile->fmt, tile->siz, pal, palIsIA);
    e = &sTex[sTexCount];
    e->tex = PS3_Rsx_TexCreate(w, h, sConv);
    if (e->tex == NULL) {
        return NULL;
    }
    e->key = key;
    e->w = (u16) w;
    e->h = (u16) h;
    e->lastUsed = sFrame;
    sTexIndex[slot] = (s16) sTexCount++;
    gPs3GfxStats.textures++;
    sTileTex[t].ent = e;
    gPs3Prof.t[PROF_TEX_NEW] += ps3_tb() - t0;
    return e;
}

/* The combiner wants a texel but the tile resolves to nothing. */
static void tex_missing(int t) {
    static int logged;
    const tile_t* tile = &rdp.tile[t];
    const load_t* ld = &rdp.load[tile->tmem & 511];

    gPs3GfxStats.texMissing++;
    if (logged < 30) {
        logged++;
        ps3_logf("[gfx] frame %u: no texture for tile %d: fmt %d siz %d line %d tmem %d size %d,%d-%d,%d | "
                 "load %p %s bytes %u img w %u rect %u,%u-%u,%u",
                 sFrame, t, tile->fmt, tile->siz, tile->line, tile->tmem, tile->uls, tile->ult, tile->lrs, tile->lrt,
                 (const void*) ld->addr, ld->isTile ? "tile" : "block", ld->bytes, ld->imgWidth, ld->uls, ld->ult,
                 ld->lrs, ld->lrt);
    }
}

/* ------------------------------------------------------------------ */
/* Batch                                                               */
/* ------------------------------------------------------------------ */

static ps3_rvert_t sBatch[BATCH_VERTS];
static int sBatchCount;
static ps3_rstate_t sBatchState;

static void flush(void) {
    if (sBatchCount > 0) {
        u64 t0 = ps3_tb();

        PS3_Rsx_Draw(&sBatchState, sBatch, sBatchCount);
        gPs3Prof.draws++;
        gPs3Prof.t[PROF_DRAW] += ps3_tb() - t0;
        sBatchCount = 0;
    }
}

/* Room for n more vertices with this state. */
static ps3_rvert_t* batch(const ps3_rstate_t* st, int n) {
    ps3_rvert_t* out;

    if (sBatchCount > 0 && (sBatchCount + n > BATCH_VERTS || memcmp(st, &sBatchState, sizeof(*st)) != 0)) {
        flush();
    }
    if (sBatchCount == 0) {
        sBatchState = *st;
    }
    out = &sBatch[sBatchCount];
    sBatchCount += n;
    return out;
}

/* ------------------------------------------------------------------ */
/* Combiner                                                            */
/* ------------------------------------------------------------------ */

enum { K_CONST, K_SHADE, K_SHADE_A, K_TEXEL, K_TEXEL_A };

/* How to fill ps3_rvert_t for the current combine mode and colours. */
typedef struct {
    u8 rgbKind[4], aKind[4];
    u8 rgb[4][3], a[4];
    u8 usesTexture;
} plan_t;

static plan_t sPlan;
static int sPlanDirty = 1;

static void plan_rgb(plan_t* p, int k, int src) {
    const u8* col = NULL;
    u8 grey = 0;

    /* src: the encoding of the A input; the others are mapped onto it. */
    switch (src) {
        case 1: case 2: p->rgbKind[k] = K_TEXEL; p->usesTexture = 1; return;
        case 3: col = rdp.prim; break;
        case 4: p->rgbKind[k] = K_SHADE; return;
        case 5: col = rdp.env; break;
        case 6: grey = 255; break;                    /* 1 */
        case 8: case 9: p->rgbKind[k] = K_TEXEL_A; p->usesTexture = 1; return;
        case 10: grey = rdp.prim[3]; break;
        case 11: p->rgbKind[k] = K_SHADE_A; return;
        case 12: grey = rdp.env[3]; break;
        case 14: grey = rdp.primLodFrac; break;
        default: break;                               /* combined, noise, LOD, 0 */
    }
    p->rgbKind[k] = K_CONST;
    if (col != NULL) {
        p->rgb[k][0] = col[0];
        p->rgb[k][1] = col[1];
        p->rgb[k][2] = col[2];
    } else {
        p->rgb[k][0] = p->rgb[k][1] = p->rgb[k][2] = grey;
    }
}

static void plan_alpha(plan_t* p, int k, int src) {
    u8 v = 0;

    switch (src) {
        case 1: case 2: p->aKind[k] = K_TEXEL; p->usesTexture = 1; return;
        case 3: v = rdp.prim[3]; break;
        case 4: p->aKind[k] = K_SHADE; return;
        case 5: v = rdp.env[3]; break;
        case 6: v = 255; break;
        default: break;                               /* combined, LOD, 0 */
    }
    p->aKind[k] = K_CONST;
    p->a[k] = v;
}

static void plan_build(void) {
    plan_t* p = &sPlan;
    u32 hi = rdp.combineHi, lo = rdp.combineLo;
    int a = (hi >> 20) & 15, c = (hi >> 15) & 31, b = (lo >> 28) & 15, d = (lo >> 15) & 7;
    int aa = (hi >> 12) & 7, ac = (hi >> 9) & 7, ab = (lo >> 12) & 7, ad = (lo >> 9) & 7;

    memset(p, 0, sizeof(*p));
    /* Colour. A: 6 = 1, 7 = noise. B: 6 = key centre, 7 = K4. C: its own
     * table (6 = key scale, 7 = combined alpha, 8.. the alphas). D: 6 = 1. */
    plan_rgb(p, 0, a > 7 ? 0 : (a == 7 ? 0 : a));
    plan_rgb(p, 1, b > 5 ? 0 : b);
    plan_rgb(p, 2, (c == 6 || c == 7 || c == 13 || c >= 15) ? 0 : c);
    plan_rgb(p, 3, d == 7 ? 0 : d);
    /* Alpha. A, B, D: 6 = 1, 7 = 0. C: 0 = LOD fraction, 6 = primitive LOD. */
    plan_alpha(p, 0, aa == 7 ? 0 : aa);
    plan_alpha(p, 1, ab == 7 ? 0 : ab);
    if (ac == 6) {
        p->aKind[2] = K_CONST;
        p->a[2] = rdp.primLodFrac;
    } else {
        plan_alpha(p, 2, (ac == 0 || ac == 7) ? 0 : ac);
    }
    plan_alpha(p, 3, ad == 7 ? 0 : ad);
    sPlanDirty = 0;
}

static void plan_apply(const plan_t* p, ps3_rvert_t* o, const u8 shade[4]) {
    int k;

    memset(o->c, 0, sizeof(o->c) + sizeof(o->wrgb) + sizeof(o->wa));
    for (k = 0; k < 4; k++) {
        switch (p->rgbKind[k]) {
            case K_CONST:
                o->c[k][0] = p->rgb[k][0];
                o->c[k][1] = p->rgb[k][1];
                o->c[k][2] = p->rgb[k][2];
                break;
            case K_SHADE:
                o->c[k][0] = shade[0];
                o->c[k][1] = shade[1];
                o->c[k][2] = shade[2];
                break;
            case K_SHADE_A:
                o->c[k][0] = o->c[k][1] = o->c[k][2] = shade[3];
                break;
            case K_TEXEL:
                o->wrgb[k * 2] = 255;
                break;
            default:
                o->wrgb[k * 2 + 1] = 255;
                break;
        }
        switch (p->aKind[k]) {
            case K_CONST:
                o->c[k][3] = p->a[k];
                break;
            case K_SHADE:
                o->c[k][3] = shade[3];
                break;
            default:
                o->wa[k] = 255;
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Render state                                                        */
/* ------------------------------------------------------------------ */

#define CYCLE_TYPE() ((rdp.otherModeH >> G_MDSFT_CYCLETYPE) & 3)   /* 0 1-cycle, 1 2-cycle, 2 copy, 3 fill */

/* Blend, depth, alpha test and scissor for what is about to be drawn. */
static void state_base(ps3_rstate_t* st, int isRect) {
    u32 ml = rdp.otherModeL;
    /* Translucent render modes force blending and use 1 - alpha as the
     * memory factor of the blender's last cycle. */
    int useAlpha = (ml & FORCE_BL) != 0 && ((ml >> 16) & 3) == G_BL_1MA;
    int textureEdge = (ml & CVG_X_ALPHA) == CVG_X_ALPHA;
    int zb = !isRect && (rsp.geometryMode & G_ZBUFFER) != 0;

    memset(st, 0, sizeof(*st));
    st->anchor = sWide ? (s8) sHudAnchor : 0;
    st->sx = rdp.scX;
    st->sy = rdp.scY;
    st->sw = rdp.scW;
    st->sh = rdp.scH;
    /* 16:9 in a race: 2D under the whole-screen scissor may draw into the
     * side bars (clouds and stars beyond the 4:3 edges). Not in the menus,
     * which keep things waiting just off the 4:3 screen. */
    st->fullsc = sWideRace && rdp.scX <= 0 && rdp.scY <= 0 && rdp.scW >= PS3_SCREEN_W && rdp.scH >= PS3_SCREEN_H;
    if (!isRect) {
        /* The game sets FRUSTRATIO_1: the RSP clips triangles at the edges of
         * the viewport. In split screen the scissor starts at 0,0 for every
         * view, so without this a view spills into its neighbours. */
        int vx0 = (int) (rsp.vpTransX - fabsf(rsp.vpScaleX) + 0.5f);
        int vy0 = (int) (rsp.vpTransY - fabsf(rsp.vpScaleY) + 0.5f);
        int vx1 = (int) (rsp.vpTransX + fabsf(rsp.vpScaleX) + 0.5f);
        int vy1 = (int) (rsp.vpTransY + fabsf(rsp.vpScaleY) + 0.5f);
        int x0 = st->sx > vx0 ? st->sx : vx0, y0 = st->sy > vy0 ? st->sy : vy0;
        int x1 = st->sx + st->sw < vx1 ? st->sx + st->sw : vx1;
        int y1 = st->sy + st->sh < vy1 ? st->sy + st->sh : vy1;

        st->sx = (s16) x0;
        st->sy = (s16) y0;
        st->sw = (s16) (x1 > x0 ? x1 - x0 : 0);
        st->sh = (s16) (y1 > y0 ? y1 - y0 : 0);
    }
    st->depth_test = zb && (ml & Z_CMP) != 0;
    st->depth_write = zb && (ml & Z_UPD) != 0;
    st->decal = st->depth_test && (ml & ZMODE_DEC) == ZMODE_DEC;
    if (textureEdge) {
        st->alpha_ref = 77;                           /* cutout: opaque or nothing */
    } else if (useAlpha) {
        st->blend = 1;
        st->alpha_ref = 1;                            /* fully transparent texels write no depth */
    }
    if ((ml & 3) == G_AC_THRESHOLD && rdp.blend[3] > st->alpha_ref) {
        st->alpha_ref = rdp.blend[3];
    }
}

static void state_texture(ps3_rstate_t* st, const texent_t* e, const tile_t* tile) {
    st->tex = e->tex;
    st->filter = ((rdp.otherModeH >> G_MDSFT_TEXTFILT) & 3) != 0;
    st->wrap_s = (tile->cms & G_TX_CLAMP) ? PS3_WRAP_CLAMP : ((tile->cms & G_TX_MIRROR) ? PS3_WRAP_MIRROR : PS3_WRAP_REPEAT);
    st->wrap_t = (tile->cmt & G_TX_CLAMP) ? PS3_WRAP_CLAMP : ((tile->cmt & G_TX_MIRROR) ? PS3_WRAP_MIRROR : PS3_WRAP_REPEAT);
    /* A texture narrower than its wrap mask repeats on the N64 inside the
     * mask; what the port can do is repeat it whole. Only powers of two
     * can repeat on the RSX. */
    if ((e->w & (e->w - 1)) != 0) {
        st->wrap_s = PS3_WRAP_CLAMP;
    }
    if ((e->h & (e->h - 1)) != 0) {
        st->wrap_t = PS3_WRAP_CLAMP;
    }
}

/* ------------------------------------------------------------------ */
/* RSP: matrices, lights, vertices                                     */
/* ------------------------------------------------------------------ */

static void mtx_mul(float r[4][4], float a[4][4], float b[4][4]) {
    float t[4][4];
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(r, t, sizeof(t));
}

/* How far apart two matrices are if they look like the same thing one frame
 * apart, -1 if they do not. The camera is in the projection (lookAt is
 * multiplied into it), so a modelview's translation is a position in the
 * world: nothing moves more than 64 units in a frame. */
static float mtx_match(float a[4][4], float b[4][4], int proj, float maxMove) {
    float big = 0.0f, diff = 0.0f, sum = 0.0f, dt = 0.0f;
    int r, c, rows = proj ? 4 : 3;

    for (r = 0; r < rows; r++) {
        for (c = 0; c < 4; c++) {
            float d = fabsf(a[r][c] - b[r][c]);

            sum += d;
            if (d > diff) diff = d;
            if (fabsf(b[r][c]) > big) big = fabsf(b[r][c]);
        }
    }
    if (diff > 0.5f * big + 0.001f) {
        return -1.0f;                                 /* turned or scaled too much */
    }
    if (proj) {
        return sum;
    }
    for (c = 0; c < 3; c++) {
        dt += (a[3][c] - b[3][c]) * (a[3][c] - b[3][c]);
    }
    if (dt > maxMove * maxMove) {
        return -1.0f;
    }
    return sum + sqrtf(dt);
}

/* An orthographic projection (the HUD, the menus, the sky): no w column.
 * (A perspective one has the camera's forward vector there, lookAt being
 * multiplied into it.) */
static int is_ortho(float m[4][4]) {
    return fabsf(m[0][3]) + fabsf(m[1][3]) + fabsf(m[2][3]) < 1e-4f;
}

int gPs3FpsCounter;
int gPs3ScreenFit = 100;
u32 gPs3FpsShown;

/* ---- FPS counter: a 5x7 font of its own, drawn over everything ---------- */
static const char sFontChars[] = "0123456789FPS ";
static const u8 sFont[14][7] = {
    { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },     { 14, 17, 1, 2, 4, 8, 31 },
    { 31, 2, 4, 2, 1, 17, 14 },     { 2, 6, 10, 18, 31, 2, 2 },    { 31, 16, 30, 1, 1, 17, 14 },
    { 6, 8, 16, 30, 17, 17, 14 },   { 31, 1, 2, 4, 8, 8, 8 },      { 14, 17, 17, 14, 17, 17, 14 },
    { 14, 17, 17, 15, 1, 2, 12 },   { 31, 16, 16, 30, 16, 16, 16 }, { 30, 17, 17, 30, 16, 16, 16 },
    { 15, 16, 16, 14, 1, 1, 30 },   { 0, 0, 0, 0, 0, 0, 0 },
};

static ps3_rvert_t* rect(const ps3_rstate_t* st, float x0, float y0, float x1, float y1, float u0, float v0,
                         float u1, float v1, int flip);

static void draw_fps_counter(void) {
    static ps3_rtex_t* font;
    char text[16];
    ps3_rstate_t st;
    int n, i, k;

    if (font == NULL) {
        static u32 px[8 * 14 * 8];               /* 14 cells of 8x8, in a row */

        for (i = 0; i < 14; i++) {
            for (k = 0; k < 7; k++) {
                int b;

                for (b = 0; b < 5; b++) {
                    px[k * 112 + i * 8 + b] = (sFont[i][k] >> (4 - b)) & 1 ? 0xFFFFFFFFu : 0;
                }
            }
        }
        font = PS3_Rsx_TexCreate(112, 8, px);
        if (font == NULL) {
            return;
        }
    }
    n = snprintf(text, sizeof(text), "%u FPS", (unsigned) gPs3FpsShown);
    memset(&st, 0, sizeof(st));
    st.tex = font;
    st.wrap_s = st.wrap_t = PS3_WRAP_CLAMP;
    st.blend = 1;
    st.alpha_ref = 1;
    st.sw = PS3_SCREEN_W;
    st.sh = PS3_SCREEN_H;
    st.anchor = sWide ? PS3_HUD_LEFT : 0;      /* 16:9: in the corner of the screen */
    for (i = 0; i < n; i++) {
        const char* c = strchr(sFontChars, text[i]);
        int g = c ? (int) (c - sFontChars) : 13;
        ps3_rvert_t* o;

        for (k = 0; k < 2; k++) {                 /* a dark shadow, then the yellow digits */
            float x = 4.0f + i * 6.0f + (k ? 0.0f : 1.0f), y = 4.0f + (k ? 0.0f : 1.0f);
            int j;

            o = rect(&st, x, y, x + 8.0f, y + 8.0f, g * 8.0f / 112.0f, 0.0f, (g + 1) * 8.0f / 112.0f, 1.0f, 0);
            for (j = 0; j < 6; j++) {
                o[j].wrgb[0] = 255;               /* A = texel */
                o[j].wa[0] = 255;
                o[j].c[2][0] = k ? 255 : 0;       /* C = the colour */
                o[j].c[2][1] = k ? 230 : 0;
                o[j].c[2][2] = 0;
                o[j].c[2][3] = 255;
            }
        }
    }
}

static void cmd_mtx(u32 w0, uintptr_t addr) {
    const u8* p = (const u8*) addr;
    float m[4][4];
    u32 params = (w0 >> 16) & 0xFF;
    int i;

    /* s15.16: 16 integer halves, then 16 fraction halves. */
    for (i = 0; i < 16; i++) {
        s32 hi = (s16) (((u32) p[i * 2] << 8) | p[i * 2 + 1]);
        u32 lo = ((u32) p[32 + i * 2] << 8) | p[32 + i * 2 + 1];

        m[i / 4][i % 4] = (float) ((hi << 16) | (s32) lo) * (1.0f / 65536.0f);
    }

    {
        /* 60 fps: the matrix's place in its gfx pool */
        int proj = (params & G_MTX_PROJECTION) != 0, slot = -1, k;
        u32 off = 0;

        for (k = 0; k < 2; k++) {
            uintptr_t base = (uintptr_t) &gGfxPools[k];

            if (addr >= base && addr < base + POOL_MTX_END) {
                off = (u32) (addr - base);
                if ((off % sizeof(Mtx)) == 0) {
                    slot = (int) (off / sizeof(Mtx));
                }
                break;
            }
        }
        if (slot >= 0 && !sInterp) {
            memcpy(sPrevMtx[slot], m, sizeof(m));
            sPrevStamp[slot] = sRealFrames + 1;      /* 0: never loaded */
        } else if (slot >= 0 && sPrevStamp[slot] == sRealFrames &&
                   /* not under an orthographic projection (HUD). A projection
                    * multiplied in (the camera's lookAt) goes with the one it
                    * multiplies. */
                   !is_ortho((proj && (params & G_MTX_LOAD)) ? m : rsp.proj)) {
            /* in the previous real frame (this one is drawn after) */
            float maxMove = off >= (u32) offsetof(struct GfxPool, mtxEffect) ? 16.0f : 64.0f;

            if (mtx_match(sPrevMtx[slot], m, proj, maxMove) >= 0.0f) {
                float (*a)[4] = sPrevMtx[slot];
                int r, c;

                for (r = 0; r < 4; r++) {
                    for (c = 0; c < 4; c++) {
                        m[r][c] = a[r][c] + (m[r][c] - a[r][c]) * sInterpT;
                    }
                }
                gPs3InterpStats.paired++;
            } else {
                gPs3InterpStats.unpaired++;
            }
        } else if (sInterp && slot >= 0) {
            gPs3InterpStats.unpaired++;
        }
    }

    if (params & G_MTX_PROJECTION) {
        if (params & G_MTX_LOAD) {
            memcpy(rsp.proj, m, sizeof(m));
        } else {
            mtx_mul(rsp.proj, m, rsp.proj);
        }
    } else {
        if ((params & G_MTX_PUSH) && rsp.mvTop < 10) {
            rsp.mvTop++;
            memcpy(rsp.mv[rsp.mvTop], rsp.mv[rsp.mvTop - 1], sizeof(m));
        }
        if (params & G_MTX_LOAD) {
            memcpy(rsp.mv[rsp.mvTop], m, sizeof(m));
        } else {
            mtx_mul(rsp.mv[rsp.mvTop], m, rsp.mv[rsp.mvTop]);
        }
        rsp.lightsDirty = 1;
    }
    rsp.mpDirty = 1;
    gPs3GfxStats.matrices++;
}

/* A direction in world space, in the space of the model (normals are not
 * transformed: the light is brought to them). */
static void dir_to_model(float out[3], const signed char dir[3]) {
    float (*m)[4] = rsp.mv[rsp.mvTop];
    float d[3] = { dir[0] / 127.0f, dir[1] / 127.0f, dir[2] / 127.0f };
    float len;
    int i;

    for (i = 0; i < 3; i++) {
        out[i] = d[0] * m[i][0] + d[1] * m[i][1] + d[2] * m[i][2];
    }
    len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 0.0f) {
        out[0] /= len;
        out[1] /= len;
        out[2] /= len;
    }
}

static void cmd_vtx_body(u32 w0, uintptr_t addr) {
    const Vtx* src = (const Vtx*) addr;
    int n = (w0 >> 10) & 0x3F;
    int v0 = ((w0 >> 16) & 0xFF) / 2;
    int i, k, viewWide;

    if (v0 + n > MAX_VERTS) {
        n = MAX_VERTS - v0;
    }
    if (rsp.mpDirty) {
        mtx_mul(rsp.mp, rsp.mv[rsp.mvTop], rsp.proj);
        rsp.mpDirty = 0;
    }
    if ((rsp.geometryMode & G_LIGHTING) && rsp.lightsDirty) {
        for (k = 0; k < rsp.numLights; k++) {
            dir_to_model(rsp.lightDir[k], rsp.lights[k].dir);
        }
        dir_to_model(rsp.lookatDir[0], rsp.lookat[0].dir);
        dir_to_model(rsp.lookatDir[1], rsp.lookat[1].dir);
        rsp.lightsDirty = 0;
    }

    viewWide = sWide;

    for (i = 0; i < n; i++, src++) {
        vert_t* d = &rsp.v[v0 + i];
        float x = src->v.ob[0], y = src->v.ob[1], z = src->v.ob[2];
        s32 u = src->v.tc[0], v = src->v.tc[1];

        d->x = x * rsp.mp[0][0] + y * rsp.mp[1][0] + z * rsp.mp[2][0] + rsp.mp[3][0];
        d->y = x * rsp.mp[0][1] + y * rsp.mp[1][1] + z * rsp.mp[2][1] + rsp.mp[3][1];
        d->z = x * rsp.mp[0][2] + y * rsp.mp[1][2] + z * rsp.mp[2][2] + rsp.mp[3][2];
        d->w = x * rsp.mp[0][3] + y * rsp.mp[1][3] + z * rsp.mp[2][3] + rsp.mp[3][3];

        if (rsp.geometryMode & G_LIGHTING) {
            const Light_t* amb = &rsp.lights[rsp.numLights];
            float nx = src->n.n[0], ny = src->n.n[1], nz = src->n.n[2];
            float r = amb->col[0], g = amb->col[1], b = amb->col[2];

            for (k = 0; k < rsp.numLights; k++) {
                float f = (nx * rsp.lightDir[k][0] + ny * rsp.lightDir[k][1] + nz * rsp.lightDir[k][2]) / 127.0f;

                if (f > 0.0f) {
                    r += f * rsp.lights[k].col[0];
                    g += f * rsp.lights[k].col[1];
                    b += f * rsp.lights[k].col[2];
                }
            }
            d->r = (u8) (r > 255.0f ? 255.0f : r);
            d->g = (u8) (g > 255.0f ? 255.0f : g);
            d->b = (u8) (b > 255.0f ? 255.0f : b);

            if (rsp.geometryMode & G_TEXTURE_GEN) {
                float dx = nx * rsp.lookatDir[0][0] + ny * rsp.lookatDir[0][1] + nz * rsp.lookatDir[0][2];
                float dy = nx * rsp.lookatDir[1][0] + ny * rsp.lookatDir[1][1] + nz * rsp.lookatDir[1][2];

                u = (s32) ((dx / 127.0f + 1.0f) / 4.0f * 65536.0f);
                v = (s32) ((dy / 127.0f + 1.0f) / 4.0f * 65536.0f);
            }
        } else {
            d->r = src->v.cn[0];
            d->g = src->v.cn[1];
            d->b = src->v.cn[2];
        }
        d->a = src->v.cn[3];

        d->u = (float) ((u * (s32) rsp.texScaleS) >> 16);
        d->v = (float) ((v * (s32) rsp.texScaleT) >> 16);

        /* 16:9: a third more field of view across; this x times the 4/3 wider
         * screen puts what was there in 4:3 at the same pixels. */
        d->persp = !is_ortho(rsp.proj);
        d->wide = (u8) viewWide;
        if (viewWide && d->persp) {
            d->x *= 0.75f;
        }

        d->clip = 0;
        if (d->x < -d->w) d->clip |= 1;
        if (d->x > d->w) d->clip |= 2;
        if (d->y < -d->w) d->clip |= 4;
        if (d->y > d->w) d->clip |= 8;
        if (d->z < -d->w) d->clip |= 16;
        if (d->z > d->w) d->clip |= 32;

        if (rsp.geometryMode & G_FOG) {
            float w = d->w, f;

            if (fabsf(w) < 0.001f) {
                w = 0.001f;
            }
            f = d->z / w;
            if (f < -1.0f || w < 0.0f) {
                f = -1.0f;
            }
            f = f * (float) rsp.fogMul + (float) rsp.fogOffset;
            d->a = (u8) (f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
        }
    }
    gPs3GfxStats.vtxLoads++;
    gPs3GfxStats.vertices += (unsigned) n;
}

static void cmd_vtx(u32 w0, uintptr_t addr) {
    u64 t0 = ps3_tb();

    cmd_vtx_body(w0, addr);
    gPs3Prof.verts += (w0 >> 10) & 0x3F;
    gPs3Prof.t[PROF_VTX] += ps3_tb() - t0;
}

static void emit_pos(ps3_rvert_t* o, const vert_t* v) {
    o->x = v->x * (rsp.vpScaleX / (PS3_SCREEN_W / 2.0f)) + v->w * (rsp.vpTransX / (PS3_SCREEN_W / 2.0f) - 1.0f);
    o->y = v->y * (rsp.vpScaleY / (PS3_SCREEN_H / 2.0f)) + v->w * (1.0f - rsp.vpTransY / (PS3_SCREEN_H / 2.0f));
    o->z = v->z;
    o->w = v->w;
}

static float tile_shift(float c, int shift) {
    if (shift == 0) {
        return c;
    }
    return (shift <= 10) ? c / (float) (1 << shift) : c * (float) (1 << (16 - shift));
}

static void tri_body(int i0, int i1, int i2) {
    const vert_t* v[3];
    ps3_rstate_t st;
    ps3_rvert_t* o;
    const texent_t* tex = NULL;
    const tile_t* tile = &rdp.tile[rsp.texTile];
    int useFog, i;

    if (i0 >= MAX_VERTS || i1 >= MAX_VERTS || i2 >= MAX_VERTS) {
        return;
    }
    v[0] = &rsp.v[i0];
    v[1] = &rsp.v[i1];
    v[2] = &rsp.v[i2];
    gPs3GfxStats.triangles++;

    if (v[0]->clip & v[1]->clip & v[2]->clip) {
        gPs3GfxStats.culledClip++;
        return;                                       /* all three outside the same plane */
    }
    if ((rsp.geometryMode & G_CULL_BOTH) && PS3_Rsx_GetDebug() != PS3_RDBG_NO_CULL) {
        /* Facing from the homogeneous determinant of (x, y, w): its sign is
         * the winding of what is left on screen after clipping, also when a
         * vertex is behind the camera (dividing by w first gets that wrong,
         * and the ground under the camera always has such vertices). */
        float cross = v[0]->x * (v[1]->y * v[2]->w - v[2]->y * v[1]->w) -
                      v[0]->y * (v[1]->x * v[2]->w - v[2]->x * v[1]->w) +
                      v[0]->w * (v[1]->x * v[2]->y - v[2]->x * v[1]->y);

        switch (rsp.geometryMode & G_CULL_BOTH) {
            case G_CULL_FRONT:                        /* front = counter-clockwise, y up */
                if (cross >= 0) { gPs3GfxStats.culledBack++; return; }
                break;
            case G_CULL_BACK:
                if (cross <= 0) { gPs3GfxStats.culledBack++; return; }
                break;
            default:
                return;
        }
    }
    if (rdp.colorImage == rdp.depthImage) {
        return;
    }

    if (sPlanDirty) {
        plan_build();
    }
    state_base(&st, 0);
    if (sPlan.usesTexture) {
        tex = tex_for_tile(rsp.texTile);
        if (tex != NULL) {
            state_texture(&st, tex, tile);
        } else {
            tex_missing(rsp.texTile);
        }
    }
    useFog = (rdp.otherModeL >> 30) == G_BL_CLR_FOG && (rsp.geometryMode & G_FOG);

    if (sWide) {
        /* 16:9: 3D across the whole width. 2D in the 4:3 middle, except what
         * covers its whole view across (the sky): stretched to the edges. */
        st.wide = v[0]->wide;
        if (st.wide && !v[0]->persp) {
            float lo = 2.0f, hi = -2.0f;

            for (i = 0; i < 3; i++) {
                float x = v[i]->x / (fabsf(v[i]->w) > 1e-6f ? v[i]->w : 1e-6f);   /* across its view */

                if (x < lo) lo = x;
                if (x > hi) hi = x;
            }
            st.wide = lo <= -0.99f && hi >= 0.99f;
        }
    }
    o = batch(&st, 3);
    for (i = 0; i < 3; i++, o++) {
        /* Flat shading: the colour of the first vertex. */
        const vert_t* cv = (rsp.geometryMode & G_SHADING_SMOOTH) ? v[i] : v[0];
        u8 shade[4];

        shade[0] = cv->r;
        shade[1] = cv->g;
        shade[2] = cv->b;
        shade[3] = v[i]->a;
        emit_pos(o, v[i]);
        if (sWide && !st.wide) {
            /* into the 4:3 middle, or against an edge */
            o->x = o->x * 0.75f + (float) sHudAnchor * 0.25f * o->w;
        }
        if (tex != NULL) {
            float s = tile_shift(v[i]->u, tile->shifts) - (float) tile->uls * 8.0f;
            float t = tile_shift(v[i]->v, tile->shiftt) - (float) tile->ult * 8.0f;

            if (st.filter) {
                s += 16.0f;                           /* half a texel: the N64 samples at the corner */
                t += 16.0f;
            }
            o->u = s / 32.0f / (float) tex->w;
            o->v = t / 32.0f / (float) tex->h;
        } else {
            o->u = o->v = 0.0f;
        }
        plan_apply(&sPlan, o, shade);
        o->fog[0] = rdp.fog[0];
        o->fog[1] = rdp.fog[1];
        o->fog[2] = rdp.fog[2];
        o->fog[3] = useFog ? v[i]->a : 0;
    }
}

static void tri(int i0, int i1, int i2) {
    u64 t0 = ps3_tb();

    tri_body(i0, i1, i2);
    gPs3Prof.tris++;
    gPs3Prof.t[PROF_TRI] += ps3_tb() - t0;
}

/* ------------------------------------------------------------------ */
/* Rectangles                                                          */
/* ------------------------------------------------------------------ */

/* A screen rectangle (pixels, y down) as two triangles; u/v per corner. */
static ps3_rvert_t* rect(const ps3_rstate_t* st, float x0, float y0, float x1, float y1, float u0, float v0,
                         float u1, float v1, int flip) {
    static const u8 cx[6] = { 0, 1, 0, 1, 1, 0 }, cy[6] = { 0, 0, 1, 0, 1, 1 };
    ps3_rvert_t* o = batch(st, 6);
    int i;

    for (i = 0; i < 6; i++) {
        float fx = cx[i], fy = cy[i];

        o[i].x = (x0 + (x1 - x0) * fx) / (PS3_SCREEN_W / 2.0f) - 1.0f;
        if (sWide && !st->wide) {
            o[i].x = o[i].x * 0.75f + (float) st->anchor * 0.25f;   /* 16:9: 2D in the 4:3 middle or at an edge */
        }
        o[i].y = 1.0f - (y0 + (y1 - y0) * fy) / (PS3_SCREEN_H / 2.0f);
        o[i].z = 0.0f;
        o[i].w = 1.0f;
        if (flip) {                                   /* TEXRECTFLIP: s runs down, t runs right */
            o[i].u = u0 + (u1 - u0) * fy;
            o[i].v = v0 + (v1 - v0) * fx;
        } else {
            o[i].u = u0 + (u1 - u0) * fx;
            o[i].v = v0 + (v1 - v0) * fy;
        }
        memset(o[i].c, 0, sizeof(o[i].c) + sizeof(o[i].wrgb) + sizeof(o[i].wa) + sizeof(o[i].fog));
    }
    return o;
}

static void cmd_fillrect_body(u32 w0, u32 w1) {
    float x1 = (float) ((w0 >> 12) & 0xFFF) / 4.0f, y1 = (float) (w0 & 0xFFF) / 4.0f;
    float x0 = (float) ((w1 >> 12) & 0xFFF) / 4.0f, y0 = (float) (w1 & 0xFFF) / 4.0f;
    ps3_rstate_t st;
    ps3_rvert_t* o;
    int cycle = CYCLE_TYPE(), i;

    gPs3GfxStats.fillRects++;
    if (rdp.colorImage == rdp.depthImage) {
        flush();
        PS3_Rsx_ClearDepth();                         /* the game clearing its z-buffer */
        return;
    }
    if (cycle >= 2) {                                 /* fill and copy include the last pixel */
        x1 += 1.0f;
        y1 += 1.0f;
    }

    state_base(&st, 1);
    if (cycle == 3) {
        st.blend = 0;
        st.alpha_ref = 0;
    }
    /* 16:9: what covers the screen across or top to bottom (clears, the
     * split-screen dividers) goes to the edges; the rest is 2D. */
    st.wide = x0 <= 0.0f && x1 >= PS3_SCREEN_W - 1 ? 1 : (y0 <= 0.0f && y1 >= PS3_SCREEN_H - 1);
    o = rect(&st, x0, y0, x1, y1, 0, 0, 0, 0, 0);
    if (cycle == 3) {
        for (i = 0; i < 6; i++) {
            memcpy(o[i].c[3], rdp.fill, 4);           /* D = the fill colour */
        }
    } else {
        /* A rectangle has no shade colour: the RDP uses 0. select_framebuffer
         * clears the screen this way (1-cycle, G_CC_SHADE) and gets black. */
        static const u8 flat[4] = { 0, 0, 0, 0 };

        if (sPlanDirty) {
            plan_build();
        }
        for (i = 0; i < 6; i++) {
            plan_apply(&sPlan, &o[i], flat);
            memset(o[i].wrgb, 0, sizeof(o[i].wrgb) + sizeof(o[i].wa));   /* no texture bound */
        }
    }
}

static void cmd_fillrect(u32 w0, u32 w1) {
    u64 t0 = ps3_tb();

    cmd_fillrect_body(w0, w1);
    gPs3Prof.t[PROF_RECT] += ps3_tb() - t0;
}

static void cmd_texrect_body(u32 w0, u32 w1, u32 st0, u32 st1, int flip) {
    float x1 = (float) ((w0 >> 12) & 0xFFF) / 4.0f, y1 = (float) (w0 & 0xFFF) / 4.0f;
    float x0 = (float) ((w1 >> 12) & 0xFFF) / 4.0f, y0 = (float) (w1 & 0xFFF) / 4.0f;
    int t = (w1 >> 24) & 7;
    float s = (float) (s16) (st0 >> 16) / 32.0f, tt = (float) (s16) (st0 & 0xFFFF) / 32.0f;
    float dsdx = (float) (s16) (st1 >> 16) / 1024.0f, dtdy = (float) (s16) (st1 & 0xFFFF) / 1024.0f;
    const tile_t* tile = &rdp.tile[t];
    const texent_t* tex;
    ps3_rstate_t st;
    ps3_rvert_t* o;
    float u0, v0, u1, v1, du, dv;
    int cycle = CYCLE_TYPE(), i;

    gPs3GfxStats.texRects++;
    if (rdp.colorImage == rdp.depthImage) {
        return;
    }
    if (cycle >= 2) {
        x1 += 1.0f;
        y1 += 1.0f;
        dsdx /= 4.0f;                                 /* copy mode steps four texels per word */
    }
    tex = tex_for_tile(t);

    state_base(&st, 1);
    if (cycle == 2) {
        st.blend = 0;
        st.alpha_ref = (rdp.otherModeL & 3) ? 128 : 0;
    }
    if (tex != NULL) {
        state_texture(&st, tex, tile);
        if (cycle == 2) {
            st.filter = 0;
        }
    }

    s -= (float) tile->uls / 4.0f;
    tt -= (float) tile->ult / 4.0f;
    du = (flip ? (y1 - y0) : (x1 - x0)) * dsdx;
    dv = (flip ? (x1 - x0) : (y1 - y0)) * dtdy;
    if (tex != NULL) {
        u0 = s / (float) tex->w;
        v0 = tt / (float) tex->h;
        u1 = (s + du) / (float) tex->w;
        v1 = (tt + dv) / (float) tex->h;
    } else {
        u0 = v0 = u1 = v1 = 0.0f;
    }

    st.wide = x0 <= 0.0f && x1 >= PS3_SCREEN_W - 1;   /* 16:9: a full-width picture is stretched */
    o = rect(&st, x0, y0, x1, y1, u0, v0, u1, v1, flip);
    if (cycle == 2) {
        for (i = 0; i < 6; i++) {
            if (tex != NULL) {
                o[i].wrgb[6] = 255;                   /* D = the texel */
                o[i].wa[3] = 255;
            }
        }
    } else {
        static const u8 flat[4] = { 255, 255, 255, 255 };

        if (sPlanDirty) {
            plan_build();
        }
        for (i = 0; i < 6; i++) {
            plan_apply(&sPlan, &o[i], flat);
            if (tex == NULL) {
                memset(o[i].wrgb, 0, sizeof(o[i].wrgb) + sizeof(o[i].wa));
            }
        }
    }
}

static void cmd_texrect(u32 w0, u32 w1, u32 st0, u32 st1, int flip) {
    u64 t0 = ps3_tb();

    cmd_texrect_body(w0, w1, st0, st1, flip);
    gPs3Prof.t[PROF_RECT] += ps3_tb() - t0;
}

/* ------------------------------------------------------------------ */
/* RDP: tiles and loads                                                */
/* ------------------------------------------------------------------ */

static void cmd_settile(u32 w0, u32 w1) {
    tile_t* t = &rdp.tile[(w1 >> 24) & 7];

    t->fmt = (w0 >> 21) & 7;
    t->siz = (w0 >> 19) & 3;
    t->line = (w0 >> 9) & 0x1FF;
    t->tmem = w0 & 0x1FF;
    t->palette = (w1 >> 20) & 15;
    t->cmt = (w1 >> 18) & 3;
    t->maskt = (w1 >> 14) & 15;
    t->shiftt = (w1 >> 10) & 15;
    t->cms = (w1 >> 8) & 3;
    t->masks = (w1 >> 4) & 15;
    t->shifts = w1 & 15;
    sTexGen++;
}

static void cmd_settilesize(u32 w0, u32 w1) {
    tile_t* t = &rdp.tile[(w1 >> 24) & 7];

    t->uls = (w0 >> 12) & 0xFFF;
    t->ult = w0 & 0xFFF;
    t->lrs = (w1 >> 12) & 0xFFF;
    t->lrt = w1 & 0xFFF;
    sTexGen++;
}

static void cmd_loadblock(u32 w0, u32 w1) {
    const tile_t* t = &rdp.tile[(w1 >> 24) & 7];
    load_t* ld = &rdp.load[t->tmem & 511];
    u32 texels = ((w1 >> 12) & 0xFFF) + 1;

    (void) w0;
    ld->addr = rdp.timgAddr;
    ld->isTile = 0;
    ld->siz = rdp.timgSiz;
    ld->bytes = (texels << rdp.timgSiz) >> 1;
    sTexGen++;
}

static void cmd_loadtile(u32 w0, u32 w1) {
    const tile_t* t = &rdp.tile[(w1 >> 24) & 7];
    load_t* ld = &rdp.load[t->tmem & 511];

    ld->addr = rdp.timgAddr;
    ld->isTile = 1;
    ld->siz = rdp.timgSiz;
    ld->imgWidth = rdp.timgWidth;
    ld->uls = ((w0 >> 12) & 0xFFF) >> 2;
    ld->ult = (w0 & 0xFFF) >> 2;
    ld->lrs = ((w1 >> 12) & 0xFFF) >> 2;
    ld->lrt = (w1 & 0xFFF) >> 2;
    ld->bytes = 0;
    sTexGen++;
}

static void cmd_loadtlut(u32 w0, u32 w1) {
    const tile_t* t = &rdp.tile[(w1 >> 24) & 7];
    int count = (int) ((w1 >> 14) & 0x3FF) + 1;
    int slot = (t->tmem >= 256) ? (t->tmem - 256) / 16 : 0;
    int i;

    (void) w0;
    for (i = 0; i * 16 < count && slot + i < 16; i++) {
        rdp.palette[slot + i] = rdp.timgAddr + i * 32;
    }
    sTexGen++;
}

/* ------------------------------------------------------------------ */
/* The interpreter                                                     */
/* ------------------------------------------------------------------ */

static void set_color(u8 c[4], u32 w1) {
    c[0] = (u8) (w1 >> 24);
    c[1] = (u8) (w1 >> 16);
    c[2] = (u8) (w1 >> 8);
    c[3] = (u8) w1;
    sPlanDirty = 1;
}

static void cmd_moveword(u32 w0, u32 w1) {
    u32 index = w0 & 0xFF, offset = (w0 >> 8) & 0xFFFF;   /* F3DEX: cmd << 24 | offset << 8 | index */

    switch (index) {
        case G_MW_SEGMENT:
            if (offset / 4 < 16) {
                rsp.segment[offset / 4] = (uintptr_t) w1;
            }
            break;
        case G_MW_NUMLIGHT:
            rsp.numLights = (int) ((w1 - 0x80000000u) / 32) - 1;
            if (rsp.numLights < 0) rsp.numLights = 0;
            if (rsp.numLights > MAX_LIGHTS) rsp.numLights = MAX_LIGHTS;
            rsp.lightsDirty = 1;
            break;
        case G_MW_FOG:
            rsp.fogMul = (s16) (w1 >> 16);
            rsp.fogOffset = (s16) w1;
            break;
        default:
            break;                                        /* clip ratio, perspective normalize, ... */
    }
}

static void cmd_movemem(u32 w0, u32 w1) {
    u32 index = (w0 >> 16) & 0xFF;
    uintptr_t addr = resolve(w1);

    if (!plausible(addr)) {
        bad("G_MOVEMEM from a bad address", w0, w1);
        return;
    }
    if (index == G_MV_VIEWPORT) {
        const Vp_t* vp = (const Vp_t*) addr;

        rsp.vpScaleX = vp->vscale[0] / 4.0f;
        rsp.vpScaleY = vp->vscale[1] / 4.0f;
        rsp.vpTransX = vp->vtrans[0] / 4.0f;
        rsp.vpTransY = vp->vtrans[1] / 4.0f;
        flush();
    } else if (index == G_MV_LOOKATY || index == G_MV_LOOKATX) {
        memcpy(&rsp.lookat[index == G_MV_LOOKATY], (const void*) addr, sizeof(Light_t));
        rsp.lightsDirty = 1;
    } else if (index >= G_MV_L0 && index <= G_MV_L7) {
        memcpy(&rsp.lights[(index - G_MV_L0) / 2], (const void*) addr, sizeof(Light_t));
        rsp.lightsDirty = 1;
    }
}

static void run(const Gfx* dl) {
    const Gfx* stack[MAX_DL_DEPTH];
    int depth = 0;
    ps3_gfx_stats_t* stats = &gPs3GfxStats;

    for (;;) {
        u32 w0 = ((const u32*) dl)[0];
        u32 w1 = ((const u32*) dl)[1];
        u32 op = w0 >> 24;
        uintptr_t addr;

        dl++;
        if (++stats->commands > MAX_COMMANDS) {
            bad("too many commands, giving up", w0, w1);
            return;
        }

        switch (op) {
            /* ---- RSP ---- */
            case (u8) G_ENDDL:
                if (depth == 0) {
                    return;
                }
                dl = stack[--depth];
                break;

            case (u8) G_DL:
                addr = resolve(w1);
                stats->dlCalls++;
                if (!plausible(addr)) {
                    bad("G_DL to a bad address", w0, w1);
                    break;
                }
                if (((w0 >> 16) & 0xFF) == G_DL_PUSH) {
                    if (depth >= MAX_DL_DEPTH) {
                        bad("display list stack overflow", w0, w1);
                        break;
                    }
                    stack[depth++] = dl;
                    if ((u32) depth > stats->maxDepth) {
                        stats->maxDepth = depth;
                    }
                }
                dl = (const Gfx*) addr;
                break;

            case (u8) G_MTX:
                addr = resolve(w1);
                if (plausible(addr)) {
                    cmd_mtx(w0, addr);
                } else {
                    bad("G_MTX from a bad address", w0, w1);
                }
                break;

            case (u8) G_NOOP:
                if ((w1 & 0xFFFFFF00u) == PS3_HUD_TAG_BASE) {
                    sHudAnchor = (s8) (w1 & 0xFF);    /* the game: where the next HUD element goes */
                }
                break;

            case (u8) G_POPMTX:
                if (rsp.mvTop > 0) {
                    rsp.mvTop--;
                    rsp.mpDirty = 1;
                    rsp.lightsDirty = 1;
                }
                break;

            case (u8) G_VTX:
                addr = resolve(w1);
                if (plausible(addr)) {
                    cmd_vtx(w0, addr);
                } else {
                    bad("G_VTX from a bad address", w0, w1);
                }
                break;

            case (u8) G_TRI1:
                tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;
            case (u8) G_TRI2:
                tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;
            case (u8) G_QUAD: {
                int a = ((w1 >> 16) & 0xFF) / 2, b = ((w1 >> 8) & 0xFF) / 2, c = (w1 & 0xFF) / 2, d = (w1 >> 24) / 2;

                tri(a, b, c);
                tri(a, c, d);
                break;
            }

            case (u8) G_MOVEWORD:
                cmd_moveword(w0, w1);
                break;
            case (u8) G_MOVEMEM:
                cmd_movemem(w0, w1);
                break;

            case (u8) G_TEXTURE:
                rsp.texScaleS = (u16) (w1 >> 16);
                rsp.texScaleT = (u16) w1;
                rsp.texTile = (w0 >> 8) & 7;
                rsp.texOn = (u8) w0;
                break;

            case (u8) G_SETGEOMETRYMODE:
                rsp.geometryMode |= w1;
                break;
            case (u8) G_CLEARGEOMETRYMODE:
                rsp.geometryMode &= ~w1;
                break;

            case (u8) G_SETOTHERMODE_H:
            case (u8) G_SETOTHERMODE_L: {
                u32 sft = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
                u32 mask = ((len >= 32) ? 0xFFFFFFFFu : ((1u << len) - 1)) << sft;

                if (op == (u8) G_SETOTHERMODE_H) {
                    rdp.otherModeH = (rdp.otherModeH & ~mask) | w1;
                    sTexGen++;                        /* the palette type lives here */
                } else {
                    rdp.otherModeL = (rdp.otherModeL & ~mask) | w1;
                }
                break;
            }

            /* ---- RDP ---- */
            case G_RDPSETOTHERMODE:
                rdp.otherModeH = w0 & 0x00FFFFFF;
                rdp.otherModeL = w1;
                sTexGen++;
                break;

            case G_SETCOMBINE:
                rdp.combineHi = w0 & 0x00FFFFFF;
                rdp.combineLo = w1;
                sPlanDirty = 1;
                break;
            case G_SETENVCOLOR:
                set_color(rdp.env, w1);
                break;
            case G_SETPRIMCOLOR:
                set_color(rdp.prim, w1);
                rdp.primLodFrac = (u8) w0;
                break;
            case G_SETBLENDCOLOR:
                set_color(rdp.blend, w1);
                break;
            case G_SETFOGCOLOR:
                set_color(rdp.fog, w1);
                break;
            case G_SETFILLCOLOR: {
                u32 c = rgba16_to_argb(w1 & 0xFFFF);

                rdp.fill[0] = (u8) (c >> 16);
                rdp.fill[1] = (u8) (c >> 8);
                rdp.fill[2] = (u8) c;
                rdp.fill[3] = (u8) (c >> 24);
                break;
            }

            case G_SETTIMG:
                addr = resolve(w1);
                rdp.timgFmt = (w0 >> 21) & 7;
                rdp.timgSiz = (w0 >> 19) & 3;
                rdp.timgWidth = (u16) ((w0 & 0xFFF) + 1);
                if (plausible(addr)) {
                    rdp.timgAddr = (const u8*) addr;
                } else {
                    rdp.timgAddr = NULL;
                    bad("G_SETTIMG to a bad address", w0, w1);
                }
                break;
            case G_SETTILE:
                cmd_settile(w0, w1);
                break;
            case G_SETTILESIZE:
                cmd_settilesize(w0, w1);
                break;
            case G_LOADBLOCK:
                cmd_loadblock(w0, w1);
                break;
            case G_LOADTILE:
                cmd_loadtile(w0, w1);
                break;
            case G_LOADTLUT:
                cmd_loadtlut(w0, w1);
                break;

            case G_SETCIMG:
                flush();
                rdp.colorImage = resolve(w1);
                break;
            case G_SETZIMG:
                rdp.depthImage = resolve(w1);
                break;

            case G_SETSCISSOR: {
                int x0 = ((w0 >> 12) & 0xFFF) >> 2, y0 = (w0 & 0xFFF) >> 2;
                int x1 = ((w1 >> 12) & 0xFFF) >> 2, y1 = (w1 & 0xFFF) >> 2;

                rdp.scX = (s16) x0;
                rdp.scY = (s16) y0;
                rdp.scW = (s16) (x1 > x0 ? x1 - x0 : 0);
                rdp.scH = (s16) (y1 > y0 ? y1 - y0 : 0);
                break;
            }

            case G_FILLRECT:
                cmd_fillrect(w0, w1);
                break;

            case G_TEXRECT:
            case G_TEXRECTFLIP: {
                /* F3D_OLD: the texture coordinates follow in the w1 of two
                 * more commands (G_RDPHALF_2, G_RDPHALF_CONT). */
                u32 st0 = ((const u32*) dl)[1], st1 = ((const u32*) dl)[3];

                dl += 2;
                cmd_texrect(w0, w1, st0, st1, op == G_TEXRECTFLIP);
                break;
            }

            default:
                break;                                /* syncs, no-ops, culling hints, what is not used */
        }
    }
}

/* One pass over a frame's display list. interp: the in-between frame, which
 * leaves the statistics and the recorded matrices alone. */
static void draw_pass_body(const Gfx* dl, int interp);

static void draw_pass(const Gfx* dl, int interp) {
    u64 t0 = ps3_tb();

    draw_pass_body(dl, interp);
    gPs3Prof.t[interp ? PROF_PASS_INTERP : PROF_PASS] += ps3_tb() - t0;
}

static void draw_pass_body(const Gfx* dl, int interp) {
    static ps3_gfx_stats_t saved;
    u32 errorsSoFar = gPs3GfxStats.totalErrors;
    uint64_t t0 = PS3_Plat_TimeUs();
    int i;

    if (interp) {
        saved = gPs3GfxStats;
    }
    memset(&gPs3GfxStats, 0, sizeof(gPs3GfxStats));
    gPs3GfxStats.frame = interp ? sFrame : ++sFrame;
    gPs3GfxStats.totalErrors = errorsSoFar;
    sInterp = interp;
    sHudAnchor = 0;

    if (sFrame == 1) {
        memset(sTexIndex, 0xFF, sizeof(sTexIndex));
    }
    if ((sFrame & 63) == 0 || sTexCount > TEX_MAX - 256) {
        tex_evict(sTexCount > TEX_MAX - 256 && (sFrame & 63) != 0);
    }

    /* What the RSP has at reset. */
    memset(&rsp, 0, sizeof(rsp));
    for (i = 0; i < 4; i++) {
        rsp.mv[0][i][i] = 1.0f;
        rsp.proj[i][i] = 1.0f;
    }
    rsp.mpDirty = 1;
    rsp.lightsDirty = 1;
    rsp.vpScaleX = rsp.vpTransX = PS3_SCREEN_W / 2.0f;
    rsp.vpScaleY = rsp.vpTransY = PS3_SCREEN_H / 2.0f;
    rsp.lookat[0].dir[0] = 127;
    rsp.lookat[1].dir[1] = 127;
    rdp.scX = rdp.scY = 0;
    rdp.scW = PS3_SCREEN_W;
    rdp.scH = PS3_SCREEN_H;
    rdp.colorImage = 0;
    rdp.depthImage = 1;
    sPlanDirty = 1;
    sTexGen++;

    if (sFrameOpen) {
        PS3_Rsx_EndFrame();                       /* a task with no swap after it */
    }
    /* 16:9, except the end-of-race screens of a 1P race: their views and 2D
     * panels slide past each other, laid out for 4:3; drawn in 4:3. */
    sWide = gPs3Wide && !(gScreenModeSelection == SCREEN_MODE_1P && D_8015F894 != 0);
    sWideRace = sWide && gGamestate == RACING && gScreenModeSelection == SCREEN_MODE_1P;
    PS3_Rsx_SetWide(sWide);
    PS3_Rsx_SetScreenFit(gPs3ScreenFit);
    PS3_Rsx_BeginFrame();
    sFrameOpen = 1;
    if (!plausible((uintptr_t) dl)) {
        bad("the task's display list pointer is bad", 0, (u32) (uintptr_t) dl);
    } else {
        run(dl);
    }
    if (gPs3FpsCounter) {
        draw_fps_counter();
    }
    flush();
    sInterp = 0;
    if (interp) {
        gPs3GfxStats = saved;
        gPs3InterpStats.drawnUs += (u32) (PS3_Plat_TimeUs() - t0);
        gPs3InterpStats.inbetween++;
        return;
    }
    gPs3GfxStats.totalErrors += gPs3GfxStats.errors;
    sRealFrames++;
    gPs3InterpStats.lastPassUs = (u32) (PS3_Plat_TimeUs() - t0);
}

void ps3_gfx_run_task(void* taskPtr) {
    OSTask* task = (OSTask*) taskPtr;
    const Gfx* dl = (const Gfx*) task->t.data_ptr;

    if (sPendingDl != NULL) {
        draw_pass(sPendingDl, 0);                 /* two tasks before a swap */
        sPendingDl = NULL;
    }
    if (gPs3Fps60) {
        sPendingDl = dl;                          /* drawn by ps3_frame_end */
        return;
    }
    draw_pass(dl, 0);
}

int ps3_gfx_pending(void) {
    return sPendingDl != NULL;
}

/* The frame half-way between the last one shown and the pending one. */
void ps3_gfx_draw_inbetween(float t) {
    if (sPendingDl == NULL || sRealFrames == 0) {
        return;
    }
    sInterpT = t;
    draw_pass(sPendingDl, 1);
    PS3_Rsx_EndFrame();
    sFrameOpen = 0;
    gPs3InterpStats.inbetweenNow++;
}

/* The frame the game just built is shown here (called from osViSwapBuffer
 * through ps3_frame_end). */
void ps3_gfx_end_frame(void) {
    if (sPendingDl != NULL) {
        draw_pass(sPendingDl, 0);
        sPendingDl = NULL;
    }
    if (sFrameOpen) {
        PS3_Rsx_EndFrame();
        sFrameOpen = 0;
    }
}
