/* ps3_rom.h -- the user's Mario Kart 64 ROM, loaded whole into memory.
 *
 * The PKG ships no game data. Every asset (courses, textures, models, audio)
 * is read at run time from the ROM the user copies to USRDIR. The N64 is
 * big-endian like the PS3, so the bytes are used as they are.
 */

#ifndef PS3_ROM_H
#define PS3_ROM_H

#include <stdint.h>

#define PS3_ROM_SIZE   0xC00000u          /* 12 MB: the stock ROM; hacks may be bigger */
#define PS3_ROM_MAX    0x4000000u         /* 64 MB: the most a hack can be */
#define PS3_ROM_CRC32  0x434389C1u        /* Mario Kart 64 (USA), .z64 order */

enum {
    PS3_ROM_OK = 0,
    PS3_ROM_NOT_FOUND = -1,   /* nothing that looks like an N64 ROM in USRDIR */
    PS3_ROM_BAD_SIZE  = -2,   /* an N64 ROM, but smaller than 12 MB or bigger than 64 MB */
    PS3_ROM_BAD_GAME  = -3,   /* N64 ROM, but not Mario Kart 64 (USA) or a hack of it */
    PS3_ROM_NO_MEMORY = -4,
    PS3_ROM_READ_ERR  = -5
};

/* Finds the ROM in `dir` (the file named by `rom=` in mk64.cfg, else by
 * extension .z64/.v64/.n64, else by its N64 header; the name does not
 * matter), loads it, puts it in .z64 byte order and checks that it is the
 * USA release or a hack of it (game code NKTE). Returns PS3_ROM_OK or one
 * of the errors above. */
int PS3_Rom_Load(const char *dir);

const uint8_t *PS3_Rom_Data(void);        /* NULL until PS3_Rom_Load succeeds */
const char    *PS3_Rom_Path(void);        /* file that was picked, "" if none */
uint32_t       PS3_Rom_Crc32(void);       /* of the file that was read */
int            PS3_Rom_IsStock(void);     /* 1 if it is the unmodified USA ROM */

/* What osPiStartDma becomes: copies `size` bytes at ROM offset `off`.
 * Returns 0, or -1 when the range is outside the ROM. */
int PS3_Rom_Read(void *dst, uint32_t off, uint32_t size);

#endif /* PS3_ROM_H */
