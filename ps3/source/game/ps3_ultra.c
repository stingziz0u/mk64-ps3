/* ps3_ultra.c -- the part of libultra the game calls, for a single thread.
 *
 * On the N64 the game is four threads talking through message queues (video,
 * audio, game loop, idle) plus the RSP. Here the game loop is the only thread:
 *   - message queues are real ring buffers, but nothing ever blocks: a
 *     blocking receive on an empty queue returns at once, because nobody else
 *     could fill it;
 *   - a ROM read (osPiStartDma) is done on the spot from the ROM in memory;
 *   - osViSwapBuffer() is the end of the frame.
 */

#include <ultra64.h>
#include <PR/os.h>
#include <PR/ucode.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "ps3_game.h"
#include "ps3_plat.h"
#include "ps3_rom.h"
#include "ps3_pad_bits.h"

/* ------------------------------------------------------------------ */
/* Globals libultra owns                                               */
/* ------------------------------------------------------------------ */

u32 osTvType = TV_TYPE_NTSC;
u32 osResetType = 0;                      /* cold reset */
s32 osAppNmiBuffer[16];
u64 osClockRate = 62500000ULL;
OSViMode osViModeTable[56];

/* The RSP microcode. Nothing runs it; the task structures just point here. */
u64 rspF3DBootStart[1], rspF3DBootEnd[1];
u64 gspF3DEXTextStart[1], gspF3DLXTextStart[1];
u64 gspF3DEXDataStart[1], gspF3DLXDataStart[1];
u64 rspAspMainStart[1], rspAspMainDataStart[1], rspAspMainDataEnd[1];

/* ------------------------------------------------------------------ */
/* Threads, VI, PI, caches: nothing to do                              */
/* ------------------------------------------------------------------ */

void osInitialize(void) {
}
void osCreateThread(OSThread* t, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri) {
    (void) t; (void) id; (void) entry; (void) arg; (void) sp; (void) pri;
}
void osStartThread(OSThread* t) {
    (void) t;
}
void osSetThreadPri(OSThread* t, OSPri pri) {
    (void) t; (void) pri;
}
void osCreateViManager(OSPri pri) {
    (void) pri;
}
void osViSetMode(OSViMode* mode) {
    (void) mode;
}
void osViBlack(u8 active) {
    (void) active;
}
void osViSetSpecialFeatures(u32 func) {
    (void) func;
}
void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount) {
    (void) mq; (void) msg; (void) retraceCount;
}
void osSetEventMesg(OSEvent e, OSMesgQueue* mq, OSMesg msg) {
    (void) e; (void) mq; (void) msg;
}
void osCreatePiManager(OSPri pri, OSMesgQueue* cmdQ, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    (void) pri; (void) cmdQ; (void) cmdBuf; (void) cmdMsgCnt;
}
void osInvalDCache(void* addr, size_t n) {
    (void) addr; (void) n;
}
void osInvalICache(void* addr, size_t n) {
    (void) addr; (void) n;
}
void osWritebackDCacheAll(void) {
}

void create_debug_thread(void) {
}
void start_debug_thread(void) {
}
void crash_screen_set_framebuffer(u16* framebuffer) {
    (void) framebuffer;
}

void rmonPrintf(const char* fmt, ...) {
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ps3_logf("[rmon] %s", buf);
}

