/*
 * Controller Pak on player 1's pad: the Time Trials ghosts.
 *
 * The pak is a 32 KB image in USRDIR (mk64.mpk) with the real Controller Pak
 * layout (the .mpk of N64 emulators): page 0 the ID area, pages 1-2 the inode
 * table and its copy, pages 3-4 the 16 notes, pages 5-127 the data. Created
 * formatted the first time; written back after every change.
 */

#include <ultra64.h>
#include <PR/os.h>
#include <string.h>

#include "ps3_game.h"
#include "ps3_plat.h"

#define PAK_FILE    "mk64.mpk"
#define PAK_SIZE    0x8000
#define PAGE        256
#define NPAGES      (PAK_SIZE / PAGE)
#define FIRST_DATA  5
#define NNOTES      16
#define INODE_OFS   (1 * PAGE)
#define INODE2_OFS  (2 * PAGE)
#define NOTE_OFS    (3 * PAGE)
#define INODE_END   0x0001
#define INODE_FREE  0x0003

static u8 sPak[PAK_SIZE];
static int sLoaded;

static unsigned rd16(const u8 *p) { return (p[0] << 8) | p[1]; }
static u32 rd32(const u8 *p) { return ((u32) p[0] << 24) | ((u32) p[1] << 16) | (p[2] << 8) | p[3]; }
static void wr16(u8 *p, unsigned v) { p[0] = (u8) (v >> 8); p[1] = (u8) v; }
static void wr32(u8 *p, u32 v) { p[0] = (u8) (v >> 24); p[1] = (u8) (v >> 16); p[2] = (u8) (v >> 8); p[3] = (u8) v; }

static unsigned inode(int page) { return rd16(&sPak[INODE_OFS + page * 2]); }
static void set_inode(int page, unsigned v) { wr16(&sPak[INODE_OFS + page * 2], v); }
static u8 *note(int n) { return &sPak[NOTE_OFS + n * 32]; }
static int note_used(int n) { return rd32(note(n)) != 0 && rd16(note(n) + 4) != 0; }

/* ID block: 28 bytes of data, then the sum of its 14 halfwords and 0xFFF2 - sum */
static void id_block(u8 *b)
{
    unsigned i, sum = 0;

    memset(b, 0, 32);
    memset(b, 0xFF, 4);
    memcpy(b + 4, "MK64PS3", 7);
    b[0x19] = 0x01;                     /* device id: Controller Pak */
    b[0x1A] = 0x01;                     /* one bank */
    for (i = 0; i < 28; i += 2) sum += rd16(b + i);
    wr16(b + 0x1C, sum & 0xFFFF);
    wr16(b + 0x1E, (0xFFF2 - sum) & 0xFFFF);
}

static void inode_commit(void)
{
    unsigned i, sum = 0;

    for (i = FIRST_DATA * 2; i < PAGE; i++) sum += sPak[INODE_OFS + i];
    sPak[INODE_OFS + 1] = (u8) sum;
    memcpy(&sPak[INODE2_OFS], &sPak[INODE_OFS], PAGE);
}

static void pak_format(void)
{
    int i;

    memset(sPak, 0, sizeof(sPak));
    id_block(&sPak[0x20]);
    memcpy(&sPak[0x60], &sPak[0x20], 32);
    memcpy(&sPak[0x80], &sPak[0x20], 32);
    memcpy(&sPak[0xC0], &sPak[0x20], 32);
    for (i = FIRST_DATA; i < NPAGES; i++) set_inode(i, INODE_FREE);
    inode_commit();
}

static void pak_save(void)
{
    if (PS3_Plat_SaveFile(PAK_FILE, sPak, PAK_SIZE) != PAK_SIZE) {
        ps3_log("[pak] WARNING: could not write " PAK_FILE);
    }
}

/* A chain that runs off the data pages or loops makes the pak unreadable;
 * then it is formatted again (the N64 would ask to repair it). */
