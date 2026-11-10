/* ps3_rsx.c -- the drawing backend of ps3_rsx.h on the RSX.
 *
 * Triangles arrive in clip space from the display list interpreter
 * (game/ps3_gfx.c) and are drawn with one vertex program and one fragment
 * program (shaders/, precompiled in ps3_shader_data.h). Everything GCM lives
 * here. The calls and constants are the ones of Doom64-PS3's backend, which
 * runs on hardware.
 *
 * VERTICES: a ring in RSX memory split in 3 segments, one per framebuffer.
 * PS3_Video_BeginFrame waits until at most one flip is in flight, so when a
 * segment is reused the RSX has finished the frame that read it.
 *
 * VIEWPORT: the N64 screen is 320x240 (4:3). It is centred on the output with
 * bars at the sides, not stretched.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "ps3_rsx.h"
#include "ps3_video.h"
#include "ps3_log.h"
#include "ps3_shader_data.h"

#define RING_SEGMENTS   3
#define RING_SEG_SIZE   (4 * 1024 * 1024)

#define TEX_REMAP_IDENTITY  0x00AAE4       /* same as IoQuake3-PS3 */
#define TEX_FREE_DELAY      4              /* frames before freeing */
#define TEX_PENDING_MAX     4096

struct ps3_rtex {
    void       *data;
    u32         offset;
    u16         w, h;
    int         swz;         /* 1 = swizzled (Morton), 0 = linear */
    gcmTexture  gt;
};

typedef struct {
    ps3_rtex_t *t;
    u32         due;
} pending_free_t;

static int      s_ready = 0;

static uint8_t *s_ring = NULL;
static u32      s_ring_off = 0;
static u32      s_seg = 0;
static u32      s_head = 0;
static int      s_warned_full = 0;

static rsxVertexProgram   *s_vp = NULL;
static void               *s_vp_ucode = NULL;
/* Fragment programs: 0 = layout B (normal), 1 = layout A, 2 = alpha view. */
enum { FP_B, FP_A, FP_ALPHA, FP_COUNT };
static rsxFragmentProgram *s_fp[FP_COUNT];
static void               *s_fp_ucode[FP_COUNT];
static u32                 s_fp_off[FP_COUNT];
static int                 s_cur_fp = -1;

/* Layout B, what the RSX reads (56 bytes, like ps3_rvert_t):
 *   0  pos   f32 x4            TEX0... see bind_layout_b()
 *  16  uv    f32 x2
 *  24  t1..t4  u8 x4 each: constant colour of A, B, C, D (w unused)
 *  40  c1    u8 x4: constant alpha of A, B, C, D
 *  44  t5    u8 x4: texel flags of the colour of A, B, C
 *  48  t6    u8 x4: texel flags of the colour of D; texel-alpha flags of A|B, C|D
 *  52  c0    u8 x4: fog colour and amount
 * On the console the w of texture coordinates 1..7 and coordinate 7 did not
 * reach the fragment program (layout A put the alphas there): layout B only
 * uses x, y, z and the two colours. */
typedef struct {
    float   x, y, z, w;
    float   u, v;
    uint8_t t[4][4];
    uint8_t c1[4];
    uint8_t t5[4];
    uint8_t t6[4];
    uint8_t c0[4];
} vert_b_t;
typedef char vert_b_size_check[(sizeof(vert_b_t) == sizeof(ps3_rvert_t)) ? 1 : -1];

static pending_free_t s_pending[TEX_PENDING_MAX];
static int            s_npending = 0;
static u32            s_tex_bytes = 0;
static ps3_rtex_t    *s_white = NULL;      /* bound when a draw has no texture */

/* What the RSX currently has, so the same thing is not sent twice. */
static ps3_rstate_t s_cur;
static int          s_cur_valid = 0;

/* New textures written by the CPU: invalidate the RSX texture cache before
 * sampling. */
static int s_tex_dirty = 0;

static u32 s_frames = 0;
static u32 s_draws = 0, s_verts = 0;

static int s_dbg = 0;           /* PS3_RDBG_* */
static int fp_for_mode(void)
{
    return s_dbg == PS3_RDBG_SHADER_A ? FP_A : s_dbg == PS3_RDBG_SHOW_ALPHA ? FP_ALPHA : FP_B;
}
static int s_log_next = 0;      /* L3 pressed: log the next frame */
static int s_logging = 0;       /* logging this frame */

