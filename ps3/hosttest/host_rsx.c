/* host_rsx.c -- ps3_rsx.h in software, for the host test.
 *
 * Draws into a 320x240 buffer with the same combiner formula as
 * shaders/mk64_fp.fcg and writes chosen frames as PPM files, so what the
 * display list interpreter produces can be looked at without a console. It
 * is a reference, not a copy of the RSX: no filtering, no MSAA.
 *
 *   MK64_DUMP="300 600 900"   frames to write as frame_NNNNN.ppm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ps3_rsx.h"

#define WMAX 427                            /* 320 * 4 / 3: 16:9 */
#define H PS3_SCREEN_H
static int W = PS3_SCREEN_W;                /* output width: 320 (4:3) or 427 (16:9) */
static int s_wide;
void PS3_Rsx_SetWide(int on) { s_wide = on ? 1 : 0; }
void PS3_Rsx_SetScreenFit(int percent) { (void)percent; }   /* the host draws the N64 screen only */
#define NA 34                               /* interpolated floats per vertex */

struct ps3_rtex { int w, h; uint32_t *px; };

static float s_color[H][WMAX][3];
static float s_depth[H][WMAX];
static unsigned s_frame;
static unsigned s_dump[256];
static int s_ndump = -1;

int PS3_Rsx_Init(void) { return 0; }
void PS3_Rsx_Shutdown(void) {}

void PS3_Rsx_ClearDepth(void)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) s_depth[y][x] = 1.0f;
}

static int s_want;
static long s_written;

static void parse_dump(void)
{
    const char *e = getenv("MK64_DUMP");
    s_ndump = 0;
    while (e && *e && s_ndump < 256) {
        char *end;
        unsigned long v = strtoul(e, &end, 10);
        if (end == e) break;
        s_dump[s_ndump++] = (unsigned)v;
        e = end;
        while (*e == ' ' || *e == ',') e++;
    }
}

void PS3_Rsx_BeginFrame(void)
{
    int i;

    if (s_ndump < 0) parse_dump();
    s_frame++;
    s_want = 0;
    for (i = 0; i < s_ndump; i++) if (s_dump[i] == s_frame) s_want = 1;
    if (!s_want) return;                  /* only the dumped frames are drawn: qemu is slow */
    W = s_wide ? WMAX : PS3_SCREEN_W;
    memset(s_color, 0, sizeof(s_color));
    PS3_Rsx_ClearDepth();
}

void PS3_Rsx_EndFrame(void)
{
    int i, x, y;
    char name[64];
    FILE *f;

    if (!s_want) return;
    snprintf(name, sizeof(name), "frame_%05u.ppm", s_frame);
    f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            for (i = 0; i < 3; i++) {
                float c = s_color[y][x][i];
                fputc((int)(c <= 0 ? 0 : c >= 1 ? 255 : c * 255.0f + 0.5f), f);
            }
    fclose(f);
    printf("[host] wrote %s\n", name);
}

ps3_rtex_t *PS3_Rsx_TexCreate(int w, int h, const uint32_t *argb)
{
    ps3_rtex_t *t = malloc(sizeof(*t));
    t->w = w;
    t->h = h;
    t->px = malloc((size_t)w * h * 4);
    memcpy(t->px, argb, (size_t)w * h * 4);
    return t;
}

void PS3_Rsx_TexRelease(ps3_rtex_t *t)
{
    if (t) { free(t->px); free(t); }
}

/* ------------------------------------------------------------------ */

typedef struct { float x, y, z, w; float a[NA]; } cv_t;

static void to_cv(cv_t *o, const ps3_rvert_t *v)
{
    int i, k = 0;
    o->x = v->x; o->y = v->y; o->z = v->z; o->w = v->w;
    o->a[k++] = v->u;
    o->a[k++] = v->v;
    for (i = 0; i < 16; i++) o->a[k++] = v->c[i / 4][i % 4] / 255.0f;
    for (i = 0; i < 8; i++) o->a[k++] = v->wrgb[i] / 255.0f;
    for (i = 0; i < 4; i++) o->a[k++] = v->wa[i] / 255.0f;
    for (i = 0; i < 4; i++) o->a[k++] = v->fog[i] / 255.0f;
}

