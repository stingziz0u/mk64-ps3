/* main.c -- mk64-ps3: platform start-up, then the game.
 *
 * Checks, in order and leaving a trace in the log, every link that has broken
 * before without giving any error:
 *
 *   [0] the binary reaches main()        -> if not, -lrt -llv2 are missing
 *   [1] worker thread
 *   [2] sysutil callback registered      -> without it, quitting from the PS
 *                                           button restarts the console
 *   [3] USRDIR is writable (the log lives there)
 *   [4] pad
 *   [5] RSX
 *   [6] the user's ROM: found, byte order, checksum
 *   [7] memory probe: where the heap, the stack and lv2 memory live
 *   [8] the game (source/game/ps3_game.c); if the ROM is missing or wrong,
 *       the reason on screen until START or "Quit Game"
 *   [9] clean shutdown
 *
 * If the screen stays black, the log says how far it got.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include <ppu-types.h>
#include <ppu-lv2.h>
#include <sys/thread.h>
#include <sys/process.h>
#include <sys/memory.h>
#include <sysutil/sysutil.h>
#include <lv2/systime.h>

#include "ps3_log.h"
#include "ps3_video.h"
#include "ps3_pad.h"
#include "ps3_rom.h"
#include "ps3_plat.h"
#include "ps3_rsx.h"

void PS3_Plat_RequestExit(void);     /* ps3_plat.c */

/* The loader reads this ELF section to size the primary thread's stack. */
SYS_PROCESS_PARAM(1001, 0x100000);

#define TITLE "MK64-PS3"

static volatile int s_running = 1;

static void sysutil_callback(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;

    if (status == SYSUTIL_EXIT_GAME) {
        ps3_log("[sysutil] SYSUTIL_EXIT_GAME -> quitting");
        s_running = 0;
        PS3_Plat_RequestExit();
    }
}

static void list_dir(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int count = 0;

    if (!d) {
        ps3_logf("[fs] %s -> cannot open", path);
        return;
    }
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (count < 24) ps3_logf("[fs]     %s", e->d_name);
        count++;
    }
    closedir(d);
    ps3_logf("[fs] %s -> %d entries", path, count);
}

/* The game's data comes from the ROM with 32-bit addresses inside (display
 * lists, tables). The plan is to keep those structures in their N64 layout,
 * which only works if everything the game can point at lives below 4 GB.
 * This prints where each kind of memory really is on the console. */
struct lv2_meminfo { u32 total; u32 avail; };

static void probe_memory(void)
{
    static int in_data = 1;
    static int in_bss;
    int on_stack = 0;
    void *small = malloc(64);
    void *big = malloc(16 * 1024 * 1024);
    sys_mem_addr_t lv2 = 0;
    struct lv2_meminfo info = { 0, 0 };
    s32 r;

    ps3_logf("[mem] sizeof: void* %u, long %u, int %u, size_t %u",
             (unsigned)sizeof(void *), (unsigned)sizeof(long), (unsigned)sizeof(int),
             (unsigned)sizeof(size_t));
    ps3_logf("[mem] code   %p", (void *)probe_memory);
    ps3_logf("[mem] .data  %p", (void *)&in_data);
    ps3_logf("[mem] .bss   %p", (void *)&in_bss);
    ps3_logf("[mem] stack  %p", (void *)&on_stack);
    ps3_logf("[mem] malloc(64)    %p", small);
    ps3_logf("[mem] malloc(16 MB) %p", big);
    ps3_logf("[mem] ROM (12 MB)   %p", (void *)PS3_Rom_Data());

    r = sysMemoryAllocate(1024 * 1024, SYS_MEMORY_PAGE_SIZE_1M, &lv2);
    ps3_logf("[mem] sysMemoryAllocate(1 MB) -> %d, addr 0x%08X", (int)r, (unsigned)lv2);
    if (r == 0) sysMemoryFree(lv2);

    lv2syscall1(352, (u64)(unsigned long)&info);
    ps3_logf("[mem] lv2 user memory: %u KB free of %u KB", info.avail / 1024, info.total / 1024);

    free(big);
    free(small);
    (void)in_bss;
}