void PS3_Rsx_SetDebug(int mode) { s_dbg = mode; s_cur_valid = 0; }
int  PS3_Rsx_GetDebug(void) { return s_dbg; }
void PS3_Rsx_LogNextFrame(void) { s_log_next = 1; }

const char *PS3_Rsx_DebugName(int mode)
{
    switch (mode) {
    case PS3_RDBG_NORMAL:        return "normal";
    case PS3_RDBG_NO_ALPHA_TEST: return "no alpha test";
    case PS3_RDBG_NO_DEPTH:      return "no depth test";
    case PS3_RDBG_NO_BLEND:      return "no blending";
    case PS3_RDBG_NO_TEXTURE:    return "no textures (white)";
    case PS3_RDBG_NO_SCISSOR:    return "no scissor";
    case PS3_RDBG_NO_CULL:       return "no backface culling";
    case PS3_RDBG_SHADER_A:      return "first vertex layout (A)";
    case PS3_RDBG_SHOW_ALPHA:    return "alpha as grey";
    default:                     return "?";
    }
}

/* Game area on the output, in pixels. */
static u32 s_vx, s_vy, s_vw, s_vh;   /* the 4:3 game area */
static u32 s_ax, s_aw;               /* what clip space covers: the game area, or all the width in 16:9 */
static int s_wide;
static int s_fit = 100;

void PS3_Rsx_SetWide(int on)
{
    s_wide = on ? 1 : 0;
}

void PS3_Rsx_SetScreenFit(int percent)
{
    s_fit = percent < 50 ? 50 : (percent > 100 ? 100 : percent);
}

/* The areas of the screen, from the screen fit and the aspect ratio. */
static void set_areas(u32 W, u32 H)
{
    u32 fw = W * (u32)s_fit / 100, fh = H * (u32)s_fit / 100;
    u32 fx = (W - fw) / 2, fy = (H - fh) / 2;

    s_vh = fh;
    s_vw = (fh * 4) / 3;
    if (s_vw > fw) { s_vw = fw; s_vh = (fw * 3) / 4; }
    s_vx = fx + (fw - s_vw) / 2;
    s_vy = fy + (fh - s_vh) / 2;
    s_ax = s_wide ? fx : s_vx;
    s_aw = s_wide ? fw : s_vw;
}

/* Offset of (x,y) in a swizzled 2^lw x 2^lh texture: interleaved bits, x
 * first. When one dimension runs out the other's bits follow. */
static u32 swz_offset(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 off = 0, bit = 0;
    while (lw || lh) {
        if (lw) { off |= (x & 1u) << bit; x >>= 1; bit++; lw--; }
        if (lh) { off |= (y & 1u) << bit; y >>= 1; bit++; lh--; }
    }
    return off;
}

static u32 ilog2u(u32 v) { u32 r = 0; while (v > 1) { v >>= 1; r++; } return r; }

/* ------------------------------------------------------------------ */

static int load_shaders(void)
{
    void *ucode;
    u32 size;

    s_vp = (rsxVertexProgram *)shader_vp_data;
    rsxVertexProgramGetUCode(s_vp, &s_vp_ucode, &size);
    ps3_logf("[rsx] vertex program: %u bytes of ucode", (unsigned)size);
    if (!s_vp_ucode) {
        ps3_log("[rsx] *** the vertex program did not load ***");
        return -1;
    }

    /* The fragment programs' ucode has to be in RSX memory. */
    {
        static const unsigned char *const data[FP_COUNT] = {
            shader_fp_b_data, shader_fp_a_data, shader_fp_alpha_data
        };
        static const char *const name[FP_COUNT] = { "B", "A", "alpha view" };
        int i;

        for (i = 0; i < FP_COUNT; i++) {
            s_fp[i] = (rsxFragmentProgram *)data[i];
            rsxFragmentProgramGetUCode(s_fp[i], &ucode, &size);
            s_fp_ucode[i] = rsxMemalign(64, size);
            if (!s_fp_ucode[i]) {
                ps3_log("[rsx] *** rsxMemalign failed for a fragment program ***");
                return -1;
            }
            memcpy(s_fp_ucode[i], ucode, size);
            rsxAddressToOffset(s_fp_ucode[i], &s_fp_off[i]);
            ps3_logf("[rsx] fragment program %s: %u bytes at offset 0x%08x",
                     name[i], (unsigned)size, (unsigned)s_fp_off[i]);
        }
    }
    return 0;
}

