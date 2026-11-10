/* ps3_game.h -- PS3 hooks inside the game code (included from src/main.h when
 * TARGET_PS3 is defined). No PSL1GHT headers here: see ps3_plat.h. */

#ifndef PS3_GAME_H
#define PS3_GAME_H

#include "ps3_log.h"

/* src/main.c */
void ps3_game_main(void);                 /* does not return */
void ps3_vblank(void);                    /* one 60 Hz tick */

/* src/racing/memory.c */
void ps3_init_segment_table(void);

/* src/audio/load.c */
void audio_init(void);

/* source/game/ps3_game.c */
extern int gPs3DebugMenu;                 /* debugmenu.txt present: see src/menus.c */
void ps3_stage(const char* what);         /* "[game] what" in the log */
void ps3_frame_end(void);                 /* called from osViSwapBuffer */

/* source/game/ps3_gfx_walk.c: the display list of a frame (an OSTask*). */
void ps3_gfx_run_task(void* task);
void ps3_gfx_end_frame(void);            /* shows the frame (osViSwapBuffer) */

/* 60 fps mode (ps3_gfx.c): the game's frame is drawn at the swap, after an
 * in-between frame with interpolated matrices. */
typedef struct {
    u32 inbetween, skipped;               /* in-between frames drawn / not drawn (no time) */
    u32 paired, unpaired;                 /* matrices interpolated / drawn as they are */
    u32 drawnUs;                          /* time spent drawing in-between frames */
    u32 lastPassUs;                       /* last full pass over a display list */
    u32 inbetweenNow;                     /* in-between frames shown since the last swap */
} ps3_interp_stats_t;
extern int gPs3Fps60;
extern int gPs3Wide;                      /* 16:9: 3D across the whole screen, 2D in the 4:3 middle */
extern ps3_interp_stats_t gPs3InterpStats;
extern int gPs3FpsCounter;                /* frames shown per second, top left */
extern int gPs3ScreenFit;                 /* screen fit, percent (overscan) */
extern u32 gPs3FpsShown;                  /* what it shows */
int ps3_gfx_pending(void);

/* Profiling, in timebase ticks (mftb): where a frame's time goes. Summed
 * over ps3_frame_end's 10 s window and logged there. */
enum {
    PROF_LOGIC,          /* the game, between two swaps (not drawing at 30 fps) */
    PROF_AUDIO,          /* the audio frames (synthesis) */
    PROF_PASS,           /* a full pass over the game's display list */
    PROF_PASS_INTERP,    /* the same, for the in-between frame */
    PROF_VTX,            /* vertex loads: transform, lighting, fog */
    PROF_TRI,            /* triangles, their texture lookups included */
    PROF_TEX_HASH,       /* texture lookups: hashing the contents */
    PROF_TEX_NEW,        /* new textures: conversion and upload */
    PROF_RECT,           /* rectangles */
    PROF_DRAW,           /* handing batches to the RSX backend */
    PROF_COUNT
};
typedef struct {
    u64 t[PROF_COUNT];
    u32 texLookups, texBytes, texNew, draws, verts, tris, frames;
    u64 worst, worstLogic, worstDraw, worstAudio;   /* the slowest frame of the window */
    u32 worstTris, worstVerts;
} ps3_prof_t;
extern ps3_prof_t gPs3Prof;
static inline u64 ps3_tb(void) {
    u64 t;
    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}

/* 16:9: the 1P race HUD is moved against the screen's edges. The game marks
 * where each element starts with a no-op the interpreter reads (it draws
 * later, from the display list). */
#define PS3_HUD_TAG_BASE 0x50534800u          /* "PSH" + the anchor: 0xFF left, 0 middle, 1 right */
#define PS3_HUD_LEFT (-1)
#define PS3_HUD_MIDDLE 0
#define PS3_HUD_RIGHT 1
#define PS3_HUD_ANCHOR(a)                                                                           \
    do {                                                                                            \
        if (gScreenModeSelection == SCREEN_MODE_1P) {                                               \
            gDPNoOpTag(gDisplayListHead++, PS3_HUD_TAG_BASE | ((u32) (a) & 0xFF));                  \
        }                                                                                           \
    } while (0)

/* The port's options (Options menu), saved in mk64.cfg in USRDIR:
 * 0 aspect ratio, 1 frame rate, 2 FPS counter, 3 screen fit. */
void ps3_config_load(void);
u16 ps3_map_buttons(u16 ps3);   /* PS3 buttons -> N64 buttons, from the bind_ lines */
void ps3_option_toggle(int which);
const char* ps3_option_text(int which);
void ps3_gfx_draw_inbetween(float t);

/* source/game/ps3_audio_rsp.c: the audio microcode. */
void ps3_audio_run_task(void* task);
void ps3_audio_tick(void);                /* one 60 Hz tick of the audio thread */

/* source/game/ps3_romdata.c: main-segment data that is not in the source. */
void ps3_romdata_load(void);
extern unsigned char gPs3LowRam[0x10000];

#endif /* PS3_GAME_H */
