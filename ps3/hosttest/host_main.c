/* host_main.c -- runs the game side of mk64-ps3 on big-endian PowerPC Linux
 * (qemu-ppc64 user mode), with no console.
 *
 * The game objects are the very ones built for the PS3 (ppu-gcc, same ABI:
 * 64-bit big-endian ELFv1); only the platform side (ps3_plat.h, ps3_log.h,
 * ps3_rom.h) is replaced by this file. It catches logic, layout and 64-bit
 * bugs before a round trip to the console. It says nothing about the RSX,
 * real audio or PSL1GHT.
 *
 *   mk64host <rom.z64> <frames> [frame:buttons ...]
 *
 * "frame:buttons" presses the given PS3PAD_* bits (hex) from that frame on,
 * e.g. 300:0800 304:0000 presses START for four frames.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <ucontext.h>

#include "ps3_plat.h"
#include "ps3_log.h"
#include "ps3_rom.h"

#define ROM_SIZE 0xC00000u

static uint8_t *s_rom;
static unsigned s_frame, s_max_frames;
static uint64_t s_time_us = 1000000;
static struct { unsigned frame, buttons; } s_script[256];
static int s_nscript;

/* ---- ps3_log.h ---- */
void ps3_log_init(void) {}
void ps3_log_shutdown(void) {}
void ps3_log(const char *msg) { puts(msg); fflush(stdout); }
void ps3_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ---- ps3_rom.h ---- */
int PS3_Rom_Read(void *dst, uint32_t off, uint32_t size)
{
    if (off > ROM_SIZE || size > ROM_SIZE - off) {
        ps3_logf("[rom] read outside the ROM: off 0x%X size 0x%X", off, size);
        return -1;
    }
    if ((uintptr_t)dst > 0xFFFFFFFFu) {
        ps3_logf("[rom] *** bad destination %p for off 0x%X size 0x%X, from %p <- %p", dst, off, size,
                 __builtin_return_address(0), __builtin_return_address(1));
    }
    memcpy(dst, s_rom + off, size);
    return 0;
}

/* ---- ps3_plat.h ---- */
uint64_t PS3_Plat_TimeUs(void) { return s_time_us; }
void PS3_Plat_SleepUs(uint64_t us) { s_time_us += us; }
int  PS3_Plat_ExitRequested(void) { return 0; }
void PS3_Plat_Exit(void) { ps3_log("[host] exit"); exit(0); }
void PS3_Plat_TextScreen(const char *title, const char *const *lines, int nlines)
{
    (void)title; (void)lines; (void)nlines;
}

void PS3_Plat_Pump(void)
{
    s_frame++;
    {   /* MK64_FINISH=<frame>: the player's lap count is set to 3 at that frame,
         * to get to the end-of-race screens without driving three laps */
        static long fin = -2;
        extern int gLapCountByPlayerId[];
        if (fin == -2) { const char *e = getenv("MK64_FINISH"); fin = e ? atol(e) : -1; }
        if (fin > 0 && s_frame == (unsigned)fin) {
            extern int gPs3HostFirstPlace;
            gLapCountByPlayerId[0] = 3;
            gPs3HostFirstPlace = getenv("MK64_FIRST") != NULL;
        }
        if (getenv("MK64_LAPLOG") && (s_frame % 150) == 0)
            ps3_logf("[host] laps: %d %d %d %d %d %d %d %d", gLapCountByPlayerId[0], gLapCountByPlayerId[1],
                     gLapCountByPlayerId[2], gLapCountByPlayerId[3], gLapCountByPlayerId[4], gLapCountByPlayerId[5],
                     gLapCountByPlayerId[6], gLapCountByPlayerId[7]);
        {   /* MK64_COURSE=<cup*4+race>: that GP race, whatever was picked in the menus
             * (the game takes the course from gCupCourseOrder[cup][race] when it loads) */
            static long course = -2;
            extern signed char gCupSelection, gCourseIndexInCup;
            if (course == -2) { const char *e = getenv("MK64_COURSE"); course = e ? atol(e) : -1; }
            if (course >= 0 && s_frame < 962) { gCupSelection = (signed char)(course / 4); gCourseIndexInCup = (signed char)(course % 4); }
        }
        {   /* MK64_AUTOPILOT=<frame>: from then on the CPU drives player 1 */
            static long ap = -2;
            extern unsigned short *gPlayerOne;
            if (ap == -2) { const char *e = getenv("MK64_AUTOPILOT"); ap = e ? atol(e) : -1; }
            static long ape = -2;   /* MK64_AUTOPILOT_END=<frame>: player 1 is human again */
            if (ape == -2) { const char *e = getenv("MK64_AUTOPILOT_END"); ape = e ? atol(e) : -1; }
            if (ape > 0 && s_frame >= (unsigned)ape) {
                if (s_frame == (unsigned)ape && gPlayerOne)
                    gPlayerOne[0] = (unsigned short)((gPlayerOne[0] & ~0x1000) | 0x4000);
            } else if (ap > 0 && s_frame >= (unsigned)ap && gPlayerOne && (gPlayerOne[0] & 0x4000))
                gPlayerOne[0] = (unsigned short)((gPlayerOne[0] & ~0x4000) | 0x1000);
        }
        {   /* MK64_GHOSTOK=<frame>: from then on the replay counts as saveable */
            static long go = -2;
            extern int gPostTimeTrialReplayCannotSave;
            extern unsigned short bPlayerGhostDisabled;
            if (go == -2) { const char *e = getenv("MK64_GHOSTOK"); go = e ? atol(e) : -1; }
            if (go > 0 && s_frame >= (unsigned)go) { gPostTimeTrialReplayCannotSave = 0; bPlayerGhostDisabled = 0; }
        }
        {   /* MK64_GP=<frame>: Grand Prix forced in the menus up to that frame */
            static long gp = -2;
            extern int gModeSelection;
            if (gp == -2) { const char *e = getenv("MK64_GP"); gp = e ? atol(e) : -1; }
            if (gp > 0 && s_frame < (unsigned)gp) gModeSelection = 0;
            {   /* MK64_TT=<frame>: Time Trials, the same way */
                static long tt = -2;
                if (tt == -2) { const char *e = getenv("MK64_TT"); tt = e ? atol(e) : -1; }
                if (tt > 0 && s_frame < (unsigned)tt) gModeSelection = 1;
            }
        }
    }
    s_time_us += 1000;                 /* the frame took 1 ms; the rest is "slept" */
    if (s_frame >= s_max_frames) {
        { extern unsigned g_host_underruns; ps3_logf("[host] %u frames done, %u audio underruns", s_frame, g_host_underruns); }
        exit(0);
    }
}