int PS3_Rsx_Init(void)
{
    static const uint32_t white = 0xFFFFFFFFu;
    u32 W, H;

    if (s_ready) return 0;
    if (!PS3_Video_Context()) {
        ps3_log("[rsx] *** no video context ***");
        return -1;
    }
    if (load_shaders() < 0) return -1;

    s_ring = (uint8_t *)rsxMemalign(128, RING_SEGMENTS * RING_SEG_SIZE);
    if (!s_ring) {
        ps3_logf("[rsx] *** rsxMemalign failed for the vertex ring (%u bytes) ***",
                 (unsigned)(RING_SEGMENTS * RING_SEG_SIZE));
        return -1;
    }
    rsxAddressToOffset(s_ring, &s_ring_off);
    ps3_logf("[rsx] vertex ring: %d x %u KB at offset 0x%08x, %u bytes per vertex",
             RING_SEGMENTS, (unsigned)(RING_SEG_SIZE / 1024), (unsigned)s_ring_off,
             (unsigned)sizeof(ps3_rvert_t));

    W = PS3_Video_Width();
    H = PS3_Video_Height();
    set_areas(W, H);
    ps3_logf("[rsx] game area %ux%u at (%u,%u) of %ux%u",
             (unsigned)s_vw, (unsigned)s_vh, (unsigned)s_vx, (unsigned)s_vy, (unsigned)W, (unsigned)H);

    s_ready = 1;
    s_white = PS3_Rsx_TexCreate(1, 1, &white);
    if (!s_white) {
        ps3_log("[rsx] *** could not create the 1x1 texture ***");
        s_ready = 0;
        return -1;
    }
    ps3_log("[rsx] backend ready");
    return 0;
}

void PS3_Rsx_Shutdown(void)
{
    if (!s_ready) return;
    s_ready = 0;
    /* The RSX is already stopped (PS3_Video_Shutdown does rsxFinish). */
    if (s_ring)     { rsxFree(s_ring);     s_ring = NULL; }
    {
        int i;
        for (i = 0; i < FP_COUNT; i++)
            if (s_fp_ucode[i]) { rsxFree(s_fp_ucode[i]); s_fp_ucode[i] = NULL; }
    }
}

/* ------------------------------------------------------------------ */

