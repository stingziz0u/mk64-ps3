/* ps3_audio_rsp.c -- what the RSP audio microcode did: runs the command list
 * that the game's synthesis code (src/audio/synthesis.c) builds every audio
 * frame, and hands the resulting samples to the platform.
 *
 * Mario Kart 64 (USA) uses the same command set as the European Super Mario
 * 64: ADPCM decoding, resampling, the envelope mixer with its two set-up
 * commands, buffer loads and saves between DMEM and RAM, mixing and the final
 * interleave. DMEM is a plain array here. The N64 is big-endian like the PS3,
 * so the sample data and the ADPCM codebooks are used as they are.
 *
 * Called from the game thread once per 60 Hz tick (ps3_audio_tick), like the
 * N64's audio thread ran once per vertical blank.
 */

#include <ultra64.h>
#include <PR/abi.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "main.h"
#include "ps3_game.h"
#include "ps3_plat.h"
#include "ps3_rom.h"

#define DMEM_SIZE 0x1000

static union {
    s16 s16[DMEM_SIZE / 2];
    u8 u8[DMEM_SIZE];
} sDmem;

static struct {
    u16 in, out, nbytes;
    u16 vol[2];
    u16 rate[2];
    u16 volWet, rateWet;
    s16* loopState;
    s16 adpcmTable[8][2][8];
} sRsp;

static s16 sResampleTable[64][4];
static u32 sTasks, sCommands, sUnknown;

#define DMEM_U8(a) (&sDmem.u8[(a) & (DMEM_SIZE - 1)])
#define DMEM_S16(a) ((s16*) DMEM_U8((a) & ~1))
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_32(v) (((v) + 31) & ~31)

static inline s16 clamp16(s32 v) {
    return (s16) (v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v));
}

/* A RAM address in a command: the game converted it with
 * VIRTUAL_TO_PHYSICAL2, which on the PS3 leaves it as it is. */
static void* ram(u32 w1) {
    return (void*) (uintptr_t) w1;
}

/* The microcode's 4-tap resampling filter, 64 phases, Q15. It is part of the
 * audio microcode's data in the ROM (US 1.0, checked by its CRC at start-up);
 * read from there. If it does not look right, a cubic (Catmull-Rom) kernel
 * takes its place. */
#define ROM_RESAMPLE_TABLE 0xF5A00
static void make_resample_table(void) {
    int p, k;

    if (PS3_Rom_Read(sResampleTable, ROM_RESAMPLE_TABLE, sizeof(sResampleTable)) == 0 &&
        sResampleTable[0][1] == 0x66AD && sResampleTable[63][2] == 0x66AD &&
        sResampleTable[0][0] == sResampleTable[63][3]) {
        ps3_logf("[audio] resampling table read from the ROM");
        return;
    }
    ps3_logf("[audio] WARNING: no resampling table in the ROM, using a cubic one");
    for (p = 0; p < 64; p++) {
        double t = p / 64.0, t2 = t * t, t3 = t2 * t;
        double w[4] = { (-t3 + 2 * t2 - t) / 2, (3 * t3 - 5 * t2 + 2) / 2, (-3 * t3 + 4 * t2 + t) / 2,
                        (t3 - t2) / 2 };

        for (k = 0; k < 4; k++) {
            sResampleTable[p][k] = (s16) floor(w[k] * 32767.0 + 0.5);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void adpcm_dec(u32 flags, s16* state) {
    const u8* in = DMEM_U8(sRsp.in);
    s16* out = DMEM_S16(sRsp.out);
    s32 nbytes = ROUND_UP_32(sRsp.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(s16));
    } else if ((flags & A_LOOP) && sRsp.loopState != NULL) {
        memcpy(out, sRsp.loopState, 16 * sizeof(s16));
    } else {
        memcpy(out, state, 16 * sizeof(s16));
    }
    out += 16;

    while (nbytes > 0) {
        s32 shift = *in >> 4;
        s16 (*tbl)[8] = sRsp.adpcmTable[*in++ & 7];
        int half, j, k;

        for (half = 0; half < 2; half++) {
            s16 ins[8];                     /* 16-bit lanes on the RSP */
            s32 prev1 = out[-1], prev2 = out[-2];

            for (j = 0; j < 4; j++) {
                ins[j * 2] = (s16) ((((s32) ((u32) (*in >> 4) << 28)) >> 28) << shift);
                ins[j * 2 + 1] = (s16) ((((s32) ((u32) (*in & 15) << 28)) >> 28) << shift);
                in++;
            }
            for (j = 0; j < 8; j++) {
                s32 acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);

                for (k = 0; k < j; k++) {
                    acc += tbl[1][j - k - 1] * ins[k];
                }
                *out = clamp16(acc >> 11);
                out++;
            }
        }
        nbytes -= 16 * sizeof(s16);
    }
    memcpy(state, out - 16, 16 * sizeof(s16));
}

static void resample(u32 flags, u16 pitch, s16* state) {
    s16 tmp[16];
    s16* inInitial = DMEM_S16(sRsp.in);
    s16* in = inInitial;
    s16* out = DMEM_S16(sRsp.out);
    s32 nbytes = ROUND_UP_16(sRsp.nbytes);
    u32 acc;
    int i;

    if (flags & A_INIT) {
        memset(tmp, 0, sizeof(tmp));
    } else {
        memcpy(tmp, state, sizeof(tmp));
    }
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(s16));
        in -= tmp[5] / (s32) sizeof(s16);
    }
    in -= 4;
    acc = (u16) tmp[4];
    memcpy(in, tmp, 4 * sizeof(s16));

    do {
        for (i = 0; i < 8; i++) {
            const s16* c = sResampleTable[(acc * 64) >> 16];
            /* each product rounded on its own, as the vector unit does */
            s32 s = ((in[0] * c[0] + 0x4000) >> 15) + ((in[1] * c[1] + 0x4000) >> 15) +
                    ((in[2] * c[2] + 0x4000) >> 15) + ((in[3] * c[3] + 0x4000) >> 15);

            *out = clamp16(s);
            out++;
            acc += (u32) pitch << 1;
            in += acc >> 16;
            acc &= 0xFFFF;
        }
        nbytes -= 8 * sizeof(s16);
    } while (nbytes > 0);

    state[4] = (s16) acc;
    memcpy(state, in, 4 * sizeof(s16));
    i = (int) ((in - inInitial + 4) & 7);
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = (s16) i;
    memcpy(state + 8, in, 8 * sizeof(s16));
}

