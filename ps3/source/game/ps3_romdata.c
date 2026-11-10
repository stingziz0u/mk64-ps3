/* ps3_romdata.c -- data of the N64 "main" segment that the game source does
 * not contain (the decompilation extracts it from the ROM at build time).
 *
 * The native array is left empty and filled here from the user's ROM. The main
 * segment is loaded at 0x80000400 from ROM offset 0x1000, so the ROM offset of
 * a main-segment address is addr - 0x80000400 + 0x1000.
 */

#include <ultra64.h>

#include "ps3_game.h"
#include "ps3_rom.h"

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
}
