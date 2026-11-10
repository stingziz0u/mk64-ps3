/* ps3_plat.c -- the platform services of ps3_plat.h, on PSL1GHT. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sysutil/sysutil.h>
#include <lv2/systime.h>

#include "ps3_plat.h"
#include "ps3_pad_bits.h"
#include "ps3_log.h"
#include "ps3_video.h"
#include "ps3_pad.h"
#include "ps3_rsx.h"

/* ps3_pad_bits.h repeats these values for the game side. */
typedef char pad_bits_match[(PS3PAD_CROSS == PAD_CROSS && PS3PAD_START == PAD_START &&
                             PS3PAD_L2 == PAD_L2 && PS3PAD_LEFT == PAD_LEFT) ? 1 : -1];

static volatile int s_exit_requested;

void PS3_Plat_RequestExit(void) { s_exit_requested = 1; }
int  PS3_Plat_ExitRequested(void) { return s_exit_requested; }

uint64_t PS3_Plat_TimeUs(void) { return (uint64_t)sysGetSystemTime(); }

void PS3_Plat_SleepUs(uint64_t us)
{
    if (us > 0) usleep((useconds_t)us);
}

void PS3_Plat_Pump(void)
{
    sysUtilCheckCallback();           /* without pumping the queue the event never arrives */
    PS3_Pad_Frame();
}

void PS3_Plat_Pad(int port, ps3_padstate_t *out)
{
    u16 b;

    memset(out, 0, sizeof(*out));
    if (!PS3_Pad_Player(port, &b, &out->lx, &out->ly, &out->rx, &out->ry)) return;
    out->connected = 1;
    out->buttons = b;
}

void PS3_Plat_Exit(void)
{
    ps3_log("[9] shutdown");
    PS3_Video_Shutdown();             /* rsxFinish: the RSX reads nothing any more */
    PS3_Plat_AudioShutdown();             /* before the RSX: its thread must stop first */
    PS3_Rsx_Shutdown();
    PS3_Pad_Shutdown();
    ps3_log("[9] shutdown complete");
    ps3_log_shutdown();
    exit(0);
}

void PS3_Plat_TextScreen(const char *title, const char *const *lines, int nlines)
{
    PS3_Video_TextScreen(title, lines, nlines, -1);
}

int PS3_Plat_LoadFile(const char *name, void *buf, int size)
{
    char path[512];
    FILE *f;
    int n;

    snprintf(path, sizeof(path), "%s/%s", PS3_USRDIR, name);
    f = fopen(path, "rb");
    if (!f) return -1;
    n = (int)fread(buf, 1, (size_t)size, f);
    fclose(f);
    return n;
}

int PS3_Plat_SaveFile(const char *name, const void *buf, int size)
{
    char path[512];
    FILE *f;
    int n;

    snprintf(path, sizeof(path), "%s/%s", PS3_USRDIR, name);
    f = fopen(path, "wb");
    if (!f) return -1;
    n = (int)fwrite(buf, 1, (size_t)size, f);
    fclose(f);
    return n;
}
