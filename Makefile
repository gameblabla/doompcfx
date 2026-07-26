# Makefile — DOOM for the NEC PC-FX (V810 + KING/HuC6272).
#
# Engine: GBADoom-derived portable core (src/) compiled little-endian for the
# V810. Platform backend: platform/ (KING video, VCE palette, FX-Pad, ADPCM/CD-DA
# hooks). WAD: baked little-endian by tools/bake_wad.py into src/iwad and linked
# straight into the program image (loaded to RAM at boot == "WAD in memory").
#
# Toolchain: /opt/v810-gcc (GCC 4.9.4 v810) + liberis + pcfx-cdlink.

V810      ?= /opt/v810-gcc
CC        := $(V810)/bin/v810-gcc
LD        := $(V810)/bin/v810-ld
OBJCOPY   := $(V810)/bin/v810-objcopy
# Vendored pcfx-cdlink, built from source in tools/pcfxtools. It adds
# append/lbaheader (embeds non-RAM CD assets like the RAINBOW sky and emits
# their LBAs) plus cddaheader/cddadir (converts the 3DOMusic WAVs to Red Book
# audio tracks and emits their track numbers). The /opt system build has none of
# these, and the older checked-in ./tools/pcfx-cdlink binary lacks the CD-DA
# half — it produces a byte-identical data track but silently drops the music.
CDLINK    ?= ./tools/pcfxtools/pcfx-cdlink

TARGET    := doom_pcfx
OBJDIR    := build

# libpcfx (successor to liberis, r20-safe SCSI + fast delays) is the platform
# library now.  Keep it in-tree so projects which still need liberis are
# unaffected; its include directory goes before the toolchain's so eris/cd.h
# resolves to libpcfx rather than stale liberis headers.
LIBPCFX   ?= ../libpcfx
INCLUDES  := -Iinclude -Iplatform -Isrc -Isrc/generated \
             -I$(LIBPCFX)/include \
             -I$(V810)/include -I$(V810)/v810/include

# Dense 32x64x8 wall/flat pages are optional at compile time. Setting this to 0
# removes their metadata, caches and draw paths. The CD IWAD still carries the
# optional pages so the checked-in map-pack manifest stays build-independent.
PCFX_TEXTURE_PAGES ?= 1
PCFX_TEXTURE_CPPFLAG := $(if $(filter 1,$(PCFX_TEXTURE_PAGES)),-DPCFX_TEXTURE_PAGES,)

# -mv810: V810 (little-endian). -msda=0: no small-data area (BIOS reserves it).
# -mno-prolog-function keeps ABI-correct save/restore code in each function and
# avoids libgcc's high-frequency __save/__load trampoline calls. -w silences the
# 20-year-old engine's warning storm (real issues are caught by link/run).
# -MMD -MP: emit .d header-dependency files so editing a header (pcfx.h, doomdef.h,
# ...) actually recompiles every .c that includes it. Without this, header edits
# silently link stale objects.
# -DHAVE_GENERATED_LBAS once the CD-linker has emitted src/generated/lbas.h (the
# sky's on-disc sector). The multi-pass `cd` target below links, regenerates the
# header, then recompiles so the real LBA is baked into the RAINBOW loader.
CFLAGS    := -O2 -fomit-frame-pointer -fno-builtin -ffunction-sections \
             -fdata-sections -std=gnu99 -mv810 -mno-prolog-function -msda=0 -w \
             -MMD -MP $(INCLUDES) \
             $(if $(wildcard src/generated/lbas.h),-DHAVE_GENERATED_LBAS,) \
             $(if $(wildcard src/generated/cdda_tracks.h),-DHAVE_CDDA_TRACKS,) \
             $(PCFX_TEXTURE_CPPFLAG) $(EXTRA)
# EXTRA lets you pass e.g. EXTRA=-DDEV_WARP to boot straight into E1M1 (run
# `make clean` when switching, since object CFLAGS aren't dependency-tracked).

