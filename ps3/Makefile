#----------------------------------------------------------------------------
# mk64-ps3 -- Mario Kart 64 (n64decomp/mk64) for PS3 homebrew (PSL1GHT)
#
# Stage 3: the game is drawn on the RSX and plays its sound.
#
#   make                  -> .pkg
#   make clean
#   make check-toolchain
#
# The packaging (.elf -> .self -> .pkg) is done entirely by ppu_rules with its
# own pattern rules. Do NOT do it by hand: that skips the strip + sprxlinker
# before signing and package_finalize after pkg.py, and the result installs
# but does not run.
#----------------------------------------------------------------------------

ifeq ($(strip $(PS3DEV)),)
$(error PS3DEV is not set. Run: export PS3DEV=/usr/local/ps3dev)
endif

include $(PS3DEV)/ppu_rules

#----------------------------------------------------------------------------
# Package identity
#
# APPID: EXACTLY 9 characters. With 8 the PS3 pads it to 9 with "_" and every
# path built from it stops matching.
#
# ':=' and not '?=': ppu_rules defines TITLE and APPID with '?=' BEFORE these
# lines, so with '?=' its defaults would win.
#----------------------------------------------------------------------------
TARGET    := mk64ps3
TITLE     := mk64-ps3
APPID     := MK64PS300
CONTENTID := UP0001-$(APPID)_00-0000000000000000

PKGFILES  := $(CURDIR)/pkgfiles
ICON0     := $(CURDIR)/ICON0.PNG

CC := ppu-gcc
LD := ppu-gcc

# Goes up by one with every patch; first line of the log and VERSION.TXT.
PATCHLEVEL := 22
VERSION    := 1.0
OUT_PKG    := mk64-$(VERSION).pkg

ROOT := ..
OBJ  := build/obj

# make TEST=1: test build, every race lasts one lap. Same APPID (it replaces
# the normal one), its own objects, PKG and title so the two never mix.
TEST ?= 0
ifeq ($(TEST),1)
TARGET   := mk64ps3test
TITLE    := mk64-ps3 TEST
OBJ      := build/obj-test
PKGFILES := $(CURDIR)/pkgfiles-test
OUT_PKG  := mk64-$(VERSION)-test.pkg
endif

#----------------------------------------------------------------------------
# Platform side (source/*.c): PSL1GHT headers, never ultra64.h.
#----------------------------------------------------------------------------
PLAT_CFLAGS := -O2 -Wall -std=gnu11 -MMD -MP
PLAT_CFLAGS += -D__PS3__ -DPS3_APPID=\"$(APPID)\" -DPS3_PATCHLEVEL=$(PATCHLEVEL)
PLAT_CFLAGS += -Isource $(LIBPSL1GHT_INC)

# ps3_video.c goes at -O1 -fno-inline on purpose: PSL1GHT's syscall stubs are
# data indirection symbols, not normal functions, and aggressive inlining
# breaks their resolution at link time.
PLAT_CFLAGS_VIDEO := $(filter-out -O2,$(PLAT_CFLAGS)) -O1 -fno-inline

PLAT_SRCS := main.c ps3_log.c ps3_pad.c ps3_video.c ps3_rom.c ps3_plat.c ps3_rsx.c ps3_audio.c
PLAT_OBJS := $(PLAT_SRCS:%.c=$(OBJ)/plat/%.o)

#----------------------------------------------------------------------------
# Game side: the decompiled game (../src) + source/game/*.c. N64 headers
# (ultra64.h), never PSL1GHT's: both define u8/u16/u32.
#
# -fsigned-char         the game was written for a compiler with signed char;
#                       PowerPC's default is unsigned.
# -ffp-contract=off     no fused multiply-add: keeps the float results of the
#                       game logic the same as on the N64.
# -fno-strict-aliasing  1996 code.
# -fno-builtin          the game has its own bcopy/bzero/sqrtf prototypes.
#----------------------------------------------------------------------------
GAME_CFLAGS := -O2 -std=gnu90 -fsigned-char -fno-builtin -fno-strict-aliasing -ffp-contract=off -MMD -MP
GAME_CFLAGS += -D_LANGUAGE_C -DTARGET_PS3 -D__PS3__ -DGCC -DNON_MATCHING -DAVOID_UB
GAME_CFLAGS += -DVERSION_US=1 -DF3DEX_GBI=1 -DF3D_OLD=1 -DIS_BIG_ENDIAN=1 -DIS_64_BIT=0
GAME_CFLAGS += -Isource/game -Isource -Igen/include -Igen
GAME_CFLAGS += -I$(ROOT)/include -I$(ROOT)/src -I$(ROOT)/src/racing -I$(ROOT)/src/ending -I$(ROOT)
ifeq ($(TEST),1)
GAME_CFLAGS += -DMK64_TEST_ONE_LAP
PLAT_CFLAGS += -DMK64_TEST_ONE_LAP
endif