static void env_mixer(u32 w0, u32 w1) {
    s16* in = DMEM_S16(((w0 >> 16) & 0xFF) << 4);
    s32 n = ROUND_UP_16((w0 >> 8) & 0xFF);
    int swapReverb = (w0 >> 2) & 1;
    s32 negL = (w0 >> 1) & 1 ? -1 : 0, negR = w0 & 1 ? -1 : 0;
    s16* dry[2] = { DMEM_S16(((w1 >> 24) & 0xFF) << 4), DMEM_S16(((w1 >> 16) & 0xFF) << 4) };
    s16* wet[2] = { DMEM_S16(((w1 >> 8) & 0xFF) << 4), DMEM_S16((w1 & 0xFF) << 4) };
    s32 negs[2] = { negL, negR };
    int src[2] = { swapReverb ? 1 : 0, swapReverb ? 0 : 1 };
    u16 vols[2] = { sRsp.vol[0], sRsp.vol[1] };
    u16 volWet = sRsp.volWet;
    int i, j;

    do {
        for (i = 0; i < 8; i++) {
            s32 x = *in++;
            s32 s[2];


            for (j = 0; j < 2; j++) {
                s[j] = ((x * vols[j]) >> 16) ^ negs[j];
            }
            for (j = 0; j < 2; j++) {
                *dry[j] = clamp16(*dry[j] + s[j]);
                dry[j]++;
                *wet[j] = clamp16(*wet[j] + ((s[src[j]] * volWet) >> 16));
                wet[j]++;
            }
        }
        vols[0] += sRsp.rate[0];
        vols[1] += sRsp.rate[1];
        volWet += sRsp.rateWet;
        n -= 8;
    } while (n > 0);
}

static void mix(s16 gain, u16 inAddr, u16 outAddr, s32 count) {
    s16* in = DMEM_S16(inAddr);
    s16* out = DMEM_S16(outAddr);
    s32 n = ROUND_UP_32(count) / 2, i;

    if (gain == -0x8000) {
        for (i = 0; i < n; i++) {
            out[i] = clamp16(out[i] - in[i]);
        }
        return;
    }
    for (i = 0; i < n; i++) {
        out[i] = clamp16(((s32) out[i] * 0x7FFF + (s32) in[i] * gain + 0x4000) >> 15);
    }
}

static void interleave(u16 left, u16 right) {
    s32 n = ROUND_UP_16(sRsp.nbytes) / (s32) sizeof(s16);
    const s16* l = DMEM_S16(left);
    const s16* r = DMEM_S16(right);
    s16* d = DMEM_S16(sRsp.out);

    while (n-- > 0) {
        *d++ = *l++;
        *d++ = *r++;
    }
}

