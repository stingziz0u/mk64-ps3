/*
 * The N64 loads two code segments from the ROM again each time they are
 * used, their variables with them: "racing" before every race (and at the
 * award ceremony) and "ending" before every award ceremony and the credits.
 * Here that code is part of the executable, so its variables kept the values
 * of the last race or ceremony: the second ceremony of a session started
 * with the karts already on the podium and the camera shots out of step.
 *
 * The Makefile puts the variables of those segments (the same objects as in
 * mk64.ld) in sections of their own. The initial .data is copied the first
 * time (after the ROM tables are in place) and put back on every load; the
 * .bss is cleared, as the N64's bzero does.
 */
#include <ultra64.h>
#include <stdlib.h>
#include <string.h>

#include "ps3_game.h"
#include "ps3_log.h"

#define SEG_SECTIONS(name)                                                                            \
    extern char __start_mk64_##name##_data[] __attribute__((weak));                                  \
    extern char __stop_mk64_##name##_data[] __attribute__((weak));                                   \
    extern char __start_mk64_##name##_bss[] __attribute__((weak));                                   \
    extern char __stop_mk64_##name##_bss[] __attribute__((weak));

SEG_SECTIONS(racing)
SEG_SECTIONS(ending)

typedef struct {
    const char* name;
    char *data, *dataEnd, *bss, *bssEnd;
    char* initial;   /* copy of the initial .data, made on the first load */
} ps3_segment_t;

static ps3_segment_t sSegments[] = {
    { "racing", __start_mk64_racing_data, __stop_mk64_racing_data, __start_mk64_racing_bss, __stop_mk64_racing_bss,
      NULL },
    { "ending", __start_mk64_ending_data, __stop_mk64_ending_data, __start_mk64_ending_bss, __stop_mk64_ending_bss,
      NULL },
};

void ps3_segment_reload(int which) {
    ps3_segment_t* seg = &sSegments[which];
    size_t dataSize = (seg->data && seg->dataEnd) ? (size_t) (seg->dataEnd - seg->data) : 0;
    size_t bssSize = (seg->bss && seg->bssEnd) ? (size_t) (seg->bssEnd - seg->bss) : 0;

    if (seg->initial == NULL) {
        seg->initial = malloc(dataSize ? dataSize : 1);
        if (seg->initial == NULL) {
            ps3_logf("[seg] %s: no memory for a copy of its %u bytes of data", seg->name, (unsigned) dataSize);
            return;
        }
        memcpy(seg->initial, seg->data, dataSize);
        ps3_logf("[seg] %s segment: %u bytes of data, %u of bss, reset on every load", seg->name, (unsigned) dataSize,
                 (unsigned) bssSize);
    } else {
        memcpy(seg->data, seg->initial, dataSize);
    }
    memset(seg->bss, 0, bssSize);
}