void PS3_Rsx_BeginFrame(void)
{
    gcmContextData *ctx = PS3_Video_Context();
    u32 W = PS3_Video_Width(), H = PS3_Video_Height();
    float scale[4], offset[4];
    int i;

    if (!s_ready || !ctx) return;

    PS3_Video_BeginFrame();

    s_seg  = (u32)PS3_Video_FrameIndex() % RING_SEGMENTS;
    s_head = 0;

    /* Whole screen to black (the side bars), depth to the far end. */
    rsxSetScissor(ctx, 0, 0, W, H);
    rsxSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
                         GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    rsxSetDepthWriteEnable(ctx, GCM_TRUE);
    rsxSetClearColor(ctx, 0xff000000u);
    rsxSetClearDepthStencil(ctx, 0xffffff00u);
    rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B |
                         GCM_CLEAR_A | GCM_CLEAR_S | GCM_CLEAR_Z);

    /* Clip space (x right, y up, z -w..w) onto the game area, or onto the
     * whole width in 16:9. */
    set_areas(W, H);
    scale[0]  = s_aw * 0.5f;   scale[1]  = s_vh * -0.5f;  scale[2]  = 0.5f; scale[3]  = 0.0f;
    offset[0] = s_ax + s_aw * 0.5f;
    offset[1] = s_vy + s_vh * 0.5f;
    offset[2] = 0.5f;
    offset[3] = 0.0f;
    rsxSetViewport(ctx, (u16)s_ax, (u16)s_vy, (u16)s_aw, (u16)s_vh, 0.0f, 1.0f, scale, offset);
    rsxSetViewportClip(ctx, 0, W, H);

    /* Culling is done by the interpreter (it needs it before the viewport). */
    rsxSetCullFaceEnable(ctx, GCM_FALSE);
    rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
    rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetPolygonOffset(ctx, -2.0f, -2.0f);
    s_cur_valid = 0;

    rsxLoadVertexProgram(ctx, s_vp, s_vp_ucode);
    s_cur_fp = fp_for_mode();
    rsxLoadFragmentProgramLocation(ctx, s_fp[s_cur_fp], s_fp_off[s_cur_fp], GCM_LOCATION_RSX);

    s_frames++;

    /* Textures released enough frames ago: the RSX no longer reads them. */
    i = 0;
    while (i < s_npending) {
        if ((int)(s_frames - s_pending[i].due) >= 0) {
            ps3_rtex_t *t = s_pending[i].t;
            s_tex_bytes -= (u32)t->w * t->h * 4;
            rsxFree(t->data);
            free(t);
            s_pending[i] = s_pending[--s_npending];
        } else {
            i++;
        }
    }
    if ((s_frames % 600) == 1)
        ps3_logf("[rsx] frame %u: last frame %u draws, %u vertices; %u KB of textures, %d waiting to be freed",
                 (unsigned)s_frames, (unsigned)s_draws, (unsigned)s_verts,
                 (unsigned)(s_tex_bytes / 1024), s_npending);
    s_draws = 0;
    s_verts = 0;
    s_logging = s_log_next;
    s_log_next = 0;
    if (s_logging)
        ps3_logf("[draw] ---- frame %u, mode \"%s\" ----", (unsigned)s_frames, PS3_Rsx_DebugName(s_dbg));
}

void PS3_Rsx_EndFrame(void)
{
    if (!s_ready) return;
    PS3_Video_EndFrame();
}

void PS3_Rsx_ClearDepth(void)
{
    gcmContextData *ctx = PS3_Video_Context();

    if (!s_ready || !ctx) return;
    rsxSetScissor(ctx, s_ax, s_vy, s_aw, s_vh);
    rsxSetDepthWriteEnable(ctx, GCM_TRUE);
    rsxSetClearDepthStencil(ctx, 0xffffff00u);
    rsxClearSurface(ctx, GCM_CLEAR_S | GCM_CLEAR_Z);
    s_cur_valid = 0;
}

/* ------------------------------------------------------------------ */

static u32 wrap_mode(int w)
{
    return w == PS3_WRAP_CLAMP ? GCM_TEXTURE_CLAMP_TO_EDGE :
           w == PS3_WRAP_MIRROR ? GCM_TEXTURE_MIRRORED_REPEAT : GCM_TEXTURE_REPEAT;
}

