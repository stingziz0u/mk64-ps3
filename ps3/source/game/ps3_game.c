/* ps3_game.c -- glue between the platform side and the game.
 *
 * Stage 2: the game runs and its display lists are drawn on the RSX
 * (ps3_gfx.c). What the interpreter counted goes to the log for the first
 * frames, on every game state change and once every 10 seconds.
 */

#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "code_800029B0.h"
#include "menus.h"
#include "course.h"
#include "ps3_game.h"
#include "ps3_gfx_walk.h"
#include "ps3_plat.h"
#include "ps3_pad_bits.h"
#include "ps3_rsx.h"

#define VBLANK_US 16683ULL                /* 59.94 Hz */

extern u8 _memoryPoolSegmentNoloadStart[];
extern u8 _racingSegmentStart[];

int gPs3DebugMenu;

void ps3_stage(const char* what) {
    ps3_logf("[game] %s", what);
}

static void log_frame(const char* why) {
    const ps3_gfx_stats_t* s = &gPs3GfxStats;

    ps3_logf("[frame %u] %s: state %d, menu %d, course %d, timer %d | cmds %u, dl %u (depth %u), vtx %u in %u loads, tris %u, "
             "mtx %u, tex %u, texrect %u, fill %u, notex %u, culled %u/%u, errors %u | heap %lu KB used, %lu KB free",
             s->frame, why, (int) gGamestate, (int) gMenuSelection, (int) gCurrentCourseId, (int) gGlobalTimer, s->commands, s->dlCalls, s->maxDepth,
             s->vertices, s->vtxLoads, s->triangles, s->matrices, s->textures, s->texRects, s->fillRects,
             s->texMissing, s->culledClip, s->culledBack, s->errors, (unsigned long) ((gNextFreeMemoryAddress - (uintptr_t) _memoryPoolSegmentNoloadStart) / 1024),
             (unsigned long) ((gHeapEndPtr - gNextFreeMemoryAddress) / 1024));
}

int gPs3HostFirstPlace;   /* host test: the player's finishing place counts as first */

/* ---- the port's options --------------------------------------------------
 * mk64.cfg, one "key=value" per line. Written whenever an option changes. */
#define CONFIG_FILE "mk64.cfg"

/* ---- button bindings: bind_<n64 button>=<ps3 buttons joined by +> ----------
 * The same for every player. The sticks are fixed: left stick is the N64
 * stick, right stick the C buttons (on top of their bindings). */
static const struct { const char* name; u16 mask; } sPs3Buttons[] = {
    { "CROSS", PS3PAD_CROSS },   { "CIRCLE", PS3PAD_CIRCLE }, { "SQUARE", PS3PAD_SQUARE },
    { "TRIANGLE", PS3PAD_TRIANGLE }, { "L1", PS3PAD_L1 },     { "L2", PS3PAD_L2 },
    { "L3", PS3PAD_L3 },         { "R1", PS3PAD_R1 },         { "R2", PS3PAD_R2 },
    { "R3", PS3PAD_R3 },         { "SELECT", PS3PAD_SELECT }, { "START", PS3PAD_START },
    { "UP", PS3PAD_UP },         { "DOWN", PS3PAD_DOWN },     { "LEFT", PS3PAD_LEFT },
    { "RIGHT", PS3PAD_RIGHT },
};
#define NBINDS 14
static const struct { const char* key; u16 n64; u16 def; } sBinds[NBINDS] = {
    { "a", A_BUTTON, PS3PAD_CROSS },
    { "b", B_BUTTON, PS3PAD_SQUARE },
    { "z", Z_TRIG, PS3PAD_L1 | PS3PAD_L2 },
    { "r", R_TRIG, PS3PAD_R1 | PS3PAD_R2 },
    { "l", L_TRIG, PS3PAD_SELECT },
    { "start", START_BUTTON, PS3PAD_START },
    { "c_up", U_CBUTTONS, PS3PAD_TRIANGLE },
    { "c_down", D_CBUTTONS, 0 },
    { "c_left", L_CBUTTONS, 0 },
    { "c_right", R_CBUTTONS, PS3PAD_CIRCLE },
    { "dpad_up", U_JPAD, PS3PAD_UP },
    { "dpad_down", D_JPAD, PS3PAD_DOWN },
    { "dpad_left", L_JPAD, PS3PAD_LEFT },
    { "dpad_right", R_JPAD, PS3PAD_RIGHT },
};
static u16 sBind[NBINDS];