/* ------------------------------------------------------------------ */
/* Message queues                                                      */
/* ------------------------------------------------------------------ */

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msgBuf, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    s32 last;

    (void) flag;
    if (mq == NULL || mq->msg == NULL || mq->msgCount <= 0) {
        return -1;
    }
    if (mq->validCount >= mq->msgCount) {
        /* Full. A blocking send would wait for a reader that does not exist. */
        return -1;
    }
    last = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[last] = msg;
    mq->validCount++;
    return 0;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    if (mq == NULL || mq->msg == NULL || mq->msgCount <= 0 || mq->validCount == 0) {
        /* Empty. OS_MESG_BLOCK would wait for a vblank or for another thread:
         * both are handled elsewhere, so just go on. */
        return (flag == OS_MESG_NOBLOCK) ? -1 : 0;
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ROM                                                                 */
/* ------------------------------------------------------------------ */

s32 osPiStartDma(OSIoMesg* mb, s32 priority, s32 direction, uintptr_t devAddr, void* vAddr, size_t nbytes,
                 OSMesgQueue* mq) {
    static int errors;

    (void) mb; (void) priority;
    if (direction != OS_READ) {
        ps3_logf("[dma] write to the cartridge ignored (0x%lX)", (unsigned long) devAddr);
    } else if (PS3_Rom_Read(vAddr, (uint32_t) devAddr, (uint32_t) nbytes) != 0 && errors < 20) {
        errors++;
        ps3_logf("[dma] BAD read: rom 0x%lX -> %p, 0x%lX bytes", (unsigned long) devAddr, vAddr,
                 (unsigned long) nbytes);
    }
    if (mq != NULL) {
        osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* RSP tasks: the display list is drawn in exec_display_list()         */
/* ------------------------------------------------------------------ */

void osSpTaskLoad(OSTask* task) {
    (void) task;
}
void osSpTaskStartGo(OSTask* task) {
    (void) task;
}
void osSpTaskYield(void) {
}
OSYieldResult osSpTaskYielded(OSTask* task) {
    (void) task;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static s64 sTimeOffset;

/* OSTime counts CPU counter ticks: 46.875 MHz. */
OSTime osGetTime(void) {
    return (OSTime) ((s64) (PS3_Plat_TimeUs() * 46875ULL / 1000ULL) + sTimeOffset);
}

void osSetTime(OSTime time) {
    sTimeOffset = (s64) time - (s64) (PS3_Plat_TimeUs() * 46875ULL / 1000ULL);
}

u32 osGetCount(void) {
    return (u32) (PS3_Plat_TimeUs() * 46875ULL / 1000ULL);
}

/* The frame ends here: display_and_vsync() calls this once per frame. */
void osViSwapBuffer(void* framebuffer) {
    (void) framebuffer;
    ps3_frame_end();
}

/* ------------------------------------------------------------------ */
/* Controllers                                                         */
/*                                                                      */
/*   Cross A        Square B       L1 / L2  Z      R1 / R2  R           */
/*   Select L       Start START    d-pad    d-pad                       */
/*   Triangle C-up  Circle C-right right stick: the four C buttons      */
/* ------------------------------------------------------------------ */

#define N64_A 0x8000
#define N64_B 0x4000
#define N64_Z 0x2000
#define N64_START 0x1000
#define N64_DU 0x0800
#define N64_DD 0x0400
#define N64_DL 0x0200
#define N64_DR 0x0100
#define N64_L 0x0020
#define N64_R 0x0010
#define N64_CU 0x0008
#define N64_CD 0x0004
#define N64_CL 0x0002
#define N64_CR 0x0001

s32 osContInit(OSMesgQueue* mq, u8* bitpattern, OSContStatus* status) {
    s32 i;

    (void) mq;
    /* Port 1 is always reported as present: the game refuses to read any
     * controller if the first one is missing at boot. The others as they are
     * now; pads connected later are read all the same (osContGetReadData). */
    *bitpattern = 0x01;
    for (i = 0; i < 4; i++) {
        ps3_padstate_t st;

        PS3_Plat_Pad(i, &st);
        if (i > 0 && st.connected) {
            *bitpattern |= (u8) (1 << i);
        }
        status[i].type = (i == 0 || st.connected) ? CONT_TYPE_NORMAL : 0;
        status[i].status = 0;
        status[i].errnum = (i == 0 || st.connected) ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    return 0;
}

s32 osContStartReadData(OSMesgQueue* mq) {
    osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    return 0;
}

static s8 stick_to_n64(int v) {
    /* -127..127 -> -80..80, the range of a real N64 stick. */
    int r = v * 80 / 127;
    if (r > 80) r = 80;
    if (r < -80) r = -80;
    return (s8) r;
}

void osContGetReadData(OSContPad* pads) {
    s32 i;

    for (i = 0; i < 4; i++) {
        ps3_padstate_t st;
        u16 b = 0;

        memset(&pads[i], 0, sizeof(pads[i]));
        PS3_Plat_Pad(i, &st);             /* players 2-4: the other pads, in port order */
        if (!st.connected) {
            if (i != 0) {
                pads[i].errno = CONT_NO_RESPONSE_ERROR;
            }
            continue;                     /* player 1: neutral, not an error (see osContInit) */
        }
        b = ps3_map_buttons(st.buttons);  /* bind_ lines of mk64.cfg */
        if (st.ry < -64) b |= N64_CU;
        if (st.ry > 64) b |= N64_CD;
        if (st.rx < -64) b |= N64_CL;
        if (st.rx > 64) b |= N64_CR;
        pads[i].button = b;
        pads[i].stick_x = stick_to_n64(st.lx);
        pads[i].stick_y = stick_to_n64(-st.ly);   /* N64: up is positive */
    }
}

/* ------------------------------------------------------------------ */
/* EEPROM (4 kbit = 512 bytes): the save file                          */
/* ------------------------------------------------------------------ */

#define EEPROM_SIZE 512
#define EEPROM_FILE "mk64.eep"

static u8 sEeprom[EEPROM_SIZE];
static s32 sEepromLoaded;

static void eeprom_load(void) {
    s32 n;

    if (sEepromLoaded) {
        return;
    }
    sEepromLoaded = 1;
    memset(sEeprom, 0, sizeof(sEeprom));
    n = PS3_Plat_LoadFile(EEPROM_FILE, sEeprom, EEPROM_SIZE);
    ps3_logf("[eeprom] %s: %d bytes read%s", EEPROM_FILE, (int) n, n < 0 ? " (no save yet)" : "");
}

s32 osEepromProbe(OSMesgQueue* mq) {
    (void) mq;
    eeprom_load();
    return EEPROM_TYPE_4K;
}

s32 osEepromLongRead(OSMesgQueue* mq, u8 address, u8* buffer, s32 nbytes) {
    s32 off = address * 8;

    (void) mq;
    eeprom_load();
    if (off < 0 || nbytes < 0 || off + nbytes > EEPROM_SIZE) {
        ps3_logf("[eeprom] read out of range: block %d, %d bytes", (int) address, (int) nbytes);
        return -1;
    }
    memcpy(buffer, sEeprom + off, nbytes);
    return 0;
}

s32 osEepromLongWrite(OSMesgQueue* mq, u8 address, u8* buffer, s32 nbytes) {
    s32 off = address * 8;

    (void) mq;
    eeprom_load();
    if (off < 0 || nbytes < 0 || off + nbytes > EEPROM_SIZE) {
        ps3_logf("[eeprom] write out of range: block %d, %d bytes", (int) address, (int) nbytes);
        return -1;
    }
    if (memcmp(sEeprom + off, buffer, nbytes) == 0) {
        return 0;                         /* nothing changed: do not touch the disk */
    }
    memcpy(sEeprom + off, buffer, nbytes);
    if (PS3_Plat_SaveFile(EEPROM_FILE, sEeprom, EEPROM_SIZE) != EEPROM_SIZE) {
        ps3_log("[eeprom] WARNING: could not write " EEPROM_FILE);
    }
    return 0;
}

/* Controller Pak: ps3_pak.c */

/* ------------------------------------------------------------------ */
/* Audio interface: the samples go to the platform (ps3_audio.c)        */
/* ------------------------------------------------------------------ */

/* Bytes still to be played. The game produces fewer samples when this
 * grows, so it keeps the output queue around one audio frame. */
/* The game sizes each audio frame to keep about 0x40 samples waiting in the
 * AI, which works with the N64's DMA but not with an output thread that takes
 * 256 samples at a time: it would run dry before every tick. So the AI is
 * told about what is queued beyond a cushion, and the game keeps the ring
 * PS3_AUDIO_CUSHION frames fuller (about 57 ms at 26800 Hz, enough for a frame that takes 3 vblanks). */
#define PS3_AUDIO_CUSHION 1536
u32 osAiGetLength(void) {
    int q = PS3_Plat_AudioQueued() - PS3_AUDIO_CUSHION;

    return q > 0 ? (u32) q * 4 : 0;
}
s32 osAiSetFrequency(u32 freq) {
    ps3_logf("[audio] output frequency %u Hz", (unsigned) freq);
    PS3_Plat_AudioSetRate(freq);
    return (s32) freq;
}
/* An interleaved stereo 16-bit buffer, big-endian like the PS3. */
int gPs3AudioMuteOutput;   /* set while audio_reset_session_eu runs the audio thread by hand */
s32 osAiSetNextBuffer(void* buf, u32 size) {
    if (!gPs3AudioMuteOutput) {
        PS3_Plat_AudioPush((const int16_t*) buf, (int) (size / 4));
    }
    return 0;
}