static void apply_state(gcmContextData *ctx, const ps3_rstate_t *st_in)
{
    ps3_rstate_t dbg = *st_in;
    const ps3_rstate_t *st = &dbg;
    ps3_rtex_t *tex;

    switch (s_dbg) {
    case PS3_RDBG_NO_ALPHA_TEST: dbg.alpha_ref = 0; break;
    case PS3_RDBG_NO_DEPTH:      dbg.depth_test = 0; dbg.depth_write = 0; dbg.decal = 0; break;
    case PS3_RDBG_NO_BLEND:      dbg.blend = 0; break;
    case PS3_RDBG_NO_TEXTURE:    dbg.tex = NULL; break;
    case PS3_RDBG_NO_SCISSOR:    dbg.sx = 0; dbg.sy = 0; dbg.sw = PS3_SCREEN_W; dbg.sh = PS3_SCREEN_H; break;
    case PS3_RDBG_SHOW_ALPHA:    dbg.alpha_ref = 0; dbg.blend = 0; break;
    default: break;
    }
    tex = st->tex ? st->tex : s_white;

    ps3_rtex_t *cur_tex = s_cur.tex ? s_cur.tex : s_white;

    if (s_cur_valid && memcmp(st, &s_cur, sizeof(*st)) == 0)
        return;

    if (!s_cur_valid || st->sx != s_cur.sx || st->sy != s_cur.sy ||
        st->sw != s_cur.sw || st->sh != s_cur.sh || st->wide != s_cur.wide || st->anchor != s_cur.anchor ||
        st->fullsc != s_cur.fullsc) {
        /* N64 pixels -> output pixels, clamped to the game area. */
        int x0 = st->sx, y0 = st->sy, x1 = st->sx + st->sw, y1 = st->sy + st->sh;
        u32 px0, py0, px1, py1;

        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > PS3_SCREEN_W) x1 = PS3_SCREEN_W;
        if (y1 > PS3_SCREEN_H) y1 = PS3_SCREEN_H;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        {
            u32 ax = (s_wide && st->wide) ? s_ax : s_vx, aw = (s_wide && st->wide) ? s_aw : s_vw;

            if (s_wide && !st->wide && st->anchor)       /* the 4:3 strip against an edge */
                ax = st->anchor < 0 ? s_ax : s_ax + s_aw - s_vw;

            px0 = ax + (u32)x0 * aw / PS3_SCREEN_W;
            px1 = ax + (u32)x1 * aw / PS3_SCREEN_W;
        }
        if (s_wide && !st->wide && st->fullsc) {         /* 2D that may go into the side bars */
            px0 = s_ax;
            px1 = s_ax + s_aw;
        }
        py0 = s_vy + (u32)y0 * s_vh / PS3_SCREEN_H;
        py1 = s_vy + (u32)y1 * s_vh / PS3_SCREEN_H;
        rsxSetScissor(ctx, (u16)px0, (u16)py0, (u16)(px1 - px0), (u16)(py1 - py0));
    }

    if (!s_cur_valid || st->blend != s_cur.blend)
        rsxSetBlendEnable(ctx, st->blend ? GCM_TRUE : GCM_FALSE);

    if (!s_cur_valid || st->depth_test != s_cur.depth_test || st->decal != s_cur.decal) {
        rsxSetDepthTestEnable(ctx, st->depth_test ? GCM_TRUE : GCM_FALSE);
        rsxSetDepthFunc(ctx, st->decal ? GCM_LEQUAL : GCM_LESS);
        rsxSetPolygonOffsetFillEnable(ctx, st->decal ? GCM_TRUE : GCM_FALSE);
    }

    if (!s_cur_valid || st->depth_write != s_cur.depth_write)
        rsxSetDepthWriteEnable(ctx, st->depth_write ? GCM_TRUE : GCM_FALSE);

    if (!s_cur_valid || st->alpha_ref != s_cur.alpha_ref) {
        rsxSetAlphaTestEnable(ctx, st->alpha_ref ? GCM_TRUE : GCM_FALSE);
        if (st->alpha_ref)
            rsxSetAlphaFunc(ctx, GCM_GEQUAL, st->alpha_ref);
    }

    if (!s_cur_valid || tex != cur_tex || st->filter != s_cur.filter ||
        st->wrap_s != s_cur.wrap_s || st->wrap_t != s_cur.wrap_t || s_tex_dirty) {
        u32 f = st->filter ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
        u32 ws = wrap_mode(st->wrap_s), wt = wrap_mode(st->wrap_t);

        /* Linear (not swizzled) textures only work with CLAMP. */
        if (!tex->swz) ws = wt = GCM_TEXTURE_CLAMP_TO_EDGE;
        if (s_tex_dirty) {
            __asm__ volatile("sync" ::: "memory");
            rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
            s_tex_dirty = 0;
        }
        rsxLoadTexture(ctx, 0, &tex->gt);
        rsxTextureControl(ctx, 0, GCM_TRUE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
        rsxTextureFilter(ctx, 0, 0, f, f, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
        rsxTextureWrapMode(ctx, 0, ws, wt, GCM_TEXTURE_CLAMP_TO_EDGE,
                           GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                           GCM_TEXTURE_ZFUNC_NEVER, 0);
    }

    s_cur = *st;
    s_cur_valid = 1;
}

void PS3_Rsx_Draw(const ps3_rstate_t *st, const ps3_rvert_t *v, int n)
{
    gcmContextData *ctx = PS3_Video_Context();
    u32 bytes, off;
    uint8_t *dst;
    int i;

    if (!s_ready || !ctx || n < 3) return;

    bytes = (u32)n * (u32)sizeof(ps3_rvert_t);
    if (s_head + bytes > RING_SEG_SIZE) {
        /* What the RSX has not read yet is not overwritten: the batch is dropped. */
        if (!s_warned_full) {
            ps3_logf("[rsx] *** vertex segment full (%u + %u > %u): triangles dropped ***",
                     (unsigned)s_head, (unsigned)bytes, (unsigned)RING_SEG_SIZE);
            s_warned_full = 1;
        }
        return;
    }

    dst = s_ring + s_seg * RING_SEG_SIZE + s_head;
    off = s_ring_off + s_seg * RING_SEG_SIZE + s_head;
    if (fp_for_mode() == FP_A) {
        memcpy(dst, v, bytes);
    } else {
        vert_b_t *o = (vert_b_t *)dst;
        for (i = 0; i < n; i++, o++) {
            const ps3_rvert_t *q = &v[i];
            int k;
            o->x = q->x; o->y = q->y; o->z = q->z; o->w = q->w;
            o->u = q->u; o->v = q->v;
            for (k = 0; k < 4; k++) {
                o->t[k][0] = q->c[k][0];
                o->t[k][1] = q->c[k][1];
                o->t[k][2] = q->c[k][2];
                o->t[k][3] = 0;
                o->c1[k] = q->c[k][3];
                o->c0[k] = q->fog[k];
            }
            o->t5[0] = (q->wrgb[0] ? 64 : 0) | (q->wrgb[1] ? 128 : 0);
            o->t5[1] = (q->wrgb[2] ? 64 : 0) | (q->wrgb[3] ? 128 : 0);
            o->t5[2] = (q->wrgb[4] ? 64 : 0) | (q->wrgb[5] ? 128 : 0);
            o->t5[3] = 0;
            o->t6[0] = (q->wrgb[6] ? 64 : 0) | (q->wrgb[7] ? 128 : 0);
            o->t6[1] = (q->wa[0] ? 64 : 0) | (q->wa[1] ? 128 : 0);
            o->t6[2] = (q->wa[2] ? 64 : 0) | (q->wa[3] ? 128 : 0);
            o->t6[3] = 0;
        }
    }
    /* Each block starts aligned to 16: otherwise the attribute offsets shift
     * and the RSX vertex fetch hangs the console (IoQuake3-PS3 note). */
    s_head = (s_head + bytes + 15u) & ~15u;

    apply_state(ctx, st);
    if (s_cur_fp != fp_for_mode()) {
        s_cur_fp = fp_for_mode();
        rsxLoadFragmentProgramLocation(ctx, s_fp[s_cur_fp], s_fp_off[s_cur_fp], GCM_LOCATION_RSX);
    }

    /* Same offsets in both layouts; only what is at 24..55 is read
     * differently. TEX1..TEX4 at 24..39, then COLOR1 (B) or TEX5 (A) at 40,
     * TEX5/TEX6 (B) or TEX6/TEX7 (A) at 44/48, COLOR0 at 52. */
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, off + 0,
                             sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, off + 16,
                             sizeof(ps3_rvert_t), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    for (i = 0; i < 4; i++)
        rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1 + i, 0, off + 24 + 4 * (u32)i,
                                 sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    if (s_cur_fp == FP_A) {
        for (i = 0; i < 3; i++)
            rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX5 + i, 0, off + 40 + 4 * (u32)i,
                                     sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR1, 0, off + 52,
                                 sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    } else {
        rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR1, 0, off + 40,
                                 sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX5, 0, off + 44,
                                 sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX6, 0, off + 48,
                                 sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX7, 0, off + 48,
                                 sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    }
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, off + 52,
                             sizeof(ps3_rvert_t), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);

    rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, 0, (u32)n);

    if (s_logging) {
        const ps3_rvert_t *q = &v[0];
        ps3_logf("[draw] %3u n=%-4d tex %dx%d%s blend %d zt %d zw %d decal %d aref %u sc %d,%d %dx%d wrap %d,%d | "
                 "v0 %.1f %.1f %.1f %.1f uv %.3f %.3f A %02x%02x%02x%02x B %02x%02x%02x%02x C %02x%02x%02x%02x "
                 "D %02x%02x%02x%02x w %02x%02x%02x%02x%02x%02x%02x%02x wa %02x%02x%02x%02x fog %02x",
                 (unsigned)s_draws, n, st->tex ? st->tex->w : 0, st->tex ? st->tex->h : 0,
                 st->tex ? (st->tex->swz ? "s" : "l") : "-", st->blend, st->depth_test, st->depth_write,
                 st->decal, st->alpha_ref, st->sx, st->sy, st->sw, st->sh, st->wrap_s, st->wrap_t,
                 q->x, q->y, q->z, q->w, q->u, q->v,
                 q->c[0][0], q->c[0][1], q->c[0][2], q->c[0][3], q->c[1][0], q->c[1][1], q->c[1][2], q->c[1][3],
                 q->c[2][0], q->c[2][1], q->c[2][2], q->c[2][3], q->c[3][0], q->c[3][1], q->c[3][2], q->c[3][3],
                 q->wrgb[0], q->wrgb[1], q->wrgb[2], q->wrgb[3], q->wrgb[4], q->wrgb[5], q->wrgb[6], q->wrgb[7],
                 q->wa[0], q->wa[1], q->wa[2], q->wa[3], q->fog[3]);
    }

    s_draws++;
    s_verts += (u32)n;
    if (s_frames == 1 && s_draws == 1)
        ps3_logf("[rsx] first draw: %d vertices", n);
}