# The game code, as it is in the decompilation. Out: src/os (libultra, replaced
# by source/game/ps3_ultra.c) except the gu* matrix helpers, and the N64 crash
# screen.
GAME_SRCS := $(filter-out src/crash_screen.c, \
               $(patsubst $(ROOT)/%,%,$(wildcard $(ROOT)/src/*.c $(ROOT)/src/racing/*.c \
                                                $(ROOT)/src/ending/*.c $(ROOT)/src/audio/*.c)))
GAME_SRCS += $(patsubst $(ROOT)/%,%,$(wildcard $(ROOT)/src/os/gu*.c))
GAME_SRCS += src/buffers/audio_heap.c src/buffers/buffers.c src/buffers/gfx_output_buffer.c \
             src/buffers/memory_pool.c src/buffers/random.c src/buffers/trig_tables_bss.c
GAME_SRCS += src/data/kart_attributes.c src/data/path_spawn_metadata.c src/data/some_data.c
GAME_SRCS += courses/courseTable.c

# These three have Japanese text in string literals: the game expects EUC-JP,
# the source is UTF-8 (same conversion as the N64 build).
JP_SRCS   := src/ending/credits.c src/cpu_vehicles_camera_path.c src/menu_items.c
GAME_SRCS := $(filter-out $(JP_SRCS),$(GAME_SRCS))

GAME_OBJS := $(GAME_SRCS:%.c=$(OBJ)/game/%.o) $(JP_SRCS:%.c=$(OBJ)/jp/%.o)

# Small main-segment tables generated from the ROM by the decompilation's
# extractor (see gen/README).
GEN_SRCS := gen/assets/code/data_800E45C0/data_800E45C0.c gen/assets/code/data_800E8700/data_800E8700.c
GEN_OBJS := $(GEN_SRCS:%.c=$(OBJ)/%.o) $(OBJ)/gen/n64_symbols.o

SUP_SRCS := $(notdir $(wildcard source/game/*.c))
SUP_OBJS := $(SUP_SRCS:%.c=$(OBJ)/sup/%.o) $(OBJ)/sup/ps3_mem.o $(OBJ)/sup/libtkmk00.o

OBJS := $(PLAT_OBJS) $(SUP_OBJS) $(GEN_OBJS) $(GAME_OBJS)

# -lrt and -llv2 are MANDATORY: without them the binary links with no errors,
# the PKG installs, and opening it gives a black screen and back to the XMB
# without ever reaching main().
LIBS := gcm_sys rsx sysutil io audio rt lv2 m

.PHONY: all clean check-toolchain getlog

# The PKG to install is the .gnpdrm.pkg (the one that went through
# package_finalize), not the bare .pkg.
all: $(TARGET).pkg
	@cp $(TARGET).gnpdrm.pkg $(OUT_PKG)
	@echo ""
	@echo "PKG to install: $(CURDIR)/$(OUT_PKG)"
	@ls -la $(OUT_PKG)

$(OBJ)/plat/%.o: source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PLAT_CFLAGS) -c $< -o $@

# The patch level is printed by ps3_log.c: rebuild it when the Makefile changes.
$(OBJ)/plat/ps3_log.o: Makefile

$(OBJ)/plat/ps3_video.o: source/ps3_video.c
	@mkdir -p $(dir $@)
	$(CC) $(PLAT_CFLAGS_VIDEO) -c $< -o $@

# ps3_rsx.c touches GCM like ps3_video.c: same treatment.
$(OBJ)/plat/ps3_rsx.o: source/ps3_rsx.c
	@mkdir -p $(dir $@)
	$(CC) $(PLAT_CFLAGS_VIDEO) -c $< -o $@

$(OBJ)/sup/%.o: source/game/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -Wall -Wno-unused -c $< -o $@

$(OBJ)/sup/%.o: source/game/%.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

# TKMK00 decoder (title screen and menu backgrounds): the C version that the
# decompilation ships as a tool.
$(OBJ)/sup/libtkmk00.o: $(ROOT)/tools/libtkmk00.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -I$(ROOT)/tools -c $< -o $@

$(OBJ)/gen/%.o: gen/%.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(OBJ)/gen/%.o: gen/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -w -c $< -o $@

# The game code compiles with the warnings of 1996-style code; they are
# silenced so that only the warnings of OUR code show.
$(OBJ)/game/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -w -c $< -o $@

$(OBJ)/jp/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	iconv -f UTF-8 -t EUC-JP $< > $(@:.o=.jp.c)
	$(CC) $(GAME_CFLAGS) -w -I$(dir $<) -MT $@ -MF $(@:.o=.d) -c $(@:.o=.jp.c) -o $@

# --no-multi-toc: one TOC for the whole program. The game is small enough
# (the link fails if it ever is not) and it avoids the TOC-switching stubs.
$(TARGET).elf: $(OBJS)
	$(LD) $(OBJS) $(LIBPSL1GHT_LIB) -Wl,--no-multi-toc -Wl,-Map,build/$(TARGET).map -o $(TARGET).elf $(foreach lib,$(LIBS),-l$(lib))

#----------------------------------------------------------------------------
# PKG content and icon
#
# pkgfiles/ mirrors the PKG, NOT its USRDIR: what must end up in USRDIR goes
# in pkgfiles/USRDIR/. The game data does NOT go here: the user copies their
# own ROM to USRDIR by FTP.
#
# The directory cannot be EMPTY ('cp -rf $(PKGFILES)/*' fails), so VERSION.TXT
# is always written; it also tells which build is installed on the console.
#----------------------------------------------------------------------------
$(PKGFILES)/USRDIR/VERSION.TXT: $(OBJS)
	@mkdir -p $(PKGFILES)/USRDIR
	@printf 'aspect=4:3\nfps=30\nfpscounter=0\nscreenfit=100\nbind_a=CROSS\nbind_b=SQUARE\nbind_z=L1+L2\nbind_r=R1+R2\nbind_l=SELECT\nbind_start=START\nbind_c_up=TRIANGLE\nbind_c_down=\nbind_c_left=\nbind_c_right=CIRCLE\nbind_dpad_up=UP\nbind_dpad_down=DOWN\nbind_dpad_left=LEFT\nbind_dpad_right=RIGHT\n' > $(PKGFILES)/USRDIR/mk64.cfg
	@echo "$(TITLE) -- $(VERSION) -- patch $(PATCHLEVEL)$(if $(filter 1,$(TEST)), -- TEST: 1-lap races,) -- `date +%Y-%m-%d\ %H:%M`" > $@

# XMB icon (320x176 PNG). If it is missing, a plain one is generated so the
# build does not stop.
$(ICON0):
	@echo "No ICON0.PNG, generating a 320x176 placeholder..."
	@python3 -c "import zlib,struct;w,h=320,176;\
rows=b''.join(b'\x00'+bytes([200,30,30,255])*w for _ in range(h));\
png=lambda t,d:struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d));\
open('$(ICON0)','wb').write(b'\x89PNG\r\n\x1a\n'+png(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+png(b'IDAT',zlib.compress(rows))+png(b'IEND',b''))"

$(TARGET).pkg: $(PKGFILES)/USRDIR/VERSION.TXT $(ICON0)

-include $(OBJS:.o=.d)

check-toolchain:
	@echo "PS3DEV   = $(PS3DEV)"
	@echo "ppu_rules: `test -f $(PS3DEV)/ppu_rules && echo OK || echo MISSING`"
	@$(CC) --version | head -1
	@echo "APPID    = $(APPID)  (length: `printf '%s' '$(APPID)' | wc -c`, MUST be 9)"
	@for l in $(LIBS); do \
	   if [ -f "$(PS3DEV)/ppu/lib/lib$$l.a" ]; then printf "  OK      lib%s.a\n" "$$l"; \
	   elif $(CC) -print-file-name=lib$$l.a | grep -q /; then printf "  OK      lib%s.a (gcc sysroot)\n" "$$l"; \
	   else printf "  MISSING lib%s.a\n" "$$l"; fi; \
	 done

# Optional: fetch the log without leaving the terminal.
#   make getlog PS3IP=192.168.1.50
PS3USER ?= anonymous:anonymous
getlog:
	@test -n "$(PS3IP)" || (echo "Usage: make getlog PS3IP=<ip>"; exit 1)
	curl -s -u $(PS3USER) ftp://$(PS3IP)/dev_hdd0/game/$(APPID)/USRDIR/mk64_log.txt -o mk64_log.txt
	@cat mk64_log.txt

clean:
	rm -f mk64-ps3-*.pkg mk64ps3*.elf mk64ps3*.self mk64ps3*.pkg
	rm -rf build pkgfiles pkgfiles-test
