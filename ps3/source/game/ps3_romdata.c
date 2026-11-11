/* ps3_romdata.c -- data of the N64 "main" segment that the game source does
 * not contain (the decompilation extracts it from the ROM at build time).
 *
 * The native array is left empty and filled here from the user's ROM. The main
 * segment is loaded at 0x80000400 from ROM offset 0x1000, so the ROM offset of
 * a main-segment address is addr - 0x80000400 + 0x1000.
 */

#include <ultra64.h>

#include <string.h>

#include "ps3_game.h"
#include "ps3_rom.h"
#include "ps3_romdata_map.h"

#define MAIN_ROM(vaddr) ((u32) ((vaddr) - 0x80000400U + 0x1000U))

extern u8 D_800E52D0[];                   /* src/data/some_data.c: gTLUTOnomatopoeia */

static const struct {
    const char* name;
    u8* dest;
    u32 vaddr;
    u32 size;
} sRomData[] = {
    { "gTLUTOnomatopoeia", D_800E52D0, 0x800E52D0, 0x200 },
};

/* What a segmented address resolves to while its segment has no base yet: on
 * the N64 the base is 0, so the address lands in the first 64 KB of RAM,
 * 0x400 bytes of system area and then the start of the game code. The game
 * relies on it: the static over the course pictures of the cup selection
 * screen is drawn from 0x0B002A00 with segment 0x0B unset, i.e. from its own
 * code used as a texture. Same bytes here, from the ROM. */
u8 gPs3LowRam[0x10000];

void ps3_romdata_load(void) {
    u32 i;

    for (i = 0; i < sizeof(sRomData) / sizeof(sRomData[0]); i++) {
        if (PS3_Rom_Read(sRomData[i].dest, MAIN_ROM(sRomData[i].vaddr), sRomData[i].size) != 0) {
            ps3_logf("[romdata] could not read %s", sRomData[i].name);
        }
    }
    PS3_Rom_Read(gPs3LowRam + 0x400, 0x1000, sizeof(gPs3LowRam) - 0x400);
    ps3_logf("[romdata] %u block(s) read from the ROM", (unsigned) i);
    ps3_romdata_mods();
}

/* ---- mods -------------------------------------------------------------------
 * The N64 runs its data tables from the ROM: where each kart sprite and palette
 * is, the course segments, character stats, menu textures... Mods change them.
 * The port has them compiled in, with the stock values, so with a modified ROM
 * each table of gen/romdata_map.c is rebuilt from it. With the stock ROM they
 * are the same (the map is checked against it), so it is only done for others.
 * N64 RAM addresses in a table are left as they are: the PS3 ones are there. */
void ps3_build_glyph_lut(void);

void ps3_romdata_mods(void) {
    static u8 buf[0x4000];
    int i, j, changed = 0;
    u32 k;

    if (PS3_Rom_IsStock()) {
        return;
    }
    for (i = 0; i < gPs3RomDataMapCount; i++) {
        const ps3_romdata_entry* e = &gPs3RomDataMap[i];
        int diff = 0;

        if (e->size > sizeof(buf) || PS3_Rom_Read(buf, e->romOff, e->size) != 0) {
            ps3_logf("[mods] %s: could not read it, stock values kept", e->name);
            continue;
        }
        for (j = 0; j < e->nops; j++) {
            const ps3_romdata_op* op = &e->ops[j];

            if (!op->ptr) {
                if (memcmp(e->dest + op->ps3Off, buf + op->n64Off, op->n) != 0) {
                    memcpy(e->dest + op->ps3Off, buf + op->n64Off, op->n);
                    diff = 1;
                }
                continue;
            }
            for (k = 0; k < op->n; k++) {
                const u8* w = buf + op->n64Off + 4 * k;
                u32 v = ((u32) w[0] << 24) | ((u32) w[1] << 16) | ((u32) w[2] << 8) | w[3];
                u64* p = (u64*) (e->dest + op->ps3Off + 8 * k);

                if ((v >> 24) == 0x80) {
                    continue;       /* a RAM address of the N64: not one of ours */
                }
                if (*p != (u64) v) {
                    *p = (u64) v;
                    diff = 1;
                }
            }
        }
        if (diff) {
            ps3_logf("[mods] %s taken from the ROM (differs from stock)", e->name);
            changed++;
        }
    }
    ps3_build_glyph_lut();      /* a copy of some of those tables */
    ps3_logf("[mods] %d of %d data tables differ from the stock ROM", changed, gPs3RomDataMapCount);
}