static void lerp_cv(cv_t *o, const cv_t *a, const cv_t *b, float t)
{
    int i;
    o->x = a->x + (b->x - a->x) * t;
    o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t;
    o->w = a->w + (b->w - a->w) * t;
    for (i = 0; i < NA; i++) o->a[i] = a->a[i] + (b->a[i] - a->a[i]) * t;
}

static float wrapf(float c, int n, int mode)
{
    float f;
    if (mode == PS3_WRAP_CLAMP) {
        f = c * n;
        return f < 0 ? 0 : f > n - 1 ? n - 1 : f;
    }
    if (mode == PS3_WRAP_MIRROR) {
        float p = c - 2.0f * floorf(c / 2.0f);
        if (p > 1.0f) p = 2.0f - p;
        f = p * n;
    } else {
        f = (c - floorf(c)) * n;
    }
    return f > n - 1 ? n - 1 : f;
}

static void shade(const ps3_rstate_t *st, const float *a, float out[4])
{
    float t[4] = { 1, 1, 1, 1 }, in[4][4];
    const float *c = a + 2, *w = a + 18, *wa = a + 26, *fog = a + 30;
    int k, i;

    if (st->tex) {
        int x = (int)wrapf(a[0], st->tex->w, st->wrap_s);
        int y = (int)wrapf(a[1], st->tex->h, st->wrap_t);
        uint32_t p = st->tex->px[y * st->tex->w + x];
        t[0] = ((p >> 16) & 255) / 255.0f;
        t[1] = ((p >> 8) & 255) / 255.0f;
        t[2] = (p & 255) / 255.0f;
        t[3] = (p >> 24) / 255.0f;
    }
    for (k = 0; k < 4; k++) {
        for (i = 0; i < 3; i++) in[k][i] = c[k * 4 + i] + w[k * 2] * t[i] + w[k * 2 + 1] * t[3];
        in[k][3] = c[k * 4 + 3] + wa[k] * t[3];
    }
    for (i = 0; i < 4; i++) {
        float v = (in[0][i] - in[1][i]) * in[2][i] + in[3][i];
        out[i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
    for (i = 0; i < 3; i++) out[i] += (fog[i] - out[i]) * fog[3];
}

static void raster(const ps3_rstate_t *st, const cv_t *p0, const cv_t *p1, const cv_t *p2)
{
    const cv_t *p[3] = { p0, p1, p2 };
    float sx[3], sy[3], sz[3], iw[3];
    int i, k, x, y, x0, x1, y0, y1;
    float area;
    int cx0 = st->sx, cy0 = st->sy, cx1 = st->sx + st->sw, cy1 = st->sy + st->sh;

    if (s_wide) {                         /* N64 pixels onto the whole width or the 4:3 middle */
        if (st->wide) {
            cx0 = cx0 * W / PS3_SCREEN_W;
            cx1 = cx1 * W / PS3_SCREEN_W;
        } else {
            int sh = st->anchor < 0 ? 0 : st->anchor > 0 ? W - PS3_SCREEN_W : (W - PS3_SCREEN_W) / 2;
            cx0 += sh;
            cx1 += sh;
            if (st->fullsc) {             /* 2D that may go into the side bars */
                cx0 = 0;
                cx1 = W;
            }
        }
    }

    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > W) cx1 = W;
    if (cy1 > H) cy1 = H;

    for (i = 0; i < 3; i++) {
        iw[i] = 1.0f / p[i]->w;
        sx[i] = (p[i]->x * iw[i] + 1.0f) * (W / 2.0f);
        sy[i] = (1.0f - p[i]->y * iw[i]) * (H / 2.0f);
        sz[i] = p[i]->z * iw[i] * 0.5f + 0.5f;
    }
    area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-6f) return;

    x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2])));
    x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
    y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2])));
    y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f, b[3], z, q, attr[NA], col[4];
            b[0] = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
            b[1] = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
            b[2] = 1.0f - b[0] - b[1];
            if (b[0] < 0 || b[1] < 0 || b[2] < 0) continue;
            z = b[0] * sz[0] + b[1] * sz[1] + b[2] * sz[2];
            if (st->decal) z -= 0.0005f;
            if (st->depth_test && (st->decal ? z > s_depth[y][x] : z >= s_depth[y][x])) continue;
            q = b[0] * iw[0] + b[1] * iw[1] + b[2] * iw[2];
            for (k = 0; k < NA; k++)
                attr[k] = (b[0] * p[0]->a[k] * iw[0] + b[1] * p[1]->a[k] * iw[1] + b[2] * p[2]->a[k] * iw[2]) / q;
            shade(st, attr, col);
            if (st->alpha_ref && col[3] * 255.0f < st->alpha_ref) continue;
            if (st->depth_write) s_depth[y][x] = z;
            s_written++;
            for (k = 0; k < 3; k++)
                s_color[y][x][k] = st->blend ? col[k] * col[3] + s_color[y][x][k] * (1 - col[3]) : col[k];
        }
    }
}

