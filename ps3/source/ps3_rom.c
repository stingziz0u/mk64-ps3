/* ps3_rom.c -- see ps3_rom.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "ps3_rom.h"
#include "ps3_log.h"

static uint8_t *s_rom;
static char     s_path[1280];
static uint32_t s_crc;

const uint8_t *PS3_Rom_Data(void)  { return s_rom; }
const char    *PS3_Rom_Path(void)  { return s_path; }
uint32_t       PS3_Rom_Crc32(void) { return s_crc; }

int PS3_Rom_Read(void *dst, uint32_t off, uint32_t size)
{
    if (!s_rom || off > PS3_ROM_SIZE || size > PS3_ROM_SIZE - off) {
        ps3_logf("[rom] read outside the ROM: off 0x%X size 0x%X", off, size);
        return -1;
    }
    memcpy(dst, s_rom + off, size);
    return 0;
}

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    uint32_t c = 0xFFFFFFFFu;
    size_t i;

    if (!table[1]) {
        uint32_t k;
        int j;
        for (k = 0; k < 256; k++) {
            uint32_t v = k;
            for (j = 0; j < 8; j++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[k] = v;
        }
    }
    for (i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* 1 = .z64 (big-endian, what the game expects), 2 = .v64 (bytes swapped in
 * pairs), 3 = .n64 (little-endian words), 0 = not an N64 ROM. */
static int rom_byte_order(const uint8_t *h)
{
    if (h[0] == 0x80 && h[1] == 0x37 && h[2] == 0x12 && h[3] == 0x40) return 1;
    if (h[0] == 0x37 && h[1] == 0x80 && h[2] == 0x40 && h[3] == 0x12) return 2;
    if (h[0] == 0x40 && h[1] == 0x12 && h[2] == 0x37 && h[3] == 0x80) return 3;
    return 0;
}

static int has_ext(const char *name, const char *ext)
{
    size_t a = strlen(name), b = strlen(ext);
    return a > b && !strcasecmp(name + a - b, ext);
}

static int header_order(const char *path)
{
    uint8_t h[4];
    int order = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(h, 1, 4, f) == 4) order = rom_byte_order(h);
    fclose(f);
    return order;
}

/* By extension first; otherwise any file whose first four bytes are an N64
 * header in one of the three byte orders. */
static int find_rom(const char *dir, char *out, size_t outsz)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    if (!d) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (has_ext(e->d_name, ".z64") || has_ext(e->d_name, ".v64") || has_ext(e->d_name, ".n64")) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            found = 1;
        }
    }
    if (!found) {
        rewinddir(d);
        while (!found && (e = readdir(d)) != NULL) {
            char p[1280];
            struct stat st;
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0x1000) continue;
            if (header_order(p)) {
                snprintf(out, outsz, "%s", p);
                found = 1;
            }
        }
    }
    closedir(d);
    return found;
}

int PS3_Rom_Load(const char *dir)
{
    struct stat st;
    uint8_t *buf;
    FILE *f;
    size_t got;
    uint32_t i;
    int order;

    if (s_rom) return PS3_ROM_OK;
    s_path[0] = 0;
    s_crc = 0;

    if (!find_rom(dir, s_path, sizeof(s_path))) {
        ps3_logf("[rom] no .z64/.v64/.n64 and no file with an N64 header in %s", dir);
        return PS3_ROM_NOT_FOUND;
    }
    if (stat(s_path, &st) != 0) return PS3_ROM_READ_ERR;
    ps3_logf("[rom] %s, %u bytes", s_path, (unsigned)st.st_size);
    if ((uint32_t)st.st_size != PS3_ROM_SIZE) return PS3_ROM_BAD_SIZE;

    buf = (uint8_t *)malloc(PS3_ROM_SIZE);
    if (!buf) {
        ps3_log("[rom] malloc of 12 MB failed");
        return PS3_ROM_NO_MEMORY;
    }
    f = fopen(s_path, "rb");
    if (!f) { free(buf); return PS3_ROM_READ_ERR; }
    got = fread(buf, 1, PS3_ROM_SIZE, f);
    fclose(f);
    if (got != PS3_ROM_SIZE) {
        ps3_logf("[rom] short read: %u of %u bytes", (unsigned)got, PS3_ROM_SIZE);
        free(buf);
        return PS3_ROM_READ_ERR;
    }

    order = rom_byte_order(buf);
    ps3_logf("[rom] byte order: %s",
             order == 1 ? ".z64 (as is)" : order == 2 ? ".v64 (swapping pairs)" :
             order == 3 ? ".n64 (swapping words)" : "not an N64 ROM");
    if (order == 2) {
        for (i = 0; i < PS3_ROM_SIZE; i += 2) {
            uint8_t t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
        }
    } else if (order == 3) {
        for (i = 0; i < PS3_ROM_SIZE; i += 4) {
            uint8_t a = buf[i], b = buf[i + 1];
            buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2]; buf[i + 2] = b; buf[i + 3] = a;
        }
    } else if (order != 1) {
        free(buf);
        return PS3_ROM_BAD_GAME;
    }

    s_crc = crc32_buf(buf, PS3_ROM_SIZE);
    ps3_logf("[rom] internal name \"%.20s\", game code %.4s, version %u, crc32 %08X (expected %08X)",
             (const char *)buf + 0x20, (const char *)buf + 0x3B, buf[0x3F], s_crc, PS3_ROM_CRC32);
    if (s_crc != PS3_ROM_CRC32) {
        free(buf);
        return PS3_ROM_BAD_GAME;
    }

    s_rom = buf;
    return PS3_ROM_OK;
}
