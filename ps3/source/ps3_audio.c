/* ps3_audio.c -- sound output: the samples the game produces (stereo, 16-bit,
 * at the N64's rate, usually 32 kHz) go through a ring buffer to a thread
 * that feeds the PS3 audio port (48 kHz, float), resampling on the way.
 *
 * The game side pushes from its own thread (PS3_Plat_AudioPush, one audio
 * frame per 60 Hz tick); the output thread only reads. One writer and one
 * reader, so the ring needs no lock, only memory barriers.
 *
 * The port set-up is the one of Doom64-PS3, which runs on hardware.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <audio/audio.h>

#include "ps3_plat.h"
#include "ps3_log.h"

#define OUT_RATE    48000
#define BLOCK       256                     /* AUDIO_BLOCK_SAMPLES */
#define RING_FRAMES 16384                   /* about half a second at 32 kHz */
#define MAX_QUEUED  (RING_FRAMES - 2048)

static int16_t           s_ring[RING_FRAMES * 2];
static volatile uint32_t s_wr, s_rd;        /* in frames, they only grow */
static volatile uint32_t s_rate = 32000;
static uint32_t          s_frac;            /* position between two input frames, 16.16 */
static uint32_t          s_underruns, s_dropped;
static int               s_priming = 1;     /* waiting for PRIME_FRAMES before playing */
#define PRIME_FRAMES 768                    /* half of the cushion in osAiGetLength */

static u32               s_port;
static audioPortConfig   s_cfg;
static sys_event_queue_t s_queue;
static sys_ipc_key_t     s_key;
static sys_ppu_thread_t  s_tid;
static volatile int      s_quit;
static int               s_ok;

#define BARRIER() __asm__ volatile("lwsync" ::: "memory")

void PS3_Plat_AudioSetRate(unsigned hz)
{
    if (hz >= 8000 && hz <= 48000) s_rate = hz;
}

int PS3_Plat_AudioQueued(void)
{
    return (int)(s_wr - s_rd);
}

void PS3_Plat_AudioPush(const int16_t *stereo, int frames)
{
    uint32_t wr = s_wr, i;

    if (!s_ok || frames <= 0) return;
    if ((int)(wr - s_rd) + frames > MAX_QUEUED) {
        s_dropped += (uint32_t)frames;      /* too far ahead: drop rather than add latency */
        return;
    }
    for (i = 0; i < (uint32_t)frames; i++) {
        uint32_t p = ((wr + i) % RING_FRAMES) * 2;
        s_ring[p] = stereo[i * 2];
        s_ring[p + 1] = stereo[i * 2 + 1];
    }
    BARRIER();
    s_wr = wr + (uint32_t)frames;
}

/* One block of the port: BLOCK frames at 48 kHz, linear interpolation. */
static void fill(float *dst)
{
    uint32_t step = (uint32_t)(((uint64_t)s_rate << 16) / OUT_RATE);
    uint32_t rd = s_rd, avail;
    int i;

    BARRIER();
    avail = s_wr - rd;
    if (s_priming) {
        /* After start-up or an underrun, wait for some samples to pile up
         * instead of playing them as they trickle in (that crackles). */
        if (avail < PRIME_FRAMES) {
            memset(dst, 0, BLOCK * 2 * sizeof(float));
            return;
        }
        s_priming = 0;
    }
    for (i = 0; i < BLOCK; i++) {
        if (avail < 2) {
            dst[i * 2] = dst[i * 2 + 1] = 0.0f;
            if (!s_priming) {
                s_underruns++;
                s_priming = 1;
            }
            continue;
        }
        {
            uint32_t a = (rd % RING_FRAMES) * 2, b = ((rd + 1) % RING_FRAMES) * 2;
            float f = (float)s_frac * (1.0f / 65536.0f);
            float l = s_ring[a] + (s_ring[b] - s_ring[a]) * f;
            float r = s_ring[a + 1] + (s_ring[b + 1] - s_ring[a + 1]) * f;
            dst[i * 2] = l * (1.0f / 32768.0f);
            dst[i * 2 + 1] = r * (1.0f / 32768.0f);
        }
        s_frac += step;
        rd += s_frac >> 16;
        avail -= s_frac >> 16;
        s_frac &= 0xFFFF;
    }
    BARRIER();
    s_rd = rd;
}

static void output_thread(void *arg)
{
    sys_event_t ev;

    (void)arg;
    while (!s_quit) {
        u64 cur;
        u32 blk;

        if (sysEventQueueReceive(s_queue, &ev, 20 * 1000) != 0)
            continue;
        cur = *(volatile u64 *)(u64)s_cfg.readIndex;
        blk = (u32)((cur + 1) % s_cfg.numBlocks);
        fill((float *)(u64)s_cfg.audioDataStart + blk * BLOCK * s_cfg.channelCount);
    }
    sysThreadExit(0);
}

int PS3_Plat_AudioInit(void)
{
    audioPortParam p;
    s32 r;

    r = audioInit();
    if (r != 0) { ps3_logf("[snd] audioInit failed (%d)", (int)r); return -1; }

    memset(&p, 0, sizeof(p));
    p.numChannels = AUDIO_PORT_2CH;
    p.numBlocks   = AUDIO_BLOCK_16;
    p.attrib      = 0;
    p.level       = 1.0f;
    r = audioPortOpen(&p, &s_port);
    if (r != 0) { ps3_logf("[snd] audioPortOpen failed (%d)", (int)r); audioQuit(); return -1; }
    audioGetPortConfig(s_port, &s_cfg);
    audioCreateNotifyEventQueue(&s_queue, &s_key);
    audioSetNotifyEventQueue(s_key);
    sysEventQueueDrain(s_queue);
    memset((void *)(u64)s_cfg.audioDataStart, 0, s_cfg.portSize);
    audioPortStart(s_port);
    s_ok = 1;

    s_quit = 0;
    sysThreadCreate(&s_tid, output_thread, NULL, 200, 64 * 1024, THREAD_JOINABLE, "mk64snd");
    ps3_logf("[snd] audio ready: port %u, %u blocks, %u channels, 48000 Hz",
             (unsigned)s_port, (unsigned)s_cfg.numBlocks, (unsigned)s_cfg.channelCount);
    return 0;
}

void PS3_Plat_AudioShutdown(void)
{
    u64 rv;

    if (!s_ok) return;
    s_quit = 1;
    sysThreadJoin(s_tid, &rv);
    audioPortStop(s_port);
    audioRemoveNotifyEventQueue(s_key);
    audioPortClose(s_port);
    sysEventQueueDestroy(s_queue, 0);
    audioQuit();
    s_ok = 0;
    ps3_logf("[snd] audio closed (%u underruns, %u frames dropped)", (unsigned)s_underruns, (unsigned)s_dropped);
}