/* Clips against the near plane (z > -w) and against w > 0. */
void PS3_Rsx_Draw(const ps3_rstate_t *st, const ps3_rvert_t *v, int n)
{
    int t, i;

    if (!s_want) return;
    s_written = 0;
    for (t = 0; t + 2 < n; t += 3) {
        cv_t in[3], poly[8];
        int np = 0;

        for (i = 0; i < 3; i++) to_cv(&in[i], &v[t + i]);
        for (i = 0; i < 3; i++) {
            const cv_t *a = &in[i], *b = &in[(i + 1) % 3];
            float da = a->z + a->w, db = b->z + b->w;
            if (da >= 0) poly[np++] = *a;
            if ((da >= 0) != (db >= 0)) lerp_cv(&poly[np++], a, b, da / (da - db));
        }
        for (i = 1; i + 1 < np; i++) {
            if (poly[0].w <= 0 || poly[i].w <= 0 || poly[i + 1].w <= 0) continue;
            raster(st, &poly[0], &poly[i], &poly[i + 1]);
        }
    }
    if (s_written > 4000 && getenv("MK64_DRAWLOG")) {
        const ps3_rvert_t *q = &v[0];
        printf("[draw] %ld px, %d verts, tex %dx%d, blend %d zt %d zw %d decal %d aref %d | v0 clip %.1f %.1f %.1f %.1f uv %.3f %.3f"
               " A %d,%d,%d,%d B %d,%d,%d,%d C %d,%d,%d,%d D %d,%d,%d,%d w %d%d%d%d%d%d%d%d wa %d%d%d%d fog %d\n",
               s_written, n, st->tex ? st->tex->w : 0, st->tex ? st->tex->h : 0, st->blend, st->depth_test,
               st->depth_write, st->decal, st->alpha_ref, q->x, q->y, q->z, q->w, q->u, q->v,
               q->c[0][0], q->c[0][1], q->c[0][2], q->c[0][3], q->c[1][0], q->c[1][1], q->c[1][2], q->c[1][3],
               q->c[2][0], q->c[2][1], q->c[2][2], q->c[2][3], q->c[3][0], q->c[3][1], q->c[3][2], q->c[3][3],
               q->wrgb[0] > 0, q->wrgb[1] > 0, q->wrgb[2] > 0, q->wrgb[3] > 0, q->wrgb[4] > 0, q->wrgb[5] > 0,
               q->wrgb[6] > 0, q->wrgb[7] > 0, q->wa[0] > 0, q->wa[1] > 0, q->wa[2] > 0, q->wa[3] > 0, q->fog[3]);
    }
}

/* Diagnostics of ps3_rsx.h: not used by the host test. */
void PS3_Rsx_SetDebug(int mode) { (void)mode; }
int  PS3_Rsx_GetDebug(void) { return 0; }
const char *PS3_Rsx_DebugName(int mode) { (void)mode; return "normal"; }
void PS3_Rsx_LogNextFrame(void) {}