void PS3_Plat_Pad(int port, ps3_padstate_t *out)
{
    int i;

    memset(out, 0, sizeof(*out));
    if (port != 0) {   /* MK64_PADS=<n>: pads 2..n connected, at rest */
        static int npads = -1;
        if (npads < 0) { const char *e = getenv("MK64_PADS"); npads = e ? atoi(e) : 1; }
        out->connected = port < npads;
        return;
    }
    out->connected = 1;
    for (i = 0; i < s_nscript; i++)
        if (s_frame >= s_script[i].frame) out->buttons = s_script[i].buttons;
    /* d-pad left/right also steer: the left stick at 3/4 */
    if (out->buttons & 0x8000) out->lx = -96;
    if (out->buttons & 0x2000) out->lx = 96;
}

int PS3_Plat_LoadFile(const char *name, void *buf, int size)
{
    FILE *f = fopen(name, "rb");
    int n;
    if (!f) return -1;
    n = (int)fread(buf, 1, (size_t)size, f);
    fclose(f);
    return n;
}

int PS3_Plat_SaveFile(const char *name, const void *buf, int size)
{
    FILE *f = fopen(name, "wb");
    int n;
    if (!f) return -1;
    n = (int)fwrite(buf, 1, (size_t)size, f);
    fclose(f);
    return n;
}

/* ---- crash report: fault address, pc and the chain of return addresses ---- */
static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    unsigned long *r = (unsigned long *)uc->uc_mcontext.gp_regs;
    unsigned long sp = r[1];
    int i;

    printf("[host] CRASH signal %d at frame %u: fault addr %p, pc 0x%lx, lr 0x%lx\n",
           sig, s_frame, si->si_addr, r[32], r[36]);
    for (i = 0; i < 31; i += 4)
        printf("[host]   r%-2d %016lx  r%-2d %016lx  r%-2d %016lx  r%-2d %016lx\n",
               i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    for (i = 0; i < 24 && sp >= 0x10000000UL && sp < 0x100000000UL; i++) {
        unsigned long next = *(unsigned long *)sp;
        if (next <= sp || next >= 0x100000000UL) break;
        printf("[host]   return address 0x%lx\n", *(unsigned long *)(next + 16));
        sp = next;
    }
    fflush(stdout);
    _exit(3);
}

/* The game needs every address it can point at below 4 GB, the stack
 * included (on the PS3 it is at 0xD0000000): run it on a stack in .bss. */
static char s_stack[8 * 1024 * 1024] __attribute__((aligned(16)));
static char s_sigstack[256 * 1024];

