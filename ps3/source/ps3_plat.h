/* ps3_plat.h -- what the game side asks of the platform side.
 *
 * Plain C types only. The game is compiled against the N64 headers
 * (ultra64.h), which define u8/u16/u32... just like PSL1GHT's ppu-types.h, so
 * no file may include both. Everything under source/game/ sees the PS3 through
 * this header, ps3_log.h and ps3_rom.h; everything else never sees ultra64.h.
 */

#ifndef PS3_PLAT_H
#define PS3_PLAT_H

#include <stdint.h>

/* DualShock 3 state for one port, already with the dead zone applied.
 * buttons: PAD_* bits of ps3_pad.h. Sticks: -127..127, up and left negative. */
typedef struct {
    int connected;
    unsigned buttons;
    int lx, ly, rx, ry;
} ps3_padstate_t;

uint64_t PS3_Plat_TimeUs(void);
void     PS3_Plat_SleepUs(uint64_t us);

/* Once per frame: pumps the sysutil queue (the "Quit Game" event) and reads
 * the pads. */
void PS3_Plat_Pump(void);
void PS3_Plat_Pad(int port, ps3_padstate_t *out);

int  PS3_Plat_ExitRequested(void);
void PS3_Plat_Exit(void);                 /* clean shutdown; does not return */

/* Text screen (title + lines), flips. For the stages with no renderer. */
void PS3_Plat_TextScreen(const char *title, const char *const *lines, int nlines);

/* Whole file <-> buffer in USRDIR. Return the byte count or -1. */
int PS3_Plat_LoadFile(const char *name, void *buf, int size);
int PS3_Plat_SaveFile(const char *name, const void *buf, int size);

/* Sound output (ps3_audio.c). Stereo 16-bit frames at the rate given. */
int  PS3_Plat_AudioInit(void);
void PS3_Plat_AudioShutdown(void);
void PS3_Plat_AudioSetRate(unsigned hz);
void PS3_Plat_AudioPush(const int16_t *stereo, int frames);
int  PS3_Plat_AudioQueued(void);          /* frames not played yet */

/* The game (source/game/ps3_game.c). Does not return. */
void PS3_Game_Run(void);

#endif /* PS3_PLAT_H */