u16 ps3_map_buttons(u16 ps3) {
    u16 b = 0;
    int k;

    for (k = 0; k < NBINDS; k++) {
        if (ps3 & sBind[k]) b |= sBinds[k].n64;
    }
    return b;
}

static int bind_text(char* out, int size, u16 mask) {
    int i, n = 0;

    out[0] = 0;
    for (i = 0; i < (int) (sizeof(sPs3Buttons) / sizeof(sPs3Buttons[0])); i++) {
        if (mask & sPs3Buttons[i].mask) {
            n += snprintf(out + n, size - n, "%s%s", n ? "+" : "", sPs3Buttons[i].name);
            if (n >= size) return size - 1;
        }
    }
    return n;
}

/* "L1+L2", "cross", " R1 , R2 ": names joined by + or ,; unknown names are
 * skipped (and logged). */
static u16 bind_parse(const char* s) {
    u16 mask = 0;

    while (*s) {
        char name[16];
        int i, n = 0;

        while (*s == ' ' || *s == '+' || *s == ',' || *s == '\t' || *s == '\r') s++;
        while (*s && *s != ' ' && *s != '+' && *s != ',' && *s != '\t' && *s != '\r') {
            if (n < 15) name[n++] = (char) ((*s >= 'a' && *s <= 'z') ? *s - 32 : *s);
            s++;
        }
        name[n] = 0;
        if (n == 0) continue;
        for (i = 0; i < (int) (sizeof(sPs3Buttons) / sizeof(sPs3Buttons[0])); i++) {
            if (strcmp(name, sPs3Buttons[i].name) == 0) break;
        }
        if (i < (int) (sizeof(sPs3Buttons) / sizeof(sPs3Buttons[0]))) mask |= sPs3Buttons[i].mask;
        else ps3_logf("[config] unknown button '%s'", name);
    }
    return mask;
}

static void config_save(void) {
    char buf[1024];
    int k, n = snprintf(buf, sizeof(buf), "aspect=%s\nfps=%d\nfpscounter=%d\nscreenfit=%d\n", gPs3Wide ? "16:9" : "4:3",
                     gPs3Fps60 ? 60 : 30, gPs3FpsCounter, gPs3ScreenFit);

    for (k = 0; k < NBINDS; k++) {
        char t[96];

        bind_text(t, sizeof(t), sBind[k]);
        n += snprintf(buf + n, sizeof(buf) - n, "bind_%s=%s\n", sBinds[k].key, t);
    }

    if (PS3_Plat_SaveFile(CONFIG_FILE, buf, n) < 0) {
        ps3_logf("[config] could not write %s", CONFIG_FILE);
    }
}

void ps3_config_load(void) {
    char buf[2048];
    int n = PS3_Plat_LoadFile(CONFIG_FILE, buf, sizeof(buf) - 1);
    int k, seen = 0;

    for (k = 0; k < NBINDS; k++) sBind[k] = sBinds[k].def;

    gPs3Wide = 0;                           /* defaults: as the original */
    gPs3Fps60 = 0;
    gPs3FpsCounter = 0;
    gPs3ScreenFit = 100;
    if (n > 0) {
        char* line = buf;

        buf[n] = 0;
        while (line != NULL && *line) {
            char* next = strchr(line, '\n');

            if (next) *next++ = 0;
            if (strncmp(line, "aspect=", 7) == 0) gPs3Wide = strncmp(line + 7, "16:9", 4) == 0;
            if (strncmp(line, "fps=", 4) == 0) gPs3Fps60 = atoi(line + 4) == 60;
            if (strncmp(line, "fpscounter=", 11) == 0) gPs3FpsCounter = atoi(line + 11) != 0;
            if (strncmp(line, "bind_", 5) == 0) {
                char* eq = strchr(line, '=');

                for (k = 0; eq && k < NBINDS; k++) {
                    if ((int) strlen(sBinds[k].key) == eq - line - 5 &&
                        strncmp(line + 5, sBinds[k].key, eq - line - 5) == 0) {
                        sBind[k] = bind_parse(eq + 1);
                        seen |= 1 << k;
                        break;
                    }
                }
            }
            if (strncmp(line, "screenfit=", 10) == 0) {
                int f = atoi(line + 10);

                gPs3ScreenFit = f >= 85 && f <= 100 ? f - f % 5 : 100;
            }
            line = next;
        }
    }
    ps3_logf("[config] %s: aspect %s, %d fps, FPS counter %s, screen fit %d%%",
             n > 0 ? CONFIG_FILE : "no " CONFIG_FILE ", defaults", gPs3Wide ? "16:9" : "4:3", gPs3Fps60 ? 60 : 30,
             gPs3FpsCounter ? "on" : "off", gPs3ScreenFit);
    for (k = 0; k < NBINDS; k++) {
        char t[96];

        bind_text(t, sizeof(t), sBind[k]);
        ps3_logf("[config] N64 %s = %s", sBinds[k].key, t[0] ? t : "(none)");
    }
    if (seen != (1 << NBINDS) - 1) {
        config_save();                      /* an older mk64.cfg: write the bind_ lines out */
    }
}

