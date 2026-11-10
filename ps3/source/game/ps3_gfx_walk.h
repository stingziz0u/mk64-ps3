/* ps3_gfx_walk.h -- what the display list interpreter counted in the last frame. */

#ifndef PS3_GFX_WALK_H
#define PS3_GFX_WALK_H

typedef struct {
    unsigned frame;                       /* frames walked so far */
    unsigned commands;
    unsigned dlCalls, maxDepth;
    unsigned vtxLoads, vertices, triangles;
    unsigned matrices, textures;
    unsigned texRects, fillRects;
    unsigned culledClip, culledBack;      /* triangles dropped */
    unsigned texMissing;                  /* texel wanted, nothing loaded */
    unsigned errors;                      /* this frame */
    unsigned totalErrors;                 /* since boot */
} ps3_gfx_stats_t;

extern ps3_gfx_stats_t gPs3GfxStats;

#endif /* PS3_GFX_WALK_H */