static int pak_valid(void)
{
    int n, steps;
    unsigned p;

    for (n = 0; n < NNOTES; n++) {
        if (!note_used(n)) continue;
        p = rd16(note(n) + 6) & 0xFF;
        for (steps = 0; p != INODE_END; steps++) {
            if (p < FIRST_DATA || p >= NPAGES || steps >= NPAGES) return 0;
            p = inode((int) p);
        }
    }
    return 1;
}

static void pak_load(void)
{
    int n;

    if (sLoaded) return;
    sLoaded = 1;
    n = PS3_Plat_LoadFile(PAK_FILE, sPak, PAK_SIZE);
    if (n != PAK_SIZE || !pak_valid()) {
        ps3_logf("[pak] %s: %s, new formatted pak", PAK_FILE, n < 0 ? "not there yet" : "not valid");
        pak_format();
        pak_save();
    } else {
        ps3_logf("[pak] %s loaded", PAK_FILE);
    }
}

static int free_pages(void)
{
    int i, n = 0;

    for (i = FIRST_DATA; i < NPAGES; i++) n += inode(i) == INODE_FREE;
    return n;
}

static int file_pages(int fileNo)
{
    unsigned p = rd16(note(fileNo) + 6) & 0xFF;
    int n = 0;

    while (p != INODE_END && n < NPAGES) { n++; p = inode((int) p); }
    return n;
}

static int pak_ok(OSPfs *pfs)
{
    pak_load();
    return pfs != NULL && pfs->channel == 0 && (pfs->status & PFS_INITIALIZED);
}

s32 osPfsIsPlug(OSMesgQueue *mq, u8 *pattern)
{
    (void) mq;
    pak_load();
    *pattern = 0x01;                    /* player 1's pad only */
    return 0;
}

s32 osPfsInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    pak_load();
    memset(pfs, 0, sizeof(*pfs));
    pfs->queue = mq;
    pfs->channel = channel;
    if (channel != 0) return PFS_ERR_NOPACK;
    pfs->status = PFS_INITIALIZED;
    memcpy(pfs->id, &sPak[0x20], 32);
    memcpy(pfs->label, sPak, 32);
    pfs->dir_size = NNOTES;
    pfs->inode_table = 1;
    pfs->minode_table = 2;
    pfs->dir_table = 3;
    pfs->inode_start_page = FIRST_DATA;
    pfs->banks = 1;
    return 0;
}

s32 osPfsNumFiles(OSPfs *pfs, s32 *maxFiles, s32 *filesUsed)
{
    int n, used = 0;

    if (!pak_ok(pfs)) return PFS_ERR_NOPACK;
    for (n = 0; n < NNOTES; n++) used += note_used(n);
    *maxFiles = NNOTES;
    *filesUsed = used;
    return 0;
}

s32 osPfsFreeBlocks(OSPfs *pfs, s32 *bytesNotUsed)
{
    if (!pak_ok(pfs)) return PFS_ERR_NOPACK;
    *bytesNotUsed = free_pages() * PAGE;
    return 0;
}

s32 osPfsFileState(OSPfs *pfs, s32 fileNo, OSPfsState *state)
{
    const u8 *d;

    if (!pak_ok(pfs)) return PFS_ERR_NOPACK;
    if (fileNo < 0 || fileNo >= NNOTES || !note_used(fileNo)) return PFS_ERR_INVALID;
    d = note(fileNo);
    state->file_size = (u32) file_pages(fileNo) * PAGE;
    state->game_code = rd32(d);
    state->company_code = (u16) rd16(d + 4);
    memcpy(state->ext_name, d + 0x0C, 4);
    memcpy(state->game_name, d + 0x10, 16);
    return 0;
}

static int find(u16 company, u32 game, const u8 *name, const u8 *ext)
{
    int n;

    for (n = 0; n < NNOTES; n++) {
        const u8 *d = note(n);
        if (note_used(n) && rd32(d) == game && rd16(d + 4) == company &&
            memcmp(d + 0x10, name, 16) == 0 && memcmp(d + 0x0C, ext, 4) == 0) {
            return n;
        }
    }
    return -1;
}

