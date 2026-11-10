/* ps3_mem.S -- the memory the N64 linker script laid out by hand.
 *
 * On the N64 the game heap is the RAM between the end of the main segment's
 * bss and the "racing" code segment: the game takes memory upwards from
 * _memoryPoolSegmentNoloadStart and downwards from _racingSegmentStart
 * (gHeapEndPtr). The "ending" segment sits in between and is only loaded for
 * the award ceremony and the credits.
 *
 *   N64:  pool start 0x801978D0, pool end +0xAB630, ending +0xE8730,
 *         racing +0xF6630  (about 1 MB in all)
 *
 * Here the code segments are part of the executable, so the labels only mark
 * the limits of the heap. It is 1 MB larger than on the N64 because structures
 * with native (64-bit) pointers are bigger.
 */

#define HEAP_EXTRA  0x100000

    .section .bss
    .balign 16

    .globl _memoryPoolSegmentNoloadStart
    .globl _memoryPoolSegmentNoloadEnd
    .globl _endingSegmentStart
    .globl _endingSegmentNoloadEnd
    .globl _racingSegmentStart
    .globl _racingSegmentNoloadEnd
_memoryPoolSegmentNoloadStart:
    .space 0xAB630
_memoryPoolSegmentNoloadEnd:
    .space 0xE8730 - 0xAB630 + HEAP_EXTRA
_endingSegmentStart:
    .space 0xDF00
_endingSegmentNoloadEnd:
_racingSegmentStart:
    .space 0x100
_racingSegmentNoloadEnd:

/* Trig tables: 0x5810 bytes read from the ROM by setup_game_memory(). */
    .balign 16
    .globl _trigTablesSegmentStart
    .globl gSineTable
    .globl gArctanTable
_trigTablesSegmentStart:
gSineTable:
    .space 0x5000
gArctanTable:
    .space 0x810
