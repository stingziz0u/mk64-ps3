/* ps3_decode.c -- the decompressors that are hand-written MIPS assembly in
 * the original (asm/mio0_decode.s, asm/tkmk00_decode.s). */

#include <ultra64.h>
#include <string.h>

#include "ps3_game.h"

/* tools/libtkmk00.c, the C version of asm/tkmk00_decode.s. It reads and
 * writes through explicit big-endian accessors, so its output is what the
 * game expects on any host. */
void tkmk00_decode(u8* tkmk, u8* tmp_buf, u8* rgba16, s32 alpha_color);

#define BE32(p) (((u32) (p)[0] << 24) | ((u32) (p)[1] << 16) | ((u32) (p)[2] << 8) | (u32) (p)[3])

/**
 * MIO0: "MIO0", decoded size, offset of the compressed stream, offset of the
 * raw stream, then one bit per output chunk (1 = a raw byte, 0 = a 16-bit
 * back-reference: 4 bits of length - 3, 12 bits of distance - 1).
 */
void mio0decode(u8* in, u8* out) {
    u32 size, written = 0;
    const u8* comp;
    const u8* raw;
    const u8* bits = in + 16;
    u32 mask = 0, cur = 0;

    if (memcmp(in, "MIO0", 4) != 0) {
        ps3_logf("[mio0] BAD header at %p: %02X %02X %02X %02X", (void*) in, in[0], in[1], in[2], in[3]);
        return;
    }
    size = BE32(in + 4);
    comp = in + BE32(in + 8);
    raw = in + BE32(in + 12);

    while (written < size) {
        if (mask == 0) {
            cur = *bits++;
            mask = 0x80;
        }
        if (cur & mask) {
            out[written++] = *raw++;
        } else {
            u32 len = (comp[0] >> 4) + 3;
            u32 dist = (((u32) comp[0] & 0x0F) << 8) + comp[1] + 1;

            comp += 2;
            while (len-- != 0 && written < size) {
                out[written] = out[written - dist];
                written++;
            }
        }
        mask >>= 1;
    }
}

void tkmk00decode(u32* in, u8* tmp, u16* out, s32 alphaColor) {
    tkmk00_decode((u8*) in, tmp, (u8*) out, alphaColor);
}

/* Only called from functions the game never calls. */
void func_80040030(u8* in, u8* out) {
    (void) in; (void) out;
    ps3_log("[decode] func_80040030 (unused MIO0 variant) was called");
}

/* Ghost compression for the Controller Pak (src/replays.c). There is no
 * Controller Pak yet, so nothing can ask for these. */
s32 func_80040174(void* in, s32 size, s32 out) {
    (void) in; (void) size; (void) out;
    ps3_log("[decode] func_80040174 (ghost pre-encode) not implemented");
    return 0;
}

s32 mio0encode(s32 in, s32 size, s32 out) {
    (void) in; (void) size; (void) out;
    ps3_log("[decode] mio0encode not implemented");
    return 0;
}