s32 osPfsFindFile(OSPfs *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName, s32 *fileNo)
{
    int n;

    if (!pak_ok(pfs)) return PFS_ERR_NOPACK;
    n = find(companyCode, gameCode, gameName, extName);
    if (n < 0) return PFS_ERR_INVALID;
    *fileNo = n;
    return 0;
}

s32 osPfsDeleteFile(OSPfs *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName)
{
    int n, steps = 0;
    unsigned p, next;

    if (!pak_ok(pfs)) return PFS_ERR_NOPACK;
    n = find(companyCode, gameCode, gameName, extName);
    if (n < 0) return PFS_ERR_INVALID;
    for (p = rd16(note(n) + 6) & 0xFF; p != INODE_END && steps < NPAGES; p = next, steps++) {
        next = inode((int) p);
        set_inode((int) p, INODE_FREE);
    }
    memset(note(n), 0, 32);
    inode_commit();
    pak_save();
    ps3_logf("[pak] file %d deleted", n);
    return 0;
}

s32 osPfsAllocateFile(OSPfs *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName, int nbytes,
                      s32 *fileNo)
{
    int n, i, pages, prev = -1;
    u8 *d;

    if (!pak_ok(pfs)) return PFS_ERR_NOPACK;
    if (companyCode == 0 || gameCode == 0 || nbytes <= 0) return PFS_ERR_INVALID;
    if (find(companyCode, gameCode, gameName, extName) >= 0) return PFS_ERR_EXIST;
    pages = (nbytes + PAGE - 1) / PAGE;
    if (pages > free_pages()) return PFS_DATA_FULL;
    for (n = 0; n < NNOTES && note_used(n); n++) {}
    if (n == NNOTES) return PFS_DIR_FULL;

    d = note(n);
    memset(d, 0, 32);
    for (i = FIRST_DATA; i < NPAGES && pages > 0; i++) {
        if (inode(i) != INODE_FREE) continue;
        if (prev < 0) wr16(d + 6, (unsigned) i);   /* bank 0, page i */
        else set_inode(prev, (unsigned) i);
        memset(&sPak[i * PAGE], 0, PAGE);
        prev = i;
        pages--;
    }
    set_inode(prev, INODE_END);
    wr32(d, gameCode);
    wr16(d + 4, companyCode);
    d[8] = 0x02;                                   /* occupied */
    memcpy(d + 0x0C, extName, 4);
    memcpy(d + 0x10, gameName, 16);
    inode_commit();
    pak_save();
    *fileNo = n;
    ps3_logf("[pak] file %d created (%d bytes)", n, nbytes);
    return 0;
}

s32 osPfsReadWriteFile(OSPfs *pfs, s32 fileNo, u8 flag, int offset, int nbytes, u8 *buffer)
{
    unsigned p;
    int skip, done = 0, steps = 0;

    if (!pak_ok(pfs)) return PFS_ERR_NOPACK;
    if (fileNo < 0 || fileNo >= NNOTES || !note_used(fileNo)) return PFS_ERR_INVALID;
    if (offset < 0 || nbytes < 0 || (offset & 31) || (nbytes & 31)) return PFS_ERR_INVALID;
    if (offset + nbytes > file_pages(fileNo) * PAGE) return PFS_ERR_INVALID;

    p = rd16(note(fileNo) + 6) & 0xFF;
    for (skip = offset / PAGE; skip > 0; skip--) p = inode((int) p);
    offset %= PAGE;
    while (done < nbytes && p != INODE_END && steps++ < NPAGES) {
        int n = PAGE - offset;
        if (n > nbytes - done) n = nbytes - done;
        if (flag == PFS_WRITE) memcpy(&sPak[p * PAGE + offset], buffer + done, n);
        else memcpy(buffer + done, &sPak[p * PAGE + offset], n);
        done += n;
        offset = 0;
        p = inode((int) p);
    }
    if (flag == PFS_WRITE) pak_save();
    return 0;
}