# Link with v810-ld directly: toolchain crt0.o first, default v810.x script
# (load 0x8000, heap up to 2 MB), then --gc-sections to drop unused engine code.
LDFLAGS   := -L$(LIBPCFX) \
             -L$(V810)/lib -L$(V810)/v810/lib -L$(V810)/lib/gcc/v810/4.9.4 \
             -T platform/pcfx_hot.ld \
             $(V810)/v810/lib/crt0.o --gc-sections \
             --undefined=_pcfx_r_subsector_layout_pad
# crt0.o is byte-identical between liberis and libpcfx, so the toolchain copy is
# kept. -lpcfx replaces all former liberis/lib7up facilities.
LIBS      := -lpcfx -lc -lsim -lgcc

# Generated VDC weapon-sprite tables (see the pcfx_weapons rule below). Listed
# explicitly (not via wildcard) so a clean tree still links it before it exists.
WEP_C     := src/generated/pcfx_weapons.c
# Generated VDC BG font tiles (see the pcfx_font rule below), used by platform/pcfx_text.c.
FONT_C    := src/generated/pcfx_font.c
CSRC      := $(sort $(wildcard src/*.c)) $(sort $(wildcard platform/*.c)) $(WEP_C) $(FONT_C)
# Hand-written V810 assembly (e.g. the LZ4 depacker for CD-streamed assets).
# libpcfx's DMA CD APIs preserve the ABI and are called directly.
ASRC      := $(sort $(wildcard platform/*.S))
OBJ       := $(patsubst %.c,$(OBJDIR)/%.o,$(CSRC)) $(patsubst %.S,$(OBJDIR)/%.o,$(ASRC))

# Inline prologues cost text space. Keep frame-critical renderer/game-tic code
# at its established optimization level, and recover the remaining image space
# from front-end, level-setup and event-driven special code instead. Together
# with the 16-bit runtime viewangletox table this preserves the linker's 6 KiB
# stack gap without reducing the viewport or reclaiming render/weapon caches.
#
# The sector-special thinkers and p_inter were added to this list to pay for the
# CD-DA music code (libpcfx cdda.o + the track tables in i_sound_pcfx.c, ~1.3 KiB
# of image). They fit the same rationale: a door, lift or pickup only runs code
# while that thing is actually happening, so they are nowhere near the per-frame
# cost of the renderer, and -Os on them keeps ZONE_SIZE_KB — and therefore the
# one-chunk map-pack arena that every E1 map depends on (src/z_zone.c) — untouched.
#
# Watch the margin here: .pcfx_renderseg is ALIGN(1024), so .text growth is not
# priced smoothly. Crossing a 1 KiB boundary steps .rodata/.data/.bss up with it
# and costs ~2 KiB of __end at once (the ASSERT in platform/pcfx_hot.ld is what
# catches it). ~150 bytes of new .text can therefore blow a 700-byte margin —
# check `v810-nm build/doom_pcfx.elf | grep __end` rather than guessing from
# object sizes.
SIZE_OPT_OBJ := $(addprefix $(OBJDIR)/src/,\
                am_map.o f_finale.o m_menu.o p_setup.o p_switch.o r_data.o wi_stuff.o \
                p_ceilng.o p_doors.o p_floor.o p_genlin.o p_lights.o p_plats.o p_telept.o \
                p_inter.o)
$(SIZE_OPT_OBJ): CFLAGS += -Os

ELF       := $(OBJDIR)/$(TARGET).elf
BIN       := $(TARGET).bin
MAP       := $(OBJDIR)/$(TARGET).map

.PHONY: all cd zip clean
all: cd

# --- Build identity stamp ----------------------------------------------------
# Every hardware photo must be attributable to an exact build: two 2026-07-24
# burns were misdiagnosed because the disc didn't match the assumed tree
# (detached HEAD at e085ed8; uncommitted WIP). Format "DOOMHASH LLIBHASH DDHHMM",
# 'M' suffix = dirty tree; uppercased because the boot font is 0-9A-Z only.
# Regenerated every make via FORCE; only rewritten (and pcfx_boot.o only
# rebuilt) when the text actually changes.
BUILDID_H := src/generated/buildid.h
$(BUILDID_H): FORCE
	@mkdir -p src/generated
	@d=$$(git rev-parse --short=7 HEAD 2>/dev/null || echo NOGIT); \
	git update-index -q --refresh 2>/dev/null; \
	git diff-index --quiet HEAD -- 2>/dev/null || d="$${d}M"; \
	l=$$(git -C ../libpcfx rev-parse --short=7 HEAD 2>/dev/null || echo NOGIT); \
	git -C ../libpcfx update-index -q --refresh 2>/dev/null; \
	git -C ../libpcfx diff-index --quiet HEAD -- 2>/dev/null || l="$${l}M"; \
	id=$$(printf '%s L%s %s' "$$d" "$$l" "$$(date +%d%H%M)" | tr 'a-z' 'A-Z'); \
	printf '#define PCFX_BUILD_ID "%s"\n' "$$id" > $@.tmp; \
	if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(OBJDIR)/platform/pcfx_boot.o: $(BUILDID_H)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble .S through the C driver (runs the preprocessor first); C-only flags are
# ignored for assembly. -MMD emits a .d so header/asm edits recompile.
$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# --- WAD assets --------------------------------------------------------------
# The IWAD is NO LONGER baked into the program image. The full doom1.wad (all 9
# E1 maps + full graphics, ~1.8 MB of gfx) cannot fit 2 MB RAM, so bake_wad
# --cd-wad packs it sector-aligned onto the CD and w_wad.c streams the directory
# + lumps at runtime (docs/cd-streaming-plan.md, Phases 2+3). The same PC-FX
# transforms still apply (index-0 remap, colormap-hue repair, GBA HUD digits,
# STBARFX 256px bar, full-bleed 256x240 TITLEPIC).
# Set JAGUAR_MAPS=0 to force PC maps, or JAGUAR_MAPS=1 (optionally with
# JAGUAR_WAD=/path/to/jaguartc.wad) to force the Jaguar-map overlay. In auto
# mode, the default, a local jaguartc.wad is used when it is available. Only map
# groups already present in DOOM1WAD are overlaid: a shareware doom1.wad remains
# episode one, while a registered IWAD keeps PC fallbacks for missing Jaguar maps.
DOOM1WAD  ?= ../doom1.wad
JAGUAR_WAD  ?= jaguartc.wad
JAGUAR_MAPS ?= auto
ifeq ($(JAGUAR_MAPS),auto)
JAGUAR_MAPS := $(if $(wildcard $(JAGUAR_WAD)),1,0)
endif
ifneq ($(filter 0 1,$(JAGUAR_MAPS)),$(JAGUAR_MAPS))
$(error JAGUAR_MAPS must be auto, 0, or 1 (got $(JAGUAR_MAPS)))
endif
ifeq ($(JAGUAR_MAPS),1)
ifeq ($(wildcard $(JAGUAR_WAD)),)
$(error JAGUAR_MAPS=1 but JAGUAR_WAD is not readable: $(JAGUAR_WAD))
endif
JAGUAR_MAP_ARGS := --replace-maps $(JAGUAR_WAD)
JAGUAR_MAP_DEP  := $(JAGUAR_WAD)
else
JAGUAR_MAP_ARGS :=
JAGUAR_MAP_DEP  :=
endif
IWAD_SRC  ?= assets/newdoom1_1lev.wad
HUD_PWAD  := generated/hudfx.wad
HUD_BAR   := assets/stbar.png
TITLE_SRC := assets/doom256.png
TITLE_PWAD:= generated/titlefx.wad
CDWAD_BIN := src/generated/pcfx_iwad.bin

# Switching JAGUAR_MAPS mode changes no source-file timestamp, so make would
# otherwise reuse a stale WAD built for the other mode (and the checked-in map
# packs would no longer match its texture table). Record the mode string in a
# stamp file, rewritten only when it actually changes, and make the WAD depend on
# it — so `make JAGUAR_MAPS=0` / `=1` always rebuilds the WAD for the new mode.
# NOTE: the reserve manifest (mappack-manifest) is mode-specific too; regenerate
# it after a mode switch (the checked-in one matches the default, Jaguar) build.
JAGUAR_STAMP := src/generated/.jaguar_mode
.PHONY: FORCE
FORCE:
$(JAGUAR_STAMP): FORCE
	@mkdir -p src/generated
	@printf '%s' '$(JAGUAR_MAPS):$(JAGUAR_WAD)' | cmp -s - $@ 2>/dev/null || \
	  printf '%s' '$(JAGUAR_MAPS):$(JAGUAR_WAD)' > $@
MERGE_HUD := STGANUM0,STGANUM1,STGANUM2,STGANUM3,STGANUM4,STGANUM5,STGANUM6,STGANUM7,STGANUM8,STGANUM9

$(HUD_PWAD): tools/gen_pcfx_hud.py $(HUD_BAR)
	@mkdir -p generated
	python3 tools/gen_pcfx_hud.py $(HUD_BAR) $@

# Full-bleed 256x240 TITLEPIC baked from assets/doom256.png (drawn 1:1, no stretch).
$(TITLE_PWAD): tools/gen_pcfx_title.py $(TITLE_SRC) $(IWAD_SRC)
	@mkdir -p generated
	python3 tools/gen_pcfx_title.py $(TITLE_SRC) $(IWAD_SRC) $@

# Title source: the local 256x240 PNG when present (else doom1.wad's own TITLEPIC).
TITLE_REPLACE := $(if $(wildcard $(TITLE_SRC)),--replace $(TITLE_PWAD):TITLEPIC,)
TITLE_DEP     := $(if $(wildcard $(TITLE_SRC)),$(TITLE_PWAD),)

# The sector-aligned CD IWAD blob (cdlink `append`s it; w_wad.c reads it by LBA).
$(CDWAD_BIN): tools/bake_wad.py tools/lz4_block.py tools/pcfx_sum32.py $(DOOM1WAD) $(JAGUAR_MAP_DEP) $(JAGUAR_STAMP) assets/gbadoom.wad $(HUD_PWAD) $(TITLE_DEP)
	@mkdir -p src/generated
	python3 tools/bake_wad.py $(DOOM1WAD) $@ --endian little --cd-lz4 \
	  --drop-audio --drop-demos --fix-index0 --pcfx-textures \
	  --merge assets/gbadoom.wad:$(MERGE_HUD) \
	  --merge $(HUD_PWAD):STBARFX \
	  $(TITLE_REPLACE) $(JAGUAR_MAP_ARGS)

# CD-DMA matrix self-test (EXTRA=-DDEV_CD_MATRIX) reference: build-time CRC32
# of the first CDMATRIX_SECTORS sectors of the CD IWAD blob. The on-console
# test reads that exact extent (BINARY_LBA_SRC_GENERATED_PCFX_IWAD_BIN) through
# every arm-shape/path/chunking/KRAM-region cell and compares against this
# ground truth — which no read path can influence, unlike a read-path-derived
# reference. Cheap, so it is generated unconditionally.
#
# Two extents, because the size is itself a variable under test. REG.0A is a
# 17-bit even byte counter the manual bounds at 2..128K (max 0x1FFFE = 131070
# bytes), so the SMALL extent (16 KiB) fits in a single arm of any shape while
# the BIG one (64 sectors = 131072 bytes) exceeds the counter's reach by two
# bytes and therefore CANNOT be read by any single arm — only by a path that
# chunks correctly, which is the case every real asset load in this port hits.
#
# 64 sectors is also the largest extent that fits in POPULATED KRAM: the A16=1
# half of each bit-17 half is unpopulated on real silicon (pcfx_kram.h), so the
# contiguous runs are words 0x00000..0x0FFFF and 0x20000..0x2FFFF, the second
# of which is exactly 0x10000 words = 128 KiB. The earlier 96-sector (192 KiB)
# extent ran off the end of both and would have failed for reasons that have
# nothing to do with the arm shape under test.
CDMATRIX_H           := src/generated/cdmatrix_ref.h
CDMATRIX_SECTORS     := 8
CDMATRIX_BIG_SECTORS := 64
# Makefile is a prerequisite: the extents are defined right here, and a stale
# reference header silently CRCs the wrong number of sectors.
$(CDMATRIX_H): $(CDWAD_BIN) Makefile
	@mkdir -p src/generated
	python3 -c "import zlib,sys; \
	d=open('$(CDWAD_BIN)','rb').read($(CDMATRIX_BIG_SECTORS)*2048); \
	assert len(d)==$(CDMATRIX_BIG_SECTORS)*2048; \
	print('#define CDMATRIX_REF_CRC 0x%08xu' % (zlib.crc32(d[:$(CDMATRIX_SECTORS)*2048])&0xffffffff)); \
	print('#define CDMATRIX_SECTORS $(CDMATRIX_SECTORS)u'); \
	print('#define CDMATRIX_BIG_CRC 0x%08xu' % (zlib.crc32(d)&0xffffffff)); \
	print('#define CDMATRIX_BIG_SECTORS $(CDMATRIX_BIG_SECTORS)u')" > $@

$(OBJDIR)/platform/pcfx_cdmatrix.o: $(CDMATRIX_H) $(BUILDID_H)

# Per-map contiguous asset packs (PSX-Doom style): each map's whole precache set,
# gap-free on the CD, so the runtime streams it into the arena in one ~0-seek sweep
# (see platform/pcfx_mappack.c, tools/gen_pcfx_packs.py). Built from the CD-WAD blob
# (same compressed payloads) + the checked-in reserve manifest. cdlink `append`s it.
MAPPACK_BIN := src/generated/pcfx_mappacks.bin
MANIFEST    := generated/mappack_manifest.txt
$(MAPPACK_BIN): tools/gen_pcfx_packs.py tools/pcfx_sum32.py $(CDWAD_BIN) $(MANIFEST)
	@mkdir -p src/generated
	python3 tools/gen_pcfx_packs.py $(CDWAD_BIN) $(MANIFEST) $@

# Regenerate the reserve manifest by dumping the engine's own R_PrecacheLevel set for
# every E1 map (headless). Slow (one build+run per map) and only needed when the WAD,
# selected map set, or precache logic changes — the result is checked in. The variables
# are passed through so `make mappack-manifest` refreshes Jaguar-map asset packs too.
.PHONY: mappack-manifest
mappack-manifest:
	JAGUAR_MAPS=$(JAGUAR_MAPS) JAGUAR_WAD=$(JAGUAR_WAD) tools/gen_mappack_manifest.sh $(DOOM1WAD)

# CD-streamed full-screen backgrounds (intermission map + episode-1 finale). The
# squashed WAD ships brick-texture placeholders and baking the real 320x200
# pictures into the RAM image starves the zone heap, so these are rendered to
# native 256x240, packed as framebuffer words, and put on the CD as a sector-
# aligned blob (see docs/cd-streaming-plan.md). Read into a transient buffer at
# runtime by platform/pcfx_cdasset.c. Falls back to an empty catalog (placeholders
# stay) without doom1.wad. WIMAP0 = intermission, HELP2 = episode-end finale.
CDA_H    := src/generated/pcfx_cdassets.h
CDA_BIN  := src/generated/pcfx_cdassets.bin
CDA_WAD  := $(if $(wildcard $(DOOM1WAD)),$(DOOM1WAD),)
# raw:TITLEPIC is stored UNCOMPRESSED so the title streams CD->KRAM framebuffer by DMA
# (no LZ4 decode / CPU blit — platform/pcfx_cdasset.c pcfx_cd_background_dma). It is
# sourced from the enhanced 256x240 art assets/doom256.png (PLAYPAL-indexed), NOT the
# IWAD TITLEPIC lump. WIMAP0/HELP2 stay LZ4 from the WAD.
# The title is DMA'd once per page and never touched again (the menu cursor/text
# ride the VDC overlay, not the framebuffer — src/m_menu.c, src/d_main.c
# D_PageDrawer), so no LZ4 copy of the title is needed. WIMAP0/HELP2 are the
# intermission/finale backgrounds, still LZ4 (pcfx_cd_background).
TITLE_PNG := assets/doom256.png
CDA_LUMPS:= $(if $(CDA_WAD),raw:TITLEPIC=$(TITLE_PNG) raw:WIMAP0 raw:HELP2,)
# Depend on the Makefile so editing CDA_LUMPS (the asset list) forces a regen — the
# generated catalog is otherwise only rebuilt when the tool or the PNG changes.
$(CDA_H) $(CDA_BIN): tools/gen_pcfx_cdassets.py $(TITLE_PNG) Makefile
	@mkdir -p src/generated
	python3 tools/gen_pcfx_cdassets.py $(if $(CDA_WAD),$(CDA_WAD),none) $(CDA_H) $(CDA_LUMPS)
$(OBJDIR)/platform/pcfx_cdasset.o: $(CDA_H)

# ADPCM SFX bank baked from the full IWAD's DS* sounds (the squashed 1-level WAD's
# sounds are near-silent), indexed by Doom sfx id. Loaded to KRAM at boot.
SFX_H   := src/generated/pcfx_sfx.h
SFX_BIN := src/generated/pcfx_sfx.bin
SFX_WAD := $(if $(wildcard $(DOOM1WAD)),$(DOOM1WAD),$(IWAD_SRC))
# The raw ADPCM bank is a CD asset (see `append` in cdlink.txt) DMA'd to KRAM at
# boot, NOT baked into the RAM image — that reclaims ~80 KB of .rodata for the
# zone heap (needed so a whole level's graphics stay resident, no in-game CD).
$(SFX_H) $(SFX_BIN): tools/gen_pcfx_sfx.py src/sounds.c
	@mkdir -p src/generated
	python3 tools/gen_pcfx_sfx.py $(SFX_WAD) src/sounds.c $(SFX_H) $(SFX_BIN)
$(OBJDIR)/platform/i_sound_pcfx.o: $(SFX_H)

# RAINBOW sky: Doom SKY1 -> HuC6271 RAINBOW YUV/DCT stream. NOT linked into the
# program — emitted as a raw .bin, embedded on the CD (see `append` in cdlink.txt),
# and DMA'd into KRAM at runtime (the HuC6271 only decodes CD-DMA'd KRAM data).
SKY_H   := src/generated/pcfx_sky.h
SKY_BIN := src/generated/pcfx_sky.bin
$(SKY_H) $(SKY_BIN): tools/gen_pcfx_sky.py tools/gen_pcfx_rainbow_bg.py
	@mkdir -p src/generated
	python3 tools/gen_pcfx_sky.py $(SFX_WAD) $(SKY_H)
$(OBJDIR)/platform/i_system_pcfx.o: $(SKY_H)

# VDC 256-colour weapon sprites: doom1.wad psprite frames -> PNGs (for
# inspection) + generated pattern/palette/frame tables. Needs the full IWAD for
# all weapon graphics; falls back to the baked 1-level WAD (skips missing frames).
WEP_WAD := $(if $(wildcard $(DOOM1WAD)),$(DOOM1WAD),$(IWAD_SRC))
WEP_H   := src/generated/pcfx_weapons.h
$(WEP_C) $(WEP_H): tools/gen_pcfx_weapons.py $(WEP_WAD)
	@mkdir -p src/generated assets/weapons
	python3 tools/gen_pcfx_weapons.py --wad $(WEP_WAD) --out src/generated --png assets/weapons
$(OBJDIR)/platform/pcfx_weapon.o: $(WEP_H)

# VDC 256-colour BG font tiles: doom1.wad STCFN* glyphs -> HuC6270 tile CG (VDC0 high /
# VDC1 low nibble) + widths. Same IWAD fallback as the weapon tables.
FONT_H  := src/generated/pcfx_font.h
$(FONT_C) $(FONT_H): tools/gen_pcfx_font.py $(WEP_WAD)
	@mkdir -p src/generated assets
	python3 tools/gen_pcfx_font.py --wad $(WEP_WAD) --out src/generated --png assets
$(OBJDIR)/platform/pcfx_text.o: $(FONT_H)
$(OBJDIR)/src/generated/pcfx_weapons.o: $(WEP_H)
$(OBJDIR)/src/r_hotpath.o: $(WEP_H)

# --- CD-DA music -------------------------------------------------------------
# Music is Red Book CD audio, not a synthesized score: the KING ADPCM voices are
# spent on SFX and there is no RAM budget for a MUS player. The soundtrack is the
# 3DO Doom one (3DOMusic/, already 44.1kHz/16-bit/stereo), and the track order
# follows optidoom3do's SongLookup table (see platform/i_sound_pcfx.c).
#
# cdlink converts each WAV to a raw 2352-byte-sector audio track ($(TARGET)_tNN.bin,
# referenced as its own FILE by the .cue) and emits src/generated/cdda_tracks.h
# with a CDDA_TRACK_SONGnn define per track. The defines are keyed by WAV stem, so
# cdlink's alphabetical track assignment (Song1, Song10, Song11, ... Song9) never
# has to match the musical order — i_sound_pcfx.c looks tracks up by name.
CDDA_DIR  := 3DOMusic
CDDA_WAVS := $(sort $(wildcard $(CDDA_DIR)/*.wav))
CDDA_H    := src/generated/cdda_tracks.h
$(OBJDIR)/platform/i_sound_pcfx.o: $(if $(wildcard $(CDDA_H)),$(CDDA_H),)

# The host CD linker. Built from vendored source so append/lbaheader/cddaheader
# are always available regardless of what's installed system-wide.
$(CDLINK): tools/pcfxtools/pcfx-cdlink.c tools/pcfxtools/boot.h
	$(MAKE) -C tools/pcfxtools

$(ELF): $(OBJ) platform/pcfx_hot.ld
	$(LD) $(LDFLAGS) $(OBJ) $(LIBS) -o $@ -Map $(MAP)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

out.bin: $(BIN)
	cp $(BIN) out.bin

# Two-pass CD link: the sky's on-disc LBA depends on the program size, but the
# RAINBOW loader needs that LBA compiled in. Pass 1 links the disc and emits
# src/generated/lbas.h; then we recompile the loader (now with the real LBA — same
# code size, so the layout is stable) and re-link the final image.
# Pass 1 also converts the CD-DA WAVs and emits src/generated/cdda_tracks.h, so the
# pass-2 rebuild bakes in real track numbers (pass 1 compiled the fallback 0s). The
# defines are byte constants in a lookup table either way, so this does not move the
# program's size and the LBA layout stays stable. cdlink caches the converted audio
# by mtime, so pass 2 re-links the ~290 MB of tracks without re-decoding them.
cd: out.bin cdlink.txt $(CDLINK) $(SKY_BIN) $(SFX_BIN) $(CDA_BIN) $(CDWAD_BIN) $(MAPPACK_BIN) $(CDDA_WAVS)
	$(CDLINK) cdlink.txt $(TARGET)
	rm -f $(OBJDIR)/platform/i_system_pcfx.o $(OBJDIR)/platform/pcfx_cdasset.o $(OBJDIR)/platform/pcfx_wad.o $(OBJDIR)/platform/pcfx_mappack.o $(OBJDIR)/platform/i_sound_pcfx.o out.bin $(BIN)
	$(MAKE) out.bin
	$(CDLINK) cdlink.txt $(TARGET)
	@echo "built $(TARGET).cue / $(TARGET).bin  (program $$(stat -c%s $(BIN)) bytes, $(words $(CDDA_WAVS)) CD-DA tracks)"

zip: cd
	zip -j DoomPCFX_build.zip $(TARGET).cue $(TARGET).bin $(TARGET)_t*.bin $(TARGET)_SONG*.bin

clean:
	rm -rf $(OBJDIR) out.bin $(BIN) $(TARGET).cue $(TARGET).bin src/generated/lbas.h \
	       $(CDDA_H) $(TARGET)_t*.bin
	$(MAKE) -C tools/pcfxtools clean

-include $(OBJ:.o=.d)