static void *game_thread(void *arg)
{
    stack_t ss;

    (void)arg;
    ss.ss_sp = s_sigstack;
    ss.ss_size = sizeof(s_sigstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    PS3_Game_Run();
    return NULL;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    pthread_attr_t attr;
    pthread_t th;
    FILE *f;
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s rom.z64 frames [frame:buttons ...]\n", argv[0]);
        return 2;
    }
    s_max_frames = (unsigned)strtoul(argv[2], NULL, 0);
    for (i = 3; i < argc && s_nscript < 256; i++, s_nscript++)
        sscanf(argv[i], "%u:%x", &s_script[s_nscript].frame, &s_script[s_nscript].buttons);

    s_rom = malloc(ROM_SIZE);
    f = fopen(argv[1], "rb");
    if (!f || fread(s_rom, 1, ROM_SIZE, f) != ROM_SIZE) {
        fprintf(stderr, "cannot read the ROM %s\n", argv[1]);
        return 2;
    }
    fclose(f);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    printf("[host] stack %p .. %p\n", (void *)s_stack, (void *)(s_stack + sizeof(s_stack)));
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, s_stack, sizeof(s_stack));
    pthread_create(&th, &attr, game_thread, NULL);
    pthread_join(th, NULL);
    return 0;
}

/* ---- sound: everything pushed goes to mk64_audio.wav; playback is
 * simulated against the host clock so that osAiGetLength behaves ---- */
static unsigned s_arate = 32000;
static uint64_t s_apushed, s_astart_us;
static FILE *s_wav;
static uint32_t s_wav_frames;

static void wav_header(FILE *f, uint32_t frames, unsigned rate)
{
    uint32_t data = frames * 4, v;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f);   /* host is big-endian: fixed below */
    fwrite("WAVEfmt ", 1, 8, f);
    {
        unsigned char h[24];
        uint32_t le[] = { 16, 0x00020001u, rate, rate * 4, 0x00100004u };
        int i, k;
        for (i = 0; i < 5; i++)
            for (k = 0; k < 4; k++) h[i * 4 + k] = (unsigned char)(le[i] >> (8 * k));
        fwrite(h, 1, 20, f);
    }
    fwrite("data", 1, 4, f);
    {
        unsigned char d[4] = { (unsigned char)data, (unsigned char)(data >> 8), (unsigned char)(data >> 16),
                               (unsigned char)(data >> 24) };
        fwrite(d, 1, 4, f);
    }
    {   /* RIFF size, little-endian */
        unsigned char r[4];
        v = 36 + data;
        r[0] = (unsigned char)v; r[1] = (unsigned char)(v >> 8); r[2] = (unsigned char)(v >> 16); r[3] = (unsigned char)(v >> 24);
        fseek(f, 4, SEEK_SET);
        fwrite(r, 1, 4, f);
    }
    fseek(f, 0, SEEK_END);
}

int  PS3_Plat_AudioInit(void) { return 0; }
void PS3_Plat_AudioShutdown(void) {}
void PS3_Plat_AudioSetRate(unsigned hz) { s_arate = hz; }

/* The console's output thread, simulated: one block of 256 frames at 48 kHz
 * every 5.33 ms, same priming rule as ps3_audio.c. */
static uint64_t s_aread, s_ablocks;
static uint32_t s_afrac;
static int s_apriming = 1;
unsigned g_host_underruns;

static void audio_advance(void)
{
    uint64_t due;
    if (!s_wav) return;
    due = (s_time_us - s_astart_us) * 48000ULL / 1000000ULL / 256;
    for (; s_ablocks < due; s_ablocks++) {
        uint64_t avail = s_apushed - s_aread;
        uint32_t step = (uint32_t)(((uint64_t)s_arate << 16) / 48000), need;
        if (s_apriming) {
            if (avail < 768) continue;
            s_apriming = 0;
        }
        need = (uint32_t)((s_afrac + step * 256ULL) >> 16);
        if (avail < need + 1) { g_host_underruns++; s_apriming = 1; s_aread = s_apushed; s_afrac = 0; continue; }
        s_afrac += step * 256;
        s_aread += s_afrac >> 16;
        s_afrac &= 0xFFFF;
    }
}

int PS3_Plat_AudioQueued(void)
{
    audio_advance();
    return (int)(s_apushed - s_aread);
}

void PS3_Plat_AudioPush(const int16_t *stereo, int frames)
{
    int i;
    if (!s_wav) {
        s_wav = fopen("mk64_audio.wav", "wb");
        s_astart_us = s_time_us;
        wav_header(s_wav, 0, s_arate);
    }
    audio_advance();
    s_apushed += (uint64_t)frames;
    for (i = 0; i < frames * 2; i++) {                 /* to little-endian */
        unsigned char b[2] = { (unsigned char)stereo[i], (unsigned char)((uint16_t)stereo[i] >> 8) };
        fwrite(b, 1, 2, s_wav);
    }
    s_wav_frames += (uint32_t)frames;
    wav_header(s_wav, s_wav_frames, s_arate);
}