void ps3_option_toggle(int which) {
    switch (which) {
        case 0: gPs3Wide = !gPs3Wide; break;
        case 1: gPs3Fps60 = !gPs3Fps60; break;
        case 2: gPs3FpsCounter = !gPs3FpsCounter; break;
        case 3: gPs3ScreenFit = gPs3ScreenFit <= 85 ? 100 : gPs3ScreenFit - 5; break;   /* 100 95 90 85 */
        default: return;
    }
    ps3_logf("[config] aspect %s, %d fps, FPS counter %s, screen fit %d%%", gPs3Wide ? "16:9" : "4:3",
             gPs3Fps60 ? 60 : 30, gPs3FpsCounter ? "on" : "off", gPs3ScreenFit);
    config_save();
}

const char* ps3_option_text(int which) {
    switch (which) {
        case 0: return gPs3Wide ? "16X9" : "4X3";       /* the menu font has no colon */
        case 1: return gPs3Fps60 ? "60 FPS" : "30 FPS";
        case 2: return gPs3FpsCounter ? "ON" : "OFF";
        case 3: {
            static char fit[8];

            snprintf(fit, sizeof(fit), "%d", gPs3ScreenFit);    /* the font has no percent sign */
            return fit;
        }
    }
    return "";
}

/* Every 10 s: where the time of a frame went (ms per game frame), and how
 * much work the frame was. Timebase ticks are turned into time with the
 * timebase rate measured over the same 10 s. */
static void log_prof(uint64_t nowUs) {
    static uint64_t lastUs, lastTb;
    ps3_prof_t* p = &gPs3Prof;
    uint64_t tb = ps3_tb();
    double msPerTick, f;

    if (lastUs == 0) {
        lastUs = nowUs;
        lastTb = tb;
        memset(p, 0, sizeof(*p));
        return;
    }
    if (nowUs - lastUs < 10000000ULL || p->frames == 0 || tb == lastTb) {
        return;
    }
    msPerTick = (double) (nowUs - lastUs) / 1000.0 / (double) (tb - lastTb);
    f = msPerTick / (double) p->frames;
#define MS(k) (double) p->t[k] * f
    ps3_logf("[perf] course %d, %u frames: ms per frame: logic %.2f audio %.2f | draw %.2f (in-between %.2f): "
             "vertices %.2f, triangles %.2f, of which texture hashing %.2f, new textures %.2f, rects %.2f, "
             "to the RSX %.2f",
             (int) gCurrentCourseId, p->frames, MS(PROF_LOGIC), MS(PROF_AUDIO), MS(PROF_PASS), MS(PROF_PASS_INTERP),
             MS(PROF_VTX), MS(PROF_TRI), MS(PROF_TEX_HASH), MS(PROF_TEX_NEW), MS(PROF_RECT), MS(PROF_DRAW));
#undef MS
    ps3_logf("[perf] per frame (both passes): %u vertices, %u triangles, %u batches, %u texture lookups (%u KB hashed), "
             "%u new textures",
             p->verts / p->frames, p->tris / p->frames, p->draws / p->frames, p->texLookups / p->frames,
             p->texBytes / p->frames / 1024, p->texNew / p->frames);
    ps3_logf("[perf] slowest frame: %.2f ms (logic %.2f, draw %.2f, audio %.2f), %u vertices, %u triangles",
             (double) p->worst * msPerTick, (double) p->worstLogic * msPerTick, (double) p->worstDraw * msPerTick,
             (double) p->worstAudio * msPerTick, p->worstVerts, p->worstTris);
    memset(p, 0, sizeof(*p));
    lastUs = nowUs;
    lastTb = tb;
}

