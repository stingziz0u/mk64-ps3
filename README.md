# mk64-ps3

Native port of **Mario Kart 64** to PS3 homebrew ([PSL1GHT](https://github.com/ps3dev/PSL1GHT)),
based on the [n64decomp/mk64](https://github.com/n64decomp/mk64) decompilation.

The repository has **no game assets**: everything is loaded at runtime from your own
copy of the game (USA ROM).

## Features

- Sound and music (the game's own audio engine, 48 kHz)
- Internal resolution scaled to 720p
- Aspect ratio: original 4:3 or 16:9 with the HUD anchored to the screen edges
- Frame rate: original 30 FPS or 60 FPS (interpolated)
- FPS counter and *Screen Fit* (overscan) options, in the game's Options menu
- 1 to 4 players: each extra DualShock 3 is the next player
- Controller Pak emulation for the Time Trials ghosts (`mk64.mpk`)
- Button rebinding in `mk64.cfg`
- Mod support: patched and renamed ROMs (see below)

## Installing

1. Install `mk64-1.0.pkg` on the console (Install Package Files).
2. Copy your **Mario Kart 64 (USA) `.z64`** ROM to the game's `USRDIR` by FTP:
   `/dev_hdd0/game/MK64PS300/USRDIR/`
3. Run *mk64-ps3* from the XMB.

The game creates `mk64.cfg` (settings), `mk64.eep` (save) and `mk64.mpk` (Controller Pak)
in the same folder. The log is written there too.

There is also a `-test` PKG (`make TEST=1`) where every race lasts one lap, to try all
the courses quickly. It uses the same APPID, so it replaces the normal one.

## Controls

Default mapping (every player):

| PS3 | N64 |
|---|---|
| Cross | A |
| Square | B |
| L1 / L2 | Z |
| R1 / R2 | R |
| Select | L |
| Start | Start |
| D-pad | D-pad |
| Triangle, Circle, right stick | C buttons |
| Left stick | Stick |

To change it, edit the `bind_*` lines of `mk64.cfg`, with button names joined by `+`:

```
bind_a=CROSS
bind_z=L1+L2
bind_c_down=
```

Names: `CROSS CIRCLE SQUARE TRIANGLE L1 L2 L3 R1 R2 R3 SELECT START UP DOWN LEFT RIGHT`.
An empty line leaves that N64 button unassigned.

## Mods and patched ROMs

The game accepts any file name, and a ROM that is not the original as long as it is a Mario
Kart 64 (USA) ROM (header game code `NKTE`), 12 to 64 MB, in .z64, .v64 or .n64 order.
If USRDIR has several ROMs, pick one with `rom=<file name>` in `mk64.cfg`.

On the N64 the game runs its data tables from the ROM: where each kart sprite and palette is,
where each course is, character stats, menu textures. The port rebuilds those tables from the
ROM that is loaded (599 of them; `ps3/gen/romdata_map.c`, made by `ps3/tools/gen_romdata_map.py`),
so mods that change them work. With the original ROM nothing changes. The log lists
every table a mod changed (`[mods] ... taken from the ROM`).

**Works:** replaced or recolored characters and karts, new menu and course textures, changed
course data (geometry, textures, path tables) and stats, new audio, new intro, as long as the
mod only changes data. A modified ROM can be read past its end (it gets zeros, as on the N64).

**Does not work:** mods that change the game's *code* (assembly hacks, new game logic): the port
runs the decompiled C, not the ROM's MIPS code. About 190 read-only tables are not rebuilt
either. The rest of the port (RSX renderer, audio, menus) assumes the original layout, so
heavily restructured ROMs may fail even if they load.

## Building

Needs the [ps3toolchain](https://github.com/ps3dev/ps3toolchain) (`PS3DEV` and `PSL1GHT` set).

```
cd ps3
make            # mk64-1.0.pkg
make TEST=1     # mk64-1.0-test.pkg
```

`ps3/hosttest/` has a software-rasterizer test that runs the game on a PowerPC host under
qemu (`qemu-ppc64`), used to test without the console.

## License / credits

Based on the work of the [n64decomp/mk64](https://github.com/n64decomp/mk64) project. Mario
Kart 64 is a trademark of Nintendo; this project contains none of its assets and you need
your own legally obtained copy of the game.

## AI disclosure

This project's code was written collaboratively with Claude (Anthropic), working through this port with me in real time over many sessions. Every bit of testing and debugging was done by me on real hardware.
