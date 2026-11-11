/* ps3_romdata_map.h -- the data tables of the N64 code segments, rebuilt from
 * the ROM that is loaded (gen/romdata_map.c, written by
 * tools/gen_romdata_map.py). See ps3_romdata.c. */

#ifndef PS3_ROMDATA_MAP_H
#define PS3_ROMDATA_MAP_H

typedef struct {
    unsigned int ptr;     /* 0: copy n bytes; 1: n N64 words into 8-byte pointers */
    unsigned int ps3Off;
    unsigned int n64Off;
    unsigned int n;
} ps3_romdata_op;

typedef struct {
    const char* name;
    char* dest;
    unsigned int romOff;  /* where the table is in the ROM */
    unsigned int size;    /* its size there */
    const ps3_romdata_op* ops;
    int nops;
} ps3_romdata_entry;

extern const ps3_romdata_entry gPs3RomDataMap[];
extern const int gPs3RomDataMapCount;

#endif