/* Every 10 s in 60 fps mode: how many in-between frames made it. */
static void log_interp(void) {
    static uint64_t last;
    ps3_interp_stats_t* st = &gPs3InterpStats;
    uint64_t now = PS3_Plat_TimeUs();

    if (last == 0) {
        last = now;
    }
    if (now - last < 10000000ULL) {
        return;
    }
    last = now;
    if (gPs3Fps60) {
        ps3_logf("[60fps] last 10 s: %u in-between frames, %u frames with no time for one; matrices %u paired, %u not; "
                 "a frame takes %u.%u ms to draw",
                 st->inbetween, st->skipped, st->paired, st->unpaired, st->lastPassUs / 1000, (st->lastPassUs / 100) % 10);
    }
    memset(st, 0, sizeof(*st));
}

void ps3_frame_end(void) {
    static uint64_t tbExit, passAtExit;
    static uint64_t fLogic, fPass, fAudio, fTris, fVerts;  /* this frame, for the worst one */
    static uint64_t frameStart;
    static s32 lastState = -1000;
    static s32 lastMenu = -1000;
    static uint64_t lastLog;
    uint64_t now, due;
    ps3_padstate_t pad;
    int vblanks, i;

    {
        /* the game's own time since the last swap, without drawing done in it */
        uint64_t tb = ps3_tb();

        if (tbExit != 0) {
            fLogic = (tb - tbExit) - (gPs3Prof.t[PROF_PASS] - passAtExit);
            gPs3Prof.t[PROF_LOGIC] += fLogic;
        }
        gPs3Prof.frames++;
    }
    if (gPs3GfxStats.frame <= 5) {
        log_frame("first frames");
    } else if (gGamestate != lastState || gMenuSelection != lastMenu) {
        log_frame("game state changed");
    } else if (PS3_Plat_TimeUs() - lastLog > 10000000ULL) {
        log_frame("every 10 s");
        lastLog = PS3_Plat_TimeUs();
    }
    lastState = gGamestate;
    lastMenu = gMenuSelection;

    PS3_Plat_Pump();
    PS3_Plat_Pad(0, &pad);
    {
        /* Diagnostics: R3 cycles the render mode, L3 logs the next frame. */
        static unsigned prev;
        unsigned pressed = pad.buttons & ~prev;

        prev = pad.buttons;
        if (pressed & PS3PAD_R3) {
            int m = (PS3_Rsx_GetDebug() + 1) % PS3_RDBG_COUNT;

            PS3_Rsx_SetDebug(m);
            ps3_logf("[debug] frame %u: render mode %d, \"%s\"", gPs3GfxStats.frame, m, PS3_Rsx_DebugName(m));
        }
        if (pressed & PS3PAD_L3) {
            ps3_logf("[debug] frame %u: L3, logging the draws of the next frame (state %d, menu %d)",
                     gPs3GfxStats.frame, (int) gGamestate, (int) gMenuSelection);
            log_frame("L3");
            PS3_Rsx_LogNextFrame();
        }
    }
    if (PS3_Plat_ExitRequested() ||
        ((pad.buttons & PS3PAD_SELECT) && (pad.buttons & PS3PAD_START))) {
        ps3_logf("[game] quitting at frame %u", gPs3GfxStats.frame);
        PS3_Plat_Exit();
    }

    /* The game runs at 30 fps: a frame lasts two vblanks, or more if it took
     * longer. The game counts the vblanks that went by to set its tick speed. */
    now = PS3_Plat_TimeUs();
    if (frameStart == 0 || now - frameStart > 30 * VBLANK_US) {
        frameStart = now - 2 * VBLANK_US;   /* first frame, or a long stall (loading) */
    }

    /* 60 fps: the in-between frame goes out now and the game's frame one
     * vblank later. Only if both fit in the frame's two vblanks; otherwise
     * this frame is shown alone, as at 30 fps. */
    if (gPs3Fps60 && ps3_gfx_pending()) {
        static uint64_t passUs = 8000;      /* time to draw a frame, smoothed */
        uint64_t busy = now - frameStart;

        if (busy + passUs + 1500 < VBLANK_US) {
            ps3_gfx_draw_inbetween(0.5f);
            now = PS3_Plat_TimeUs();
            if (frameStart + VBLANK_US > now) {
                PS3_Plat_SleepUs(frameStart + VBLANK_US - now);
            }
        } else {
            gPs3InterpStats.skipped++;
        }
        ps3_gfx_end_frame();
        passUs = (passUs * 7 + gPs3InterpStats.lastPassUs) / 8;
        now = PS3_Plat_TimeUs();
    } else {
        ps3_gfx_end_frame();
    }
    log_interp();
    {
        static uint64_t since;
        static u32 shown;

        shown += gPs3InterpStats.inbetweenNow + 1;
        gPs3InterpStats.inbetweenNow = 0;
        if (since == 0) {
            since = now;
        }
        if (now - since >= 1000000ULL) {
            gPs3FpsShown = (u32) ((shown * 1000000ULL + (now - since) / 2) / (now - since));
            shown = 0;
            since = now;
        }
    }
    vblanks = (int) ((now - frameStart + VBLANK_US - 1) / VBLANK_US);
    if (vblanks < 2) {
        vblanks = 2;
    }
    due = frameStart + (uint64_t) vblanks * VBLANK_US;
    if (due > now) {
        PS3_Plat_SleepUs(due - now);
    }
    frameStart = due;
    for (i = 0; i < vblanks; i++) {
        ps3_vblank();
    }
    {
        /* the slowest frame: logic, both passes, audio */
        uint64_t pass = gPs3Prof.t[PROF_PASS] + gPs3Prof.t[PROF_PASS_INTERP];
        uint64_t total;

        total = fLogic + (pass - fPass) + (gPs3Prof.t[PROF_AUDIO] - fAudio);
        if (total > gPs3Prof.worst) {
            gPs3Prof.worst = total;
            gPs3Prof.worstLogic = fLogic;
            gPs3Prof.worstDraw = pass - fPass;
            gPs3Prof.worstAudio = gPs3Prof.t[PROF_AUDIO] - fAudio;
            gPs3Prof.worstTris = (u32) (gPs3Prof.tris - fTris);
            gPs3Prof.worstVerts = (u32) (gPs3Prof.verts - fVerts);
        }
    }
    log_prof(PS3_Plat_TimeUs());
    tbExit = ps3_tb();
    passAtExit = gPs3Prof.t[PROF_PASS];
    fPass = gPs3Prof.t[PROF_PASS] + gPs3Prof.t[PROF_PASS_INTERP];
    fAudio = gPs3Prof.t[PROF_AUDIO];
    fTris = gPs3Prof.tris;
    fVerts = gPs3Prof.verts;
}

void PS3_Game_Run(void) {
    ps3_logf("[game] sizeof: Gfx %u, Vtx %u, Mtx %u, void* %u", (unsigned) sizeof(Gfx), (unsigned) sizeof(Vtx),
             (unsigned) sizeof(Mtx), (unsigned) sizeof(void*));
    ps3_logf("[game] heap %p .. %p (%lu KB)", (void*) _memoryPoolSegmentNoloadStart, (void*) _racingSegmentStart,
             (unsigned long) ((_racingSegmentStart - _memoryPoolSegmentNoloadStart) / 1024));
    {
        char c;
        gPs3DebugMenu = PS3_Plat_LoadFile("debugmenu.txt", &c, 1) >= 0;
        ps3_logf("[game] debug menu (debugmenu.txt): %s", gPs3DebugMenu ? "ON" : "off");
        ps3_config_load();
    }
    ps3_romdata_load();
    ps3_game_main();
}
