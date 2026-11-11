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
static uint32_t s_size = PS3_ROM_SIZE;
static int      s_stock;

const uint8_t *PS3_Rom_Data(void)  { return s_rom; }
const char    *PS3_Rom_Path(void)  { return s_path; }
uint32_t       PS3_Rom_Crc32(void) { return s_crc; }
int            PS3_Rom_IsStock(void) { return s_stock; }

int PS3_Rom_Read(void *dst, uint32_t off, uint32_t size)
{
    if (!s_rom || off >= PS3_ROM_MAX || size > PS3_ROM_MAX - off) {
        ps3_logf("[rom] read outside the cartridge space: off 0x%X size 0x%X", off, size);
        return -1;
    }
    /* Past the end of the file: zeros. The N64 reads whatever the bus gives
     * there, and modified ROMs do it (a menu texture of unknown size is read
     * as 0x1400 bytes, even if it is the last thing in the ROM). */
    if (off >= s_size) {
        memset(dst, 0, size);
    } else if (size > s_size - off) {
        memcpy(dst, s_rom + off, s_size - off);
        memset((uint8_t *)dst + (s_size - off), 0, size - (s_size - off));
    } else {
        memcpy(dst, s_rom + off, size);
    }
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

/* rom=<file name> in USRDIR/mk64.cfg: which file to load when there are
 * several (the stock ROM and a hack, say). */
static int cfg_rom(const char *dir, char *out, size_t outsz)
{
    char path[1280], buf[2048], *p;
    struct stat st;
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), "%s/mk64.cfg", dir);
    f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    for (p = buf; p && *p; ) {
        char *eol = strpbrk(p, "\r\n");
        if (eol) *eol = 0;
        if (strncmp(p, "rom=", 4) == 0 && p[4]) {
            char name[256];
            char *q = p + 4;
            size_t l;

            while (*q == ' ') q++;
            snprintf(name, sizeof(name), "%s", q);
            l = strlen(name);
            while (l && (name[l - 1] == ' ' || name[l - 1] == '\t')) name[--l] = 0;
            if (l && !strchr(name, '/')) {
                snprintf(path, sizeof(path), "%s/%s", dir, name);
                if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                    snprintf(out, outsz, "%s", path);
                    return 1;
                }
                ps3_logf("[rom] mk64.cfg: rom=%s is not in %s, looking for another", name, dir);
            }
        }
        p = eol ? eol + 1 : NULL;
        while (p && (*p == '\r' || *p == '\n')) p++;
    }
    return 0;
}

/* The file named in mk64.cfg; else by extension; otherwise any file whose
 * first four bytes are an N64 header in one of the three byte orders. */
static int find_rom(const char *dir, char *out, size_t outsz)
{
    DIR *d;
    struct dirent *e;
    int found = 0;

    if (cfg_rom(dir, out, outsz)) return 1;
    d = opendir(dir);
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
    uint32_t size;

    if (s_rom) return PS3_ROM_OK;
    s_path[0] = 0;
    s_crc = 0;

    if (!find_rom(dir, s_path, sizeof(s_path))) {
        ps3_logf("[rom] no .z64/.v64/.n64 and no file with an N64 header in %s", dir);
        return PS3_ROM_NOT_FOUND;
    }
    if (stat(s_path, &st) != 0) return PS3_ROM_READ_ERR;
    ps3_logf("[rom] %s, %u bytes", s_path, (unsigned)st.st_size);
    if ((uint32_t)st.st_size < PS3_ROM_SIZE || (uint32_t)st.st_size > PS3_ROM_MAX) return PS3_ROM_BAD_SIZE;
    size = (uint32_t)st.st_size & ~3u;

    buf = (uint8_t *)malloc(size);
    if (!buf) {
        ps3_logf("[rom] malloc of %u MB failed", (unsigned)(size >> 20));
        return PS3_ROM_NO_MEMORY;
    }
    f = fopen(s_path, "rb");
    if (!f) { free(buf); return PS3_ROM_READ_ERR; }
    got = fread(buf, 1, size, f);
    fclose(f);
    if (got != size) {
        ps3_logf("[rom] short read: %u of %u bytes", (unsigned)got, (unsigned)size);
        free(buf);
        return PS3_ROM_READ_ERR;
    }

    order = rom_byte_order(buf);
    ps3_logf("[rom] byte order: %s",
             order == 1 ? ".z64 (as is)" : order == 2 ? ".v64 (swapping pairs)" :
             order == 3 ? ".n64 (swapping words)" : "not an N64 ROM");
    if (order == 2) {
        for (i = 0; i < size; i += 2) {
            uint8_t t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
        }
    } else if (order == 3) {
        for (i = 0; i < size; i += 4) {
            uint8_t a = buf[i], b = buf[i + 1];
            buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2]; buf[i + 2] = b; buf[i + 3] = a;
        }
    } else if (order != 1) {
        free(buf);
        return PS3_ROM_BAD_GAME;
    }

    s_crc = crc32_buf(buf, size);
    ps3_logf("[rom] internal name \"%.20s\", game code %.4s, version %u, crc32 %08X (expected %08X)",
             (const char *)buf + 0x20, (const char *)buf + 0x3B, buf[0x3F], s_crc, PS3_ROM_CRC32);
    /* The stock ROM, or a hack of it: the game code of the header stays NKTE
     * (Mario Kart 64, USA), and the file keeps the layout the port reads. */
    s_stock = (s_crc == PS3_ROM_CRC32 && size == PS3_ROM_SIZE);
    if (!s_stock) {
        if (memcmp(buf + 0x3B, "NKTE", 4) != 0) {
            free(buf);
            return PS3_ROM_BAD_GAME;
        }
        ps3_log("[rom] not the stock ROM: a hack or a patched ROM, accepted (game code NKTE)");
    }

    s_size = size;
    s_rom = buf;
    return PS3_ROM_OK;
}