static void run(const u32* cmd, u32 count) {
    u32 i;

    for (i = 0; i < count; i++, cmd += 2) {
        u32 w0 = cmd[0], w1 = cmd[1];

        sCommands++;
        switch (w0 >> 24) {
            case A_SPNOOP:
            case A_SEGMENT:
                break;
            case A_ADPCM:
                adpcm_dec((w0 >> 16) & 0xFF, (s16*) ram(w1));
                break;
            case A_CLEARBUFF:
                memset(DMEM_U8(w0 & 0xFFFF), 0, ROUND_UP_16(w1 & 0xFFFF));
                break;
            case A_RESAMPLE:
                resample((w0 >> 16) & 0xFF, (u16) w0, (s16*) ram(w1));
                break;
            case A_SETBUFF:
                sRsp.in = (u16) w0;
                sRsp.out = (u16) (w1 >> 16);
                sRsp.nbytes = (u16) w1;
                break;
            case A_DMEMMOVE:
                memmove(DMEM_U8(w1 >> 16), DMEM_U8(w0 & 0xFFFF), ROUND_UP_16(w1 & 0xFFFF));
                break;
            case A_LOADADPCM:
                memcpy(sRsp.adpcmTable, ram(w1),
                       (w0 & 0xFFFF) < sizeof(sRsp.adpcmTable) ? (w0 & 0xFFFF) : sizeof(sRsp.adpcmTable));
                break;
            case A_MIXER:
                mix((s16) w0, (u16) (w1 >> 16), (u16) w1, ((w0 >> 16) & 0xFF) << 4);
                break;
            case A_INTERLEAVE:
                interleave((u16) (w1 >> 16), (u16) w1);
                break;
            case A_SETLOOP:
                sRsp.loopState = (s16*) ram(w1);
                break;
            case A_DMEMMOVE2: {
                /* the block at `in`, `t` more times after it */
                u32 t = (w0 >> 16) & 0xFF, n = ROUND_UP_32(w1 & 0xFFFF), k;
                u16 src = (u16) w0, dst = (u16) (w1 >> 16);

                /* t + 1 copies, each from the previous one, as the
                 * microcode does: the extra block covers the resampler's
                 * look-ahead */
                for (k = 0; k <= t; k++) {
                    memmove(DMEM_U8(dst + k * n), DMEM_U8(src + k * n), n);
                }
                break;
            }
            case A_DOWNSAMPLE_HALF: {
                s32 n = ROUND_UP_8(w0 & 0xFFFF);
                const s16* in = DMEM_S16(w1 >> 16);
                s16* out = DMEM_S16(w1 & 0xFFFF);

                while (n-- > 0) {
                    *out++ = *in;
                    in += 2;
                }
                break;
            }
            case A_ENVSETUP1:
                /* MK64 builds it with aEnvSetup1Alt: the low 16 bits hold the
                 * source volumes, not a ramp for the reverb volume, which
                 * stays fixed (adding them to it every 8 samples made the
                 * reverb overflow) */
                sRsp.volWet = (u16) (((w0 >> 16) & 0xFF) << 8);
                sRsp.rateWet = 0;
                sRsp.rate[0] = (u16) (w1 >> 16);
                sRsp.rate[1] = (u16) w1;
                break;
            case A_ENVSETUP2:
                sRsp.vol[0] = (u16) (w1 >> 16);
                sRsp.vol[1] = (u16) w1;
                break;
            case A_ENVMIXER:
                env_mixer(w0, w1);
                break;
            case A_LOADBUFF:
                memcpy(DMEM_U8(w0 & 0xFFFF), ram(w1), ((w0 >> 16) & 0xFF) << 4);
                break;
            case A_SAVEBUFF:
                memcpy(ram(w1), DMEM_U8(w0 & 0xFFFF), ((w0 >> 16) & 0xFF) << 4);
                break;
            default:
                if (sUnknown++ < 10) {
                    ps3_logf("[audio] unknown command %02X (%08X %08X)", w0 >> 24, w0, w1);
                }
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* With the game                                                       */
/* ------------------------------------------------------------------ */

void ps3_audio_run_task(void* taskPtr) {
    OSTask* task = (OSTask*) taskPtr;
    const u32* cmds = (const u32*) task->t.data_ptr;
    u32 count = task->t.data_size / 8;

    if (sResampleTable[0][1] == 0) {
        make_resample_table();
    }
    if (cmds == NULL || count == 0 || count > 0x10000) {
        return;
    }
    sTasks++;
    run(cmds, count);
}

extern struct SPTask* create_next_audio_frame_task(void);

/* One 60 Hz tick of the N64's audio thread (thread4_audio). */
void ps3_audio_tick(void) {
    u64 t0 = ps3_tb();
    struct SPTask* spTask = create_next_audio_frame_task();

    if (spTask != NULL) {
        ps3_audio_run_task(&spTask->task);
    }
    gPs3Prof.t[PROF_AUDIO] += ps3_tb() - t0;
    if (sTasks == 1 || (sTasks % 3600) == 0) {
        ps3_logf("[audio] %u tasks, %u commands, %u unknown; %d frames queued for output", sTasks, sCommands,
                 sUnknown, PS3_Plat_AudioQueued());
    }
}