/* ------------------------------------------------------------------ */

ps3_rtex_t *PS3_Rsx_TexCreate(int w, int h, const uint32_t *argb)
{
    ps3_rtex_t *t;
    u32 bytes;

    if (!s_ready || w <= 0 || h <= 0) return NULL;

    t = (ps3_rtex_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;

    bytes = (u32)w * (u32)h * 4;
    t->data = rsxMemalign(128, bytes);
    if (!t->data) {
        static int warned = 0;
        if (!warned) {
            ps3_logf("[rsx] *** rsxMemalign failed for a %dx%d texture (%u KB of textures in use) ***",
                     w, h, (unsigned)(s_tex_bytes / 1024));
            warned = 1;
        }
        free(t);
        return NULL;
    }
    /* Native 0xAARRGGBB (big-endian) = bytes A,R,G,B = the RSX's A8R8G8B8.
     * Powers of two are swizzled (Morton order): the only layout that can
     * repeat on the RSX. */
    if ((w & (w - 1)) == 0 && (h & (h - 1)) == 0) {
        u32 *dst = (u32 *)t->data;
        u32 lw = ilog2u((u32)w), lh = ilog2u((u32)h), x, y;
        for (y = 0; y < (u32)h; y++)
            for (x = 0; x < (u32)w; x++)
                dst[swz_offset(x, y, lw, lh)] = argb[y * (u32)w + x];
        t->swz = 1;
    } else {
        memcpy(t->data, argb, bytes);
        t->swz = 0;
    }
    rsxAddressToOffset(t->data, &t->offset);
    s_tex_dirty = 1;
    t->w = (u16)w;
    t->h = (u16)h;

    memset(&t->gt, 0, sizeof(t->gt));
    t->gt.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 |
                      (t->swz ? GCM_TEXTURE_FORMAT_SWZ : GCM_TEXTURE_FORMAT_LIN);
    t->gt.mipmap    = 1;
    t->gt.dimension = GCM_TEXTURE_DIMS_2D;
    t->gt.cubemap   = GCM_FALSE;
    t->gt.remap     = TEX_REMAP_IDENTITY;
    t->gt.width     = (u16)w;
    t->gt.height    = (u16)h;
    t->gt.depth     = 1;
    t->gt.location  = GCM_LOCATION_RSX;
    t->gt.pitch     = t->swz ? 0 : (u32)w * 4;
    t->gt.offset    = t->offset;

    s_tex_bytes += bytes;
    return t;
}

void PS3_Rsx_TexRelease(ps3_rtex_t *t)
{
    if (!t) return;
    /* It may be referenced by frames the RSX has not drawn yet: it is freed
     * TEX_FREE_DELAY frames later. */
    if (s_npending < TEX_PENDING_MAX) {
        s_pending[s_npending].t   = t;
        s_pending[s_npending].due = s_frames + TEX_FREE_DELAY;
        s_npending++;
    }
    /* If the queue ever fills, memory is lost rather than risking a hang. */
}