static void game_thread(void *arg)
{
    static char l_file[160], l_crc[96], l_mem[96];
    const char *lines[8];
    int n = 0, rc, armed = 0;
    const char *name;

    (void)arg;

    ps3_log("[1] game_thread started");

    ps3_log("[2] registering the sysutil callback");
    sysUtilRegisterCallback(0, sysutil_callback, NULL);

    ps3_log("[3] filesystem");
    list_dir(PS3_USRDIR);

    ps3_log("[4] pad");
    if (PS3_Pad_Init() < 0)
        ps3_log("[4] WARNING: ioPadInit failed, going on without a pad");

    ps3_log("[5] video");
    if (PS3_Video_Init() < 0) {
        ps3_log("[5] FATAL: could not start the RSX");
        goto done;
    }
    ps3_logf("[5] video %ux%u", (unsigned)PS3_Video_Width(), (unsigned)PS3_Video_Height());

    ps3_log("[5b] RSX backend (shaders, vertex ring)");
    if (PS3_Rsx_Init() < 0) {
        ps3_log("[5b] FATAL: the RSX backend did not start");
        goto done;
    }

    {
        static const char *const wait[] = { "Reading the ROM..." };
        PS3_Video_TextScreen(TITLE, wait, 1, -1);
    }

    ps3_log("[5c] audio");
    if (PS3_Plat_AudioInit() < 0)
        ps3_log("[5c] WARNING: no audio output, going on without sound");

    ps3_log("[6] ROM");
    rc = PS3_Rom_Load(PS3_USRDIR);
    ps3_logf("[6] PS3_Rom_Load -> %d", rc);

    name = strrchr(PS3_Rom_Path(), '/');
    name = name ? name + 1 : PS3_Rom_Path();

    switch (rc) {
        case PS3_ROM_OK:
            snprintf(l_file, sizeof(l_file), "File: %s", name);
            snprintf(l_crc, sizeof(l_crc), "CRC32 %08X: %s", (unsigned)PS3_Rom_Crc32(),
                     PS3_Rom_IsStock() ? "Mario Kart 64 (USA)" : "modified Mario Kart 64 (USA)");
            lines[n++] = "ROM OK.";
            lines[n++] = l_file;
            lines[n++] = l_crc;
            break;
        case PS3_ROM_NOT_FOUND:
            lines[n++] = "No ROM found.";
            lines[n++] = "Copy your own Mario Kart 64 (USA) ROM to";
            lines[n++] = PS3_USRDIR;
            lines[n++] = "(.z64, .v64 or .n64)";
            break;
        case PS3_ROM_BAD_SIZE:
        case PS3_ROM_BAD_GAME:
            snprintf(l_file, sizeof(l_file), "File: %s", name);
            snprintf(l_crc, sizeof(l_crc), "CRC32 %08X (stock ROM: %08X)", (unsigned)PS3_Rom_Crc32(),
                     (unsigned)PS3_ROM_CRC32);
            lines[n++] = "This is not a Mario Kart 64 (USA) ROM or a hack of it.";
            lines[n++] = l_file;
            if (rc == PS3_ROM_BAD_GAME) lines[n++] = l_crc;
            else lines[n++] = "The ROM must be between 12 and 64 MB.";
            break;
        default:
            lines[n++] = "The ROM could not be read. See mk64_log.txt.";
            break;
    }

    ps3_log("[7] memory probe");
    probe_memory();
    {
        struct lv2_meminfo info = { 0, 0 };
        lv2syscall1(352, (u64)(unsigned long)&info);
        snprintf(l_mem, sizeof(l_mem), "Free memory: %u MB of %u MB", info.avail >> 20, info.total >> 20);
        lines[n++] = "";
        lines[n++] = l_mem;
    }
    if (rc == PS3_ROM_OK) {
        ps3_log("[8] game");
        PS3_Game_Run();                /* does not return: it leaves through PS3_Plat_Exit */
    }
    lines[n++] = "START: quit.";

    ps3_log("[8] no usable ROM: reason on screen; START or PS button -> Quit Game");
    while (s_running) {
        u16 b;

        PS3_Video_TextScreen(TITLE, lines, n, -1);
        sysUtilCheckCallback();        /* without pumping the queue the event never arrives */
        PS3_Pad_Frame();
        b = PS3_Pad_Buttons();
        /* release everything first (a button may come held from the XMB) */
        if (!b) armed = 1;
        else if (armed && (b & PAD_START)) {
            ps3_log("[8] START -> quitting");
            break;
        }
        usleep(30000);
    }

done:
    ps3_log("[9] shutdown");
    PS3_Video_Shutdown();              /* rsxFinish: the RSX reads nothing any more */
    PS3_Rsx_Shutdown();
    PS3_Plat_AudioShutdown();
    PS3_Pad_Shutdown();
    ps3_log("[9] shutdown complete");
    ps3_log_shutdown();

    sysThreadExit(0);
}

int main(int argc, char **argv)
{
    sys_ppu_thread_t tid;
    u64 exit_code;

    (void)argc;
    (void)argv;

    ps3_log_init();
    ps3_log("[0] main() reached -- the link is fine (-lrt -llv2 present)");

    /* The EBOOT's default thread has a small stack: the real work goes in a
     * thread of its own. */
    if (sysThreadCreate(&tid, game_thread, NULL, 1000, 2 * 1024 * 1024,
                        THREAD_JOINABLE, "mk64") != 0) {
        ps3_log("[0] FATAL: sysThreadCreate failed");
        ps3_log_shutdown();
        return 1;
    }

    sysThreadJoin(tid, &exit_code);
    return 0;
}
