/* i_system_pcfx.c — NEC PC-FX (V810 + KING/HuC6272) platform backend for GBADoom.
 *
 * Implements the i_system_e32.h porting contract against KING's 8bpp bitmap
 * background and the VCE (Tetsu) palette, using liberis for one-time setup and
 * hand-rolled memory-mapped KRAM stores for the per-frame present.
 *
 * DOOM renders into a 120x160 buffer of 16-bit cells; the renderer pixel-doubles
 * horizontally (color | color<<8), so the buffer is really a 240x160 8bpp image
 * with two identical pixels per 16-bit word. That is exactly the "twice as wide
 * pixels / 16-bit writes" low-detail present the PC-FX wants: each KRAM word we
 * stream paints two pixels. We present it centered in the 256x240 KING BG0 plane.
 *
 * Double-buffered with a hardware page flip: two 256x256 8bpp framebuffers live
 * in KRAM (word 0 and word 32768). We draw into the hidden page, wait vblank,
 * then repoint BG0's CG base at it (king_set_bat_cg_addr) — tear-free, and
 * no CPU copy on the flip itself. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "doomdef.h"
#include "d_event.h"
#include "d_main.h"
#include "i_system_e32.h"
#include "z_zone.h"
#include "pcfx_time.h"

#include <pcfx/king.h>
#include <pcfx/tetsu.h>
#include <pcfx/contrlr.h>
#include <eris/cd.h>
#include <eris/cdda.h>   /* CD-DA music: a data read stops the drive's audio */

#include "pcfx.h"
#include "pcfx_kram.h"
#include "pcfx_present.h"
#include "pcfx_weapon.h"
#include "pcfx_text.h"
#include "pcfx_boot.h"
#include "pcfx_sky.h"       /* generated RAINBOW sky params (PCFX_SKY_BYTES, ...) */

/* The RAINBOW sky stream is embedded on the CD (cdlink `append`) and DMA'd into
 * KRAM at runtime; its start sector comes from the CD-linker-generated lbas.h.
 * Until the first CD-link pass generates it, a placeholder keeps the code (and so
 * the program size, and thus the sector layout) identical across link passes. */
#ifdef HAVE_GENERATED_LBAS
#include "lbas.h"
#endif
#ifndef BINARY_LBA_SRC_GENERATED_PCFX_SKY_BIN
#define BINARY_LBA_SRC_GENERATED_PCFX_SKY_BIN 300u
#endif
#define SKY_LBA (BINARY_LBA_SRC_GENERATED_PCFX_SKY_BIN)

/* -------------------------------------------------------------- framebuffer */
/* There is NO system-RAM backbuffer. The DOOM drawers write pixels straight into
 * the hidden KING KRAM page through the KING data port (see pcfx_kram.h); the
 * only per-frame video work here is the hardware page flip. g_pcfx_fb_base points
 * at the current render (back) page. s_fb_token is a harmless non-NULL value for
 * the engine's now-vestigial screens[0].data / drawvars.byte_topleft, which are
 * assigned but never dereferenced. */
uint32_t g_pcfx_fb_base;
static unsigned short s_fb_token[8];

unsigned short *I_GetBackBuffer(void)  { return s_fb_token; }
unsigned short *I_GetFrontBuffer(void) { return s_fb_token; }
int I_GetVideoWidth_e32(void)  { return SCREENWIDTH; }
int I_GetVideoHeight_e32(void) { return SCREENHEIGHT; }

/* -------------------------------------------------------------- KING layout */
#define KRAM_ROW_WORDS   128                 /* 256 px / 2 px-per-word          */
#define KRAM_PAGE_WORDS  (KRAM_ROW_WORDS * 256) /* 256x256 8bpp page = 32768 w   */
#define KRAM_CG_UNIT     1024u               /* CG base granularity, in words   */
/* Center the 240x160 image inside the 256x240 visible plane. */
#define PRESENT_X_WORDS  4                   /* (256-240)/2 = 8 px = 4 words     */
#define PRESENT_Y        40                  /* (240-160)/2                      */

static int s_back_page = 1;    /* buffer being drawn this frame (0..2)          */
static int s_display_buf = 0;  /* buffer currently latched for display (0..2)   */

/* Deferred gameplay presentation state (machinery further down, contract in
 * platform/pcfx_present.h). */
int g_pcfx_present_pend;       /* nonzero: s_pend_buf awaits its flip           */
int g_pcfx_rainbow_pend;       /* nonzero: the sky needs its per-field re-arm   */
int g_pcfx_palette_pend;       /* nonzero: staged VCE colors await blanking      */
static int s_triple = 0;       /* gameplay triple-buffer (deferred present) mode */
static int s_pend_buf = -1;    /* buffer awaiting its flip                      */

/* Intermission/finale "static two-buffer" mode. Instead of the per-frame page
 * flip, the intermission pre-renders both framebuffers (buffer 0 = no "you are
 * here", buffer 1 = with it) and just selects which to DISPLAY per frame, so the
 * flashing YAH marker costs a page-select register write, not a 60 KB repaint --
 * and needs no RAM staging buffer at all (the old s_bg that fragmented the heap).
 * While s_im_mode is set the presenter does NOT flip; it shows s_im_show. */
static int s_im_mode = 0;
static int s_im_show = 0;

/* Point BG0 (and its sub layer) at the pixel data for framebuffer `page` (0..2).
 * CG base is in 1024-word units: 0/32/128 (buffer 2 lives at word 0x20000, the
 * bit-17 half — its CG base selects microprogram fetch bank B, which
 * king_video_init programs as a mirror of bank A; pcfxemu king.c likewise
 * selects MPROGData[8..] when cg & 0x20000). */
static void king_set_display_page(int page)
{
    uint32_t cg = pcfx_fb_page_base(page) / KRAM_CG_UNIT;
    king_set_bat_cg_addr(KING_BG0, 0, cg);
    king_set_bat_cg_addr(KING_BG0SUB, 0, cg);
    s_display_buf = page;
}

/* ------------------------------------------------- VCE palette staging -----
 * HuC6261 manual C6261 2.1.3 (5), CPW (colour-palette DATA WRITE), explicitly
 * warns that writing palette RAM during display puts noise on screen. Doom
 * used to write all 256 entries directly from damage/pickup/radiation palette
 * changes, so the shared KING/VDC palette could glitch most visibly across the
 * two-VDC combined weapon sprite. pcfxemu stores the writes without modelling
 * that real-hardware noise.
 *
 * Doom changes the whole 256-entry palette at once. Precompute that complete
 * palette into a 512-byte PU_STATIC zone block while display is active, then
 * stream it quickly during blanking. A static shadow crosses this port's
 * link-time stack safety floor; I_PreallocStatics_e32 places the zone block at
 * the heap bottom so it cannot fragment later level allocations. */
enum {
    PCFX_PAL_FADED,
    PCFX_PAL_FULL,
    PCFX_PAL_BOOT
};
static int s_palette_upload_mode;
static uint16_t *s_vce_pal_stage;

static inline __attribute__((always_inline)) void pcfx_palette_stage(int mode)
{
    s_palette_upload_mode = mode;
    g_pcfx_palette_pend = 1;
}

/* Definitions live with the palette converter below. */
static inline __attribute__((always_inline))
uint16_t pcfx_palette_color(int i, int mode);

static void pcfx_palette_stage_scene(int mode)
{
    if (!s_vce_pal_stage)
        s_vce_pal_stage = Z_Malloc(256 * (int)sizeof(uint16_t),
                                   PU_STATIC, NULL);
    for (int i = 0; i < 256; i++)
        s_vce_pal_stage[i] = pcfx_palette_color(i, mode);
    pcfx_palette_stage(mode);
}

/* Push the staged palette. ONLY call inside blanking. CPA (R01) auto-increments
 * on each CPW (R02) write, so select address zero once and stream all 256 data
 * words. Keep the select/data register sequence interrupt-atomic. */
static __attribute__((noinline)) void pcfx_palette_flush(void)
{
    uint32_t psw;
    int mode;
    const uint16_t *stage;

    if (!g_pcfx_palette_pend)
        return;

    mode = s_palette_upload_mode;
    stage = s_vce_pal_stage;
    g_pcfx_palette_pend = 0;
    psw = pcfx_irq_save();
    __asm__ volatile (
        "movea 1,r0,r10\n\t"
        "out.h r10,0x300[r0]\n\t"
        "out.h r0,0x304[r0]\n\t"
        "movea 2,r0,r10\n\t"
        "out.h r10,0x300[r0]"
        : : : "r10", "memory");
    for (int i = 0; i < 256; i++) {
        uint32_t c = mode == PCFX_PAL_BOOT
                   ? pcfx_palette_color(i, mode) : stage[i];
        __asm__ volatile ("out.h %0,0x304[r0]" : : "r"(c) : "memory");
    }
    pcfx_irq_restore(psw);
}

/* Initialization has no presenter poll to drain the staged blackout. */
static void pcfx_palette_flush_in_blank(void)
{
    uint32_t spin = 0;
    unsigned r;
    do {
        r = pcfx_tetsu_raster_stable();
    } while ((r < 240u || r > 258u) && spin++ < 400000u);
    pcfx_palette_flush();
}

/* --- intermission two-buffer control (called from wi_stuff.c / f_finale.c) --- */
/* Enter static two-buffer mode. Draw target = buffer 0 to start. */
void pcfx_im_begin(void)
{
    pcfx_present_flush();   /* no deferred gameplay flip may straddle im mode */
    s_im_mode = 1;
    s_im_show = 0;
    g_pcfx_fb_base = pcfx_fb_page_base(0);
}
/* Leave it; hand back to the normal double-buffer flip on the shown buffer. */
void pcfx_im_end(void)
{
    if (!s_im_mode) return;
    s_im_mode = 0;
    s_back_page = s_im_show ^ 1;      /* next real frame renders the hidden page */
    g_pcfx_fb_base = pcfx_fb_page_base(s_back_page);
}
/* Aim subsequent drawing (V_DrawPatch, the raw-bg DMA) at framebuffer `buf`. */
void pcfx_im_draw_to(int buf)
{
    g_pcfx_fb_base = pcfx_fb_page_base(buf & 1);
}
/* Choose which framebuffer the presenter latches on screen this frame. */
void pcfx_im_show(int buf)
{
    s_im_show = buf & 1;
}

/* KRAM word base of the buffer currently on screen (see pcfx_kram.h). */
uint32_t pcfx_fb_display_base(void)
{
    return pcfx_fb_page_base(s_display_buf);
}

/* ------------------------------------------------------------- RAINBOW sky */
/* The HuC6271 RAINBOW is a full-screen background layer BEHIND the KING plane.
 * We decode the Doom sky into it and show it wherever the KING framebuffer is
 * transparent (palette index 0, Y=0) — i.e. the sky-flat areas the renderer
 * leaves as index 0. It scrolls horizontally with the view angle for free. */

/* The RAINBOW output starts at this scanline; re-arming its transfer must happen
 * once per field, after the visible area (matches the pcfv reference player). */
#define RAINBOW_RESTART_RASTER 248u

/* RAINBOW control ports 0x200..0x214 (direct out.b/out.h).
 *
 * Control (0x204) = 3, NOT 1: bit0 enables the decoder, and bit1 (0x2) selects
 * the HuC6271's "endless scroll" mode. Without bit1 the horizontal scroll runs a
 * 9-bit counter and emits BLACK for every output column whose source index lands
 * past the 256px decoded line (see RAINBOW_Fast_FetchRaster) — so any pan drops
 * the sky to black. Endless mode makes that counter an 8-bit value that wraps at
 * 256, so the 256px sky scrolls seamlessly. Matches waifu's working setup. */
static void rainbow_setup(void)
{
    uint16_t zero = 0, control = 3;
    __asm__ volatile (
        "out.b %[z],0x200[r0]\n"
        "out.b %[z],0x202[r0]\n"
        "movea -128,r0,r10\n"
        "out.h r10,0x208[r0]\n"
        "out.h %[z],0x20c[r0]\n"
        "out.h %[z],0x210[r0]\n"
        "out.h %[z],0x214[r0]\n"
        "out.h %[c],0x204[r0]\n"
        : : [z] "r" (zero), [c] "r" (control) : "r10", "memory");
}

static void rainbow_set_hscroll(int hscroll)
{
    uint16_t lo = (uint16_t)(hscroll & 0xff);
    uint16_t hi = (uint16_t)((hscroll >> 8) & 0x01);
    __asm__ volatile (
        "out.b %[lo],0x200[r0]\n"
        "out.b %[hi],0x202[r0]\n"
        : : [lo] "r" (lo), [hi] "r" (hi) : "memory");
}

/* DMA the compressed sky stream from the CD into KRAM page 1 (once, at init).
 * The HuC6271 RAINBOW decoder only accepts KRAM data delivered by the KING's
 * SCSI/CD DMA engine — the CPU KRAM write port does NOT feed it (that path left
 * the layer blank). So the sky lives on the disc and is read straight into KRAM
 * here, exactly like waifu's rainbow backgrounds. The SCSI DMA must land on the
 * RAINBOW's page, so scsi is routed to page 1 for the duration of the read. */
/* KRAM page-assignment bitmask written to KING register 0x0F (out.w), in the
 * HuC6272-manual NIBBLE layout: D0=SCP(SCSI), D4=BGP(BG), D8=SOP(ADPCM),
 * D12=RAP(RAINBOW). libpcfx's king_set_kram_pages() now packs these fields the
 * same way (it previously used a wrong BYTE layout that routed the RAINBOW onto
 * the wrong page — the reason this raw register form exists); the raw path is
 * kept because it is the boot-critical, hardware-verified sky/ADPCM routing. */
#define KPS_SCSI1    0x0001u
#define KPS_BG1      0x0010u
#define KPS_ADPCM1   0x0100u
#define KPS_RAINBOW1 0x1000u
static inline void king_set_page_setting(uint32_t ps)
{
    uint32_t reg;
    /* Interrupt-atomic: a timer IRQ between the select and the data write
     * corrupts KING on real hardware (see pcfx_irq_save in pcfx.h). */
    uint32_t psw = pcfx_irq_save();
    __asm__ volatile ("movea 15,r0,%[reg]\n"
                      "out.h %[reg],0x600[r0]\n"
                      "out.w %[ps],0x604[r0]\n"
                      : [reg] "=&r" (reg) : [ps] "r" (ps) : "memory");
    pcfx_irq_restore(psw);
}

/* Clear the SCSI phase IRQ the KING raises when a CD DMA completes; a stuck phase
 * blocks later KING work (incl. the RAINBOW transfer). Mirrors waifu. */
static inline void scsi_clear_phase_irq(void)
{
    uint32_t psw = pcfx_irq_save();   /* interrupt-atomic KING sequence */
    __asm__ volatile ("out.h %[two],0x600[r0]\n"
                      "out.h r0,0x604[r0]\n" :: [two] "r" (2) : "memory");
    pcfx_irq_restore(psw);
}

/* eris_cd_read_kram is declared by <eris/cd.h> and called directly. */

/* All three CD->KRAM loads below (sky, ADPCM bank, raw framebuffer pictures)
 * prefer real KING DMA (eris_cd_read_kram, libpcfx/src/cd.c), armed COUNT-0
 * (phase-driven) rather than with the transfer's exact byte count -- immune to
 * both modeled real-hardware CD errata by construction, real hardware-DMA speed
 * instead of a CPU byte pump, and the same shape real commercial PC-FX titles
 * built with the official PC-FXGA/GMAKER SDK use for their own bulk CD loads
 * (see pcfxemu's docs/cd-loading-survey.md; verified against 4 real disc images
 * with pcfxemu's CD_XFER_DEBUG profiler: zero CPU-PIO reads across all four).
 * BUT: on the one real console this port is tested on, the DMA paths currently
 * fail outright (boot dies at the first read), so the CPU-PIO path is back as
 * an automatic sticky fallback -- see cd_kram_read() below. */

/* CD -> KRAM with automatic KING-DMA <-> CPU-PIO fallback.
 *
 * The mode is a PREFERENCE, never a verdict. This used to be a sticky one-shot
 * probe — first load tries DMA with a 4-attempt budget, and whichever path lost
 * is never tried again — which is the same antipattern pcfx_wad.c already had
 * removed: a single unlucky read (a marginal CD-R sector, a drive still
 * spinning up) permanently disables a working path for the whole session, and
 * the shrunken probe budget makes that unlucky read far more likely. Here the
 * preference only decides which path is tried FIRST; both stay available on
 * every call, at the full attempt budget, and a success on the other path just
 * reorders the next call.
 *
 * `page1` selects the CPU write-cursor page for the PIO fallback and the D31
 * verify page for DMA; the DMA engine itself takes its page from REG.0F, which
 * the caller has already routed. NOT used for the RAINBOW sky: its decoder only
 * accepts KRAM data delivered by the SCSI-DMA engine. */
#define CD_KRAM_DMA 1
#define CD_KRAM_PIO 2
#ifdef PCFX_CD_FORCE_PIO
static int s_cd_kram_pref = CD_KRAM_PIO;  /* debug knob: exercise PIO first on emu */
#else
static int s_cd_kram_pref = CD_KRAM_DMA;
#endif

static void rainbow_clear_kram(void);    /* defined below */
static void tetsu_apply_layer_mix(int);  /* defined below */

static int cd_kram_read(unsigned lba, unsigned kram_word, unsigned bytes, int page1)
{
    unsigned addr = kram_word | (page1 ? 0x80000000u : 0u);
    int ok;

    if (s_cd_kram_pref == CD_KRAM_DMA) {
        if (eris_cd_read_kram(lba, addr, bytes))
            return 1;
        ok = eris_cd_read_kram_pio(lba, addr, 1, bytes);
        if (ok)
            s_cd_kram_pref = CD_KRAM_PIO;   /* try the winner first next time */
        return ok;
    }

    if (eris_cd_read_kram_pio(lba, addr, 1, bytes))
        return 1;
    ok = eris_cd_read_kram(lba, addr, bytes);
    if (ok)
        s_cd_kram_pref = CD_KRAM_DMA;
    return ok;
}

static void rainbow_load_cd(void)
{
    unsigned bytes = (PCFX_SKY_BYTES + 2047u) & ~2047u;   /* whole CD sectors */
    king_set_page_setting(KPS_SCSI1 | KPS_RAINBOW1 | KPS_ADPCM1);  /* DMA -> page 1 */
    pcfx_boot_progress_set_io("SKY", SKY_LBA, bytes / 2048u);
    if (!eris_cd_read_kram(SKY_LBA, KRAM_RAINBOW_WORD | 0x80000000u, bytes)) {
        /* The RAINBOW decoder only accepts KRAM data delivered by the KING
         * SCSI-DMA engine, so there is no CPU-PIO fallback for the sky. Boot
         * on with a black (all-zero, chroma-keyed transparent) sky instead of
         * dying: a real-hardware burn should still reach gameplay and report
         * the verdict on the diag panel. */
        rainbow_clear_kram();
        tetsu_apply_layer_mix(0);   /* unfed RAINBOW = garbage on hw: hide it */
        pcfx_boot_progress_set_note("SKY READ FAIL");
    }
    scsi_clear_phase_irq();
    eris_cdda_notify_cd_read();
    king_set_page_setting(KPS_RAINBOW1 | KPS_ADPCM1);   /* bg/scsi page 0, rb/adpcm 1 */
}

/* Generic CD -> KRAM (page 1) DMA via the KING SCSI engine. Used to load the
 * ADPCM SFX bank straight into KRAM at boot (platform/i_sound_pcfx.c) — no RAM
 * staging copy and no per-word CPU loop, exactly like the sky above. `bytes` is
 * rounded up to whole CD sectors. The byte-for-byte KRAM image is identical to the
 * old write_kram little-endian packing (LE CPU: source byte N lands at KRAM byte N),
 * so the bank plays the same. */
int pcfx_king_dma_cd_to_kram(unsigned lba, unsigned kram_word, unsigned bytes)
{
    unsigned rd = (bytes + 2047u) & ~2047u;
    int ok;
    king_set_page_setting(KPS_SCSI1 | KPS_RAINBOW1 | KPS_ADPCM1);
    pcfx_boot_progress_set_io("KRAM", lba, rd / 2048u);
    ok = cd_kram_read(lba, kram_word, rd, 1);
    scsi_clear_phase_irq();
    eris_cdda_notify_cd_read();
    king_set_page_setting(KPS_RAINBOW1 | KPS_ADPCM1);
    return ok;
}

/* CD -> KRAM page 0 (the FRAMEBUFFER page) DMA. Same SCSI engine as above, but the
 * SCSI target page is 0 (KPS_SCSI1 CLEARED) so a raw framebuffer-word picture streams
 * straight into a framebuffer buffer with no LZ4 decode and no per-word CPU blit — the
 * "CD => KRAM DMA" title path (platform/pcfx_cdasset.c). `kram_word` is a within-page-0
 * word offset (0 or KFB_PAGE_WORDS for the two buffers); `bytes` rounds up to sectors.
 * DMA only into the HIDDEN (back) buffer so the transfer never shows mid-scanout. */
void pcfx_king_dma_cd_to_fb(unsigned lba, unsigned kram_word, unsigned bytes)
{
    unsigned rd = (bytes + 2047u) & ~2047u;
    king_set_page_setting(KPS_RAINBOW1 | KPS_ADPCM1);   /* SCSI -> page 0 */
    pcfx_boot_progress_set_io("FRAMEBUFFER", lba, rd / 2048u);
    if (!cd_kram_read(lba, kram_word, rd, 0)) {
        int dpath, dphase, dstatus, dtries;
        eris_cd_get_diag(&dpath, &dphase, &dstatus, &dtries);
        I_Error("CD framebuffer read failed: LBA %u, %u sectors"
                " (path %d phase %d st %d try %d)",
                lba, rd / 2048u, dpath, dphase, dstatus, dtries);
    }
    scsi_clear_phase_irq();
    eris_cdda_notify_cd_read();
    king_set_page_setting(KPS_RAINBOW1 | KPS_ADPCM1);
}

/* Zero the RAINBOW sky's KRAM region (page 1, the words the decoder reads). The
 * HuC6271 then decodes all-zero DCT blocks -> Y=U=V=0 pixels, which the emulator's
 * (and hardware's) chroma key turns transparent, so the layer contributes nothing
 * and the black backdrop shows. This is how the sky is "blacked out" off the field.
 * CPU KRAM writes address king->KRAM[page] directly (via the cursor's page bit),
 * independent of the page-setting register, so this is safe to call any time. */
static void rainbow_clear_kram(void)
{
    unsigned words = ((PCFX_SKY_BYTES + 2047u) & ~2047u) / 2u;  /* match the load */
    /* Interrupt-atomic (boot-time only; the timer misses a few ticks). */
    uint32_t psw = pcfx_irq_save();
    king_kram_set_cursor_page(KRAM_RAINBOW_WORD, 1, 1);
    for (unsigned i = 0; i < words; i++) write_kram(0);
    pcfx_irq_restore(psw);
}

/* Whether the RAINBOW sky is shown this frame. The sky is only meaningful during
 * actual gameplay (GS_LEVEL); on the title, menus, intermission and finale it must
 * be gone so it can't bleed through the framebuffer's index-0 (transparent) pixels.
 * We keep the per-field re-arm running unconditionally (see the presenter) and just
 * swap what sits in the sky's KRAM: the decoded sky, or zeros (= transparent). */
static int s_rainbow_active = 0;   /* is the sky composited right now?           */
static int s_rainbow_loaded = 0;   /* is the sky stream currently in KRAM?       */

/* Tetsu layer mix and the sky's on/off switch.
 *
 * Layer order (high wins): VDC BG font/HUD tiles(7) > VDC weapon sprites(6) >
 * KING scene BG0(5) > unused KING BG1/2/3(4/3/2) > RAINBOW sky(1).
 *
 * TWO corrections here over the scheme the 07-24/07-25 burns ran, both taken
 * straight from the HuC6261 manual (DEVICE_EN/C6261):
 *
 * 1. Priority 0 is NOT "hidden". C6261 2.3.3 / R08-R09: "Assign each plane a
 *    unique 3-bit priority from 0 through 7. Larger values have higher
 *    priority." Zero is simply the BOTTOM of the stack, and 2.4.5 spells out
 *    what that means: the lowest-priority plane is what shows wherever every
 *    plane above it is transparent. BG0's cleared framebuffer is palette index
 *    0 = transparent, so parking the RAINBOW at priority 0 did the exact
 *    OPPOSITE of hiding it — it put the sky decoder underneath a see-through
 *    screen. That is the coloured noise band photographed on the boot loading
 *    screen and the level-load screen while the title screen (opaque BG0
 *    everywhere) stayed clean. The band is direct YUV straight out of the
 *    HuC6271: at that point in boot 254 of the 256 VCE palette entries are
 *    black, so NO palette-plane artefact can be coloured — only the RAINBOW
 *    plane bypasses the palette.
 *
 *    Nor is the noise avoidable by feeding the decoder zeros: libpcfx's
 *    tetsu_init() writes R0A-R0C = 0x00FF, i.e. min > max on all three
 *    components, which per C6261 2.4.4 DISABLES the HuC6271 chroma key
 *    outright. Every pixel the sky decoder emits is opaque, always.
 *
 * 2. The documented per-plane hide is the CR (R00) plane-enable bit for BG71
 *    ("Plane-enable bits independently show or hide: ... HuC6271 BG71 ... take
 *    effect in the next horizontal display period"). That is what actually
 *    removes the layer, so the sky's on/off now drives that bit, and the
 *    priorities stay fixed and UNIQUE (the old call assigned 0 to BG1, BG2,
 *    BG3 and the RAINBOW simultaneously, which R08-R09 explicitly forbids).
 *
 * Belt and braces, since a stopped decoder costs nothing: rainbow_stop() also
 * parks the HuC6271 itself whenever the sky is off, so it is not left holding
 * a half-decoded field behind the disabled plane. */
static void tetsu_apply_layer_mix(int rainbow_on)
{
    tetsu_set_priorities(7, 6, 5, 4, 3, 2, 1);
    tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_256,
                              1, 1, 1, 0, 0, 0, rainbow_on ? 1 : 0);
}

static void rainbow_rearm(void);
static void rainbow_stop(void);
static int s_rb_armed_field;

void I_SetRainbowActive_e32(int on)
{
    on = on ? 1 : 0;
    if (on == s_rainbow_active) return;
    s_rainbow_active = on;
    /* Opens/closes the poll gate that keeps the per-field re-arm running. */
    g_pcfx_rainbow_pend = on;
#ifdef DEV_RB_NO_REARM
    g_pcfx_rainbow_pend = 0;
#endif
    if (on) {
        if (!s_rainbow_loaded) { rainbow_load_cd(); s_rainbow_loaded = 1; }
        /* Decode a field BEFORE the plane is enabled. An idle HuC6271 still
         * presents whatever it last produced, so enabling the plane first
         * would flash that stale content for up to one field. */
        rainbow_rearm();
        s_rb_armed_field = 1;
        tetsu_apply_layer_mix(1);
        /* Enter the deferred triple-buffer present for gameplay. Buffer 2
         * joins the flip rotation on the next present; it powers up as KRAM
         * garbage and rows the view never repaints (208..239, behind the HUD
         * tiles) would otherwise carry it forever, so clear it once here. */
        if (!s_triple) {
            pcfx_kram_fill_guarded(pcfx_fb_page_base(2), 0, KFB_PAGE_WORDS);
            s_triple = 1;
        }
    } else {
        /* Hide the plane FIRST, then tear the decoder down: everything that
         * follows (the flush, the KRAM clear) runs with the sky already out of
         * the mix, so none of it can be seen. */
        tetsu_apply_layer_mix(0);
        rainbow_stop();
        s_rb_armed_field = 0;
        /* Leave gameplay: finish any outstanding flip, then hand back to the
         * classic synchronous 2-buffer presenter on buffers {0,1}. The display
         * may legitimately sit on buffer 2 for one more frame (the old scene,
         * exactly what the classic path also leaves on screen); the next
         * classic present flips away from it. */
        pcfx_present_flush();
        s_triple = 0;
        if (s_back_page == 2) {
            s_back_page = (s_display_buf == 0) ? 1 : 0;
            g_pcfx_fb_base = pcfx_fb_page_base(s_back_page);
        }
        rainbow_clear_kram();
        s_rainbow_loaded = 0;
    }
}

/* Arm the RAINBOW decode for ONE field (KING regs 0x40..0x44). The HuC6271
 * decodes exactly one frame per arm, so this must be called every field (during
 * vblank) or the layer goes blank — the once-at-init arm the port used before is
 * why the sky never appeared. Mirrors the pcfv reference's start_rainbow_frame. */
#ifndef RB_SRC_ADDR
#define RB_SRC_ADDR  (KRAM_RAINBOW_WORD)
#endif
/* Interrupt-atomic, and not optionally so: this is SIX KING 0x600/0x604
 * register-select/data pairs and the single most-executed KING run in the
 * program (once every field, forever). By the silicon-verified rule in pcfx.h,
 * a firing ~1 ms interval-timer IRQ inside such a run corrupts the access -- so
 * leaving it unguarded put a ~1 ms window over six writes, 60 times a second.
 * The registers it can land in are exactly the ones that decide how much sky
 * appears: 0x43 is a 5-bit BLOCK COUNT (a corrupted value decodes fewer than
 * the 15 strips the image needs, so the picture stops partway down), 0x42 the
 * start raster, 0x41 the source address. None of this reproduces on pcfxemu,
 * which performs KING accesses atomically. */
static void rainbow_rearm(void)
{
    uint32_t psw = pcfx_irq_save();
    king_reg16(0x40, 0x0000);
    king_reg32(0x41, (uint32_t)RB_SRC_ADDR);
    king_reg16(0x42, (uint16_t)PCFX_SKY_TRANSFER_START);
    king_reg16(0x43, (uint16_t)PCFX_SKY_BLOCK_COUNT);
    king_reg16(0x44, 0x0000);
    king_reg16(0x40, 0x0001);
    pcfx_irq_restore(psw);
}

/* Park the HuC6271: clear the transfer-enable bit so no further field is
 * decoded. Paired with the CR plane-enable in tetsu_apply_layer_mix() — the
 * plane bit is what makes the sky invisible, this is what stops the decoder
 * from sitting on a stale field behind it. Interrupt-atomic for the usual
 * reason (a timer IRQ inside a KING 0x600/0x604 pair corrupts the access). */
static void rainbow_stop(void)
{
    uint32_t psw = pcfx_irq_save();
    king_reg16(0x40, 0x0000);
    pcfx_irq_restore(psw);
}

/* One arm per field: none leaves the layer blank for that field, two costs KRAM
 * bandwidth in vblank for nothing. Set when an arm goes out, cleared when the
 * raster is next seen inside the visible area (i.e. a new field has begun). */
static int s_rb_armed_field = 0;

/* The window in which an arm is both safe and effective: from the raster at
 * which this field's transfer has finished, to the end of the field. */
#define RAINBOW_REARM_LAST_RASTER 261u

/* Re-arm for the coming field if the raster is in the window and this field has
 * not been armed yet. NEVER spins -- it is called from the render-phase poll
 * sites, where blocking would cost frame time; a field whose window is missed
 * simply gets its arm from the presenter's own spin instead. */
static void rainbow_poll_rearm(unsigned raster)
{
    if (raster < RAINBOW_RESTART_RASTER) {
        s_rb_armed_field = 0;         /* visible area: a new field has begun */
        return;
    }
    if (s_rb_armed_field || raster > RAINBOW_REARM_LAST_RASTER)
        return;
    rainbow_rearm();
    s_rb_armed_field = 1;
}

/* ------------------------------------ deferred gameplay presentation -------
 * (call contract in platform/pcfx_present.h)
 *
 * While s_triple is set (gameplay), I_FinishUpdate_e32 does not wait for the
 * raster at all: it marks the just-rendered buffer PENDING, re-aims the
 * drawers at the third framebuffer, and returns. pcfx_present_poll() — called
 * from the tic wait loop and between rendered subsectors/visplanes/sprites —
 * performs the presentation the moment the raster is inside the safe window:
 *
 *   lines 208..239: the flip is invisible (BG0 rows 208..239 sit behind the
 *                   opaque VDC HUD tiles; everything >= 240 is vblank);
 *   line  240:      the VDCs' repeated SATB DMA latched at the VDW transition,
 *                   so a SAT published after it can never be half-latched;
 *   line  248:      the RAINBOW finished this field's transfer; KING regs
 *                   0x40..44 may be re-armed for the next field.
 *
 * The poll flips (>= 208), spins the short remaining distance to 248
 * (<= 40 lines ~ 2.6 ms worst, typically well under 1 ms), re-arms the
 * RAINBOW, then runs the VDC weapon/text uploads — the classic presenter's
 * exact order and raster placement, minus the up-to-a-full-field wait for
 * vblank to come around, which is what turned a small render overrun into a
 * whole extra field on the 1%-low frames (see MICRO-OPT-NEXT-STEPS.md "two
 * field locks": both locks are removed here, keeping the hardware sky). A
 * frame that misses every window is caught by pcfx_present_flush() at the
 * next I_FinishUpdate, degrading to exactly the old synchronous cost. */
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
extern uint32_t g_bl_vsync, g_bl_upload, g_bl_rearm;
#endif

static void present_spin_to(unsigned raster)
{
    uint32_t spin = 0;
    while (pcfx_tetsu_raster_stable() < raster && spin++ < 200000u) { }
}

void pcfx_present_poll(void)
{
    unsigned r = pcfx_tetsu_raster_stable();

    /* C6261 2.1.3 (5): palette DATA writes belong in vertical blanking. The
     * precomputed 256-entry burst takes only a few scanlines against this
     * 19-line window. */
    if (g_pcfx_palette_pend && r >= 240u && r <= 258u)
        pcfx_palette_flush();

    /* Field-rate work first, and unconditionally: the sky must be re-armed on
     * every field, not on every presented frame (see g_pcfx_rainbow_pend in
     * pcfx_present.h). This is why the poll no longer returns early on
     * !g_pcfx_present_pend -- that gate is what limited the sky to the fields
     * a flip happened to land in. */
    if (g_pcfx_rainbow_pend)
        rainbow_poll_rearm(r);

    if (!g_pcfx_present_pend)
        return;

    /* Act only inside [208..258]: early enough that the flip and the SAT
     * publish still land in this vblank, late enough to be tear-safe. */
    if (r < 208u || r > 258u)
        return;

    king_set_display_page(s_pend_buf);
    s_pend_buf = -1;
    g_pcfx_present_pend = 0;

    {
        /* Only spin for the arm if this field has not already had one from the
         * field-rate path above -- which, now that it runs, is the common case. */
        int want_rearm = s_rainbow_active && !s_rb_armed_field;
#ifdef DEV_RB_NO_REARM
        want_rearm = 0;
#endif
        if (want_rearm) {
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
            uint64_t _tr = itu_ticks();
#endif
            present_spin_to(RAINBOW_RESTART_RASTER);
            rainbow_rearm();
            s_rb_armed_field = 1;
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
            g_bl_rearm += (uint32_t)(itu_ticks() - _tr);
#endif
        } else {
            /* No sky to re-arm: still publish the SAT after this field's latch. */
            present_spin_to(240u);
        }
    }

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    {
        uint64_t _t = itu_ticks();
#endif
        pcfx_weapon_present();
        pcfx_text_flush();
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        g_bl_upload += (uint32_t)(itu_ticks() - _t);
    }
#endif
}

void pcfx_present_flush(void)
{
    uint32_t spin = 0;
    while (g_pcfx_present_pend && spin++ < 4000000u)
        pcfx_present_poll();
}

static void pcfx_rainbow_init(void)
{
    /* Route RAINBOW + ADPCM to KRAM page 1 (framebuffer stays on page 0). */
    king_set_page_setting(KPS_RAINBOW1 | KPS_ADPCM1);
    rainbow_setup();
#ifdef DEV_RAINBOW_TEST
    rainbow_load_cd();     /* DMA the sky from the disc into KRAM page 1 */
    s_rainbow_loaded = 1; s_rainbow_active = 1; g_pcfx_rainbow_pend = 1;
#else
    /* Boot lands on the title screen, where the sky must NOT show. Zero the sky
     * KRAM (which powers up as 0xFF garbage) so the decoder yields a transparent
     * layer; the sky is DMA'd in on the first transition into GS_LEVEL via
     * I_SetRainbowActive_e32(true), and cleared again when leaving gameplay. */
    rainbow_clear_kram();
#endif
    /* Arm the first field ONLY if the sky is meant to be on (the presenter
     * re-arms every field after that). Boot is not: arming an inactive decoder
     * had it chewing through the zeroed stream and emitting an opaque band of
     * direct YUV that the plane-enable bit below now keeps out of the mix
     * anyway — but there is no reason to run it at all. */
    if (s_rainbow_active)
        rainbow_rearm();
    else
        rainbow_stop();

#ifdef DEV_RAINBOW_TEST
    /* Diagnostic: demo-exact full-screen RAINBOW (rainbow priority 7, BG fully
     * OFF, microprogram off) to isolate whether the transfer+data decode. */
    king_disable_microprogram();
    king_set_bg_prio(KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE,
                          KING_BGPRIO_HIDE, 0);
    king_set_bg_mode(KING_BGMODE_NONE, 0, 0, 0);
    tetsu_set_priorities(0, 0, 0, 0, 0, 0, 7);
    tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_16,
                              0, 0, 0, 0, 0, 0, 1);
#else
    /* Layer order (high wins): VDC BG font+HUD tiles(7) > VDC weapon sprites(6)
     * > KING scene BG0(5) > unused BG1/2/3 > RAINBOW sky(1, and only ENABLED in
     * CR while gameplay wants it). BG must outrank sprites so the opaque HUD bar
     * tiles cover the weapon in the bottom 32 lines (the weapon's hardware
     * vdispwid clip was removed to let the VDC render the HUD region — see
     * pcfx_weapon.c). bg_depth = spr_depth = 256 puts picture_mode in
     * both-combine mode (0xC0): both VDCs combine into 8bpp BG font tiles
     * (platform/pcfx_text.c) AND 8bpp weapon sprites (platform/pcfx_weapon.c);
     * bg7up_show + spr7up_show both on. tetsu_apply_layer_mix() writes the whole
     * CR word including those bits, so it is the single owner of this register
     * and no second tetsu_set_video_mode() call is needed (the one that used to
     * follow it here hard-coded the RAINBOW plane ENABLED, which is what put the
     * sky decoder on screen underneath every transparent loading page). */
    tetsu_apply_layer_mix(s_rainbow_active);
#endif
    rainbow_set_hscroll(0);   /* fixed RAINBOW image: no horizontal scroll */
}

static void king_video_init(void)
{
    /* KRAM MODE (REG.61) — MUST be the first KING access of the whole program.
     *
     * C6272_1 2.1: "After reset, program the KRAM mode before performing any
     * KRAM access", and REG.61 "Set this register exactly once after reset and
     * before any KRAM access". Nothing in this port or in libpcfx ever wrote
     * it, so the console ran on whatever the retail BIOS left — and the
     * 2026-07-24 burns show that was 1-MBIT mode. The two Hudson KRAM map
     * figures (libpcfx docs/KING_REGS.md, 61476210 and c260c7b9) explain every
     * symptom exactly:
     *
     *   1-Mbit map (no pages): per bank, words 0x00000..0x0FFFF SOLID and
     *   0x10000..0x1FFFF DOTTED (optional/expansion). Page brackets absent —
     *   1.3: "In 1-Mbit mode only page 0 is available / all page selectors
     *   must be zero."
     *   4-Mbit map (paged): per bank, page 0 = real 0x00000..0x1FFFF and
     *   page 1 = real 0x20000..0x3FFFF, every 64K block solid.
     *
     * So in 1-Mbit mode the A16=1 half is the DOTTED expansion half (dead:
     * the 0x5555 open-bus reads at scratch 0x1F800, the pale-field framebuffer,
     * the intermission cache that never held its picture), bit 17 is the A/B
     * BANK bit so 0x20000.. is bank B's live first 64K (why maka/tank3d and
     * Team Innocent's 0x20000 always worked), and every page selector we
     * program is required to be zero — which is why king_set_kram_pages(0,0,1,1)
     * never actually moved RAINBOW/ADPCM anywhere. The sky stream was landing
     * at bank-A word 0, i.e. straight into visible framebuffer 0: the
     * multicolour noise band across boot rows ~35..55 (its 7 sectors = 7168
     * words = exactly 56 framebuffer rows), and the glitched SFX bank.
     *
     * 4-Mbit mode fixes all of it at once: the dotted half becomes real, page 1
     * becomes legal and distinct, and KRAM is 2 pages x 262144 words = 1 MB.
     *
     * It costs nothing to do so. libpcfx example 024 sweeps REG.61 against a
     * fixed BG schedule on this console and times 30720 CPU->KRAM halfwords:
     * $61 = 0/1/2/3 -> 26038/26035/26033/26032 ticks (18174/18172/18171/18170
     * us), a 6-tick spread of 0.023% with no bit-0 structure — noise. The same
     * bench separates the microprogram variants by 10% (NOPx16 24965 vs ALL16
     * 27600) and the 8bpp fetch rates by 14%, so it plainly resolves real
     * arbitration effects; REG.61 simply is not a speed knob. It is a capacity
     * knob, and capacity is what buys speed here: the resident BG cache and the
     * one-shot SFX bank only exist because the second 64K block is populated,
     * and each one they replace is a ~61 KB CD read costing tens of ms.
     *
     * Interrupt-atomic like every other KING select/data pair (pcfx.h). */
    {
        uint32_t psw = pcfx_irq_save();
        king_set_kram_mode(1);          /* 1 = 4-Mbit: 2 pages x 262144 words */
        pcfx_irq_restore(psw);
    }

    king_init();        /* clears all 4 half-pages — only reaches them in 4-Mbit */
    tetsu_init();

    /* BG0 farthest-back and shown; BG1..3 hidden. */
    king_set_bg_prio(KING_BGPRIO_0, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE,
                          KING_BGPRIO_HIDE, 0);
    king_set_bg_mode(KING_BGMODE_256_PAL, 0, 0, 0); /* 8bpp BG0 */
    king_set_bg_size(KING_BG0, KING_BGSIZE_256, KING_BGSIZE_256,
                          KING_BGSIZE_256, KING_BGSIZE_256);
    king_set_scroll(KING_BG0, 0, 0);
    king_set_scroll(KING_BG0SUB, 0, 0);

    /* IDENTITY affine coefficients for BG0 — REQUIRED on real hardware.
     *
     * Silicon-verified (maka/tank3d bring-up, PCFXEMU_ACCURACY_FIXES.md #1,
     * cross-checked against a Miraculum register dump): KING BG0's display
     * fetch ALWAYS passes through the affine unit and applies the A/B/C/D
     * coefficient registers, whether or not the priority-word rotate bit
     * (0x1000) is set. pcfxemu only applies them in rotate mode, so a build
     * that never writes them renders clean in emulation but horizontally
     * scrambled on silicon (power-up garbage coefficients scale every
     * scanline — the deterministic boot-text garble that survived both the
     * vblank gating and the IRQ-atomicity fixes; solid bars hid it because
     * resampling a uniform run looks uniform). Miraculum keeps A=D=0x0100
     * loaded even on non-rotating frames. Regs: 0x38=A, 0x39=B, 0x3A=C,
     * 0x3B=D (8.8 fixed-point), 0x3C/0x3D = center X0/Y0. */
    king_reg16(0x38, 0x0100);   /* A = 1.0 */
    king_reg16(0x39, 0x0000);   /* B = 0   */
    king_reg16(0x3A, 0x0000);   /* C = 0   */
    king_reg16(0x3B, 0x0100);   /* D = 1.0 */
    king_reg16(0x3C, 0x0000);   /* rotation center X0 = 0 */
    king_reg16(0x3D, 0x0000);   /* rotation center Y0 = 0 */

    /* SCSI/BG/RAINBOW -> page 0, ADPCM -> page 1. Argument order is
     * (scsi, bg, rainbow, adpcm), so this ALSO puts the RAINBOW on page 0 --
     * transient and harmless, because pcfx_rainbow_init() re-routes it to
     * page 1 via king_set_page_setting() before anything decodes a sky. */
    king_set_kram_pages(0, 0, 0, 1);

    /* Microprogram: fetch BG0 character generator (4 words for 8bpp linear).
     *
     * Unused slots MUST be KING_CODE_NOP — the value 0 is NOT a NOP, it is
     * KING_CODE_BG0_CG_0, a live fetch opcode (the NOP bit is bit 0). The old
     * zero-fill therefore scheduled twelve redundant BG0 CG+0 fetches per
     * cycle group on real hardware, saturating the K-BUS against CPU access
     * (pcfxemu draws from the slot decode and masked this). cdtest — clean on
     * silicon — fills unused slots with KING_CODE_NOP explicitly.
     *
     * Slots 0-7 are fetch bank A (CG base bit 17 clear), slots 8-15 bank B
     * (bit 17 set): mirror the program into bank B so any future buffer above
     * word 0x20000 fetches correctly (maka/tank3d bring-up fix #1: only bank
     * A was programmed and their buffer-2 frames were blank on hardware). */
    static uint16_t microprog[16];
    for (int i = 0; i < 16; i++) microprog[i] = KING_CODE_NOP;
    microprog[0]  = KING_CODE_BG0_CG_0;
    microprog[1]  = KING_CODE_BG0_CG_1;
    microprog[2]  = KING_CODE_BG0_CG_2;
    microprog[3]  = KING_CODE_BG0_CG_3;
    microprog[8]  = KING_CODE_BG0_CG_0;   /* bank B mirror */
    microprog[9]  = KING_CODE_BG0_CG_1;
    microprog[10] = KING_CODE_BG0_CG_2;
    microprog[11] = KING_CODE_BG0_CG_3;
    king_disable_microprogram();
    king_write_microprogram(microprog, 0, 16);
    king_enable_microprogram();

    /* KING BG palette bank 0. */
    tetsu_set_king_palette(0, 0, 0, 0);
    tetsu_set_rainbow_palette(0);

    /* 262 lines, 5 MHz dot clock (256px wide), 256-color BG0 shown. */
    tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_16,
                              1, 0, 1, 0, 0, 0, 0);

    /* Clear all three framebuffers AND the BG-cache slot to index 0. Buffers
     * 0/1 are in KRAM bank A and buffers 2/3 at 0x20000/0x28000 are in bank B,
     * and C6272_1 1.3 forbids a single transfer crossing a bank boundary (the
     * write cursor auto-increments within the 17-bit address field, holding the
     * D17 bank bit), so this is necessarily two runs — one per bank. */
    pcfx_kram_fill_guarded(pcfx_fb_page_base(0), 0, KRAM_PAGE_WORDS * 2);
    pcfx_kram_fill_guarded(pcfx_fb_page_base(2), 0, KRAM_PAGE_WORDS * 2);

    /* Neutral-black VCE palette until the first real palette upload, so an
     * undefined power-up palette can't flash on real hardware. Seed only the
     * two existing PLAYPAL grays used by the boot progress bar; the bar itself
     * never changes palette registers. */
    pcfx_palette_stage(PCFX_PAL_BOOT);
    pcfx_palette_flush_in_blank();

    king_set_display_page(0);
    s_back_page = 1;
    g_pcfx_fb_base = pcfx_fb_page_base(s_back_page);
}

/* Fill the current render (back) page with palette index 0. */
static void king_clear_back_page(void)
{
    pcfx_kram_fill_guarded(pcfx_fb_page_base(s_back_page), 0, KFB_PAGE_WORDS);
}

/* -------------------------------------------------------------- palette ---- */
/* DOOM palette = 256 RGB888 triplets. PC-FX color = Y8U4V4 (Y<<8|U4<<4|V4),
 * neutral black = 0x0088, Y==0 is transparent.
 *
 * The PC-FX (and the emulator) expand the 4-bit U/V nibbles to the high 4 bits
 * of a byte, then apply a fixed YUV->RGB matrix (see king.c RebuildUVLUT):
 *     R = Y            + 1.139828*v
 *     G = Y - 0.394610*u - 0.580500*v
 *     B = Y + 2.031999*u              (u = (U4<<4)-128, v = (V4<<4)-128)
 * A naive linear RGB->YUV badly UNDERSATURATES — chiefly the V (red) axis,
 * whose reconstruction gain (~18/step) is far steeper than U's (~32/step) — so
 * DOOM's red/brown-heavy art came out muddy. Instead, for each colour, search
 * the representable (U4,V4) chroma pairs and pick the one whose least-squares
 * luma reconstructs closest to the target under a green-weighted error metric
 * (2:4:1) — the closest PC-FX colour. Mirrors waifu / Cascade FX's converter.
 * The YUV->RGB matrix is near-diagonal, so we seed from its analytic inverse
 * and refine within a small window instead of scanning all 256 pairs.
 *
 * HUE PRESERVATION: the 4-bit chroma is coarse, and at LOW luma a full chroma
 * step overshoots, so the least-squares winner for a *dark* coloured texel is
 * often neutral grey (U4=V4=8) — it has less RGB error than the nearest saturated
 * step. That desaturates the darkest browns (e.g. Doom idx 1 = (31,23,11) and
 * idx 2) to grey while their brighter siblings (idx 239, ...) stay brown, so a
 * brown floor/stair speckles grey where its dark texels land. When the source has
 * real chroma (saturation >= 8) we therefore forbid the neutral-grey pick, forcing
 * a same-hue (slightly oversaturated) choice so the whole ramp reads one hue. Pure
 * greys (saturation 0) are unaffected. */
static uint16_t rgb_to_yuv(int r, int g, int b)
{
    const int y0 = (77 * r + 150 * g + 29 * b) >> 8;   /* luma seed */
    int u4c = 8 + (b - y0) / 32;   /* B gain ~32.5/step -> analytic U4 */
    int v4c = 8 + (r - y0) / 18;   /* R gain ~18.2/step -> analytic V4 */
    int u4lo = u4c - 3, u4hi = u4c + 3, v4lo = v4c - 3, v4hi = v4c + 3;
    if (u4lo < 0)  u4lo = 0;   if (u4hi > 15) u4hi = 15;
    if (v4lo < 0)  v4lo = 0;   if (v4hi > 15) v4hi = 15;

    const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const int forbid_grey = (mx - mn) >= 8;   /* coloured source -> never desaturate */

    int best_err = 0x7fffffff, best_y = 1, best_u4 = 8, best_v4 = 8;
    for (int u4 = u4lo; u4 <= u4hi; ++u4) {
        const int u = (u4 << 4) - 128;
        for (int v4 = v4lo; v4 <= v4hi; ++v4) {
            if (forbid_grey && u4 == 8 && v4 == 8)
                continue;                     /* keep the hue, don't fall to grey */
            const int v = (v4 << 4) - 128;
            /* Q10 fixed-point of the emulator's YUV->RGB offsets. */
            const int ro = (            1167 * v) / 1024;
            const int go = (-404 * u -   594 * v) / 1024;
            const int bo = ( 2081 * u -    1 * v) / 1024;
            int y = (2 * (r - ro) + 4 * (g - go) + (b - bo) + 3) / 7;
            if (y < 0) y = 0; else if (y > 255) y = 255;
            int rr = y + ro, gg = y + go, bb = y + bo;
            if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
            if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
            if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
            const int dr = rr - r, dg = gg - g, db = bb - b;
            const int err = 2 * dr * dr + 4 * dg * dg + db * db;
            if (err < best_err) {
                best_err = err; best_y = y; best_u4 = u4; best_v4 = v4;
            }
        }
    }
    if (best_y < 1) best_y = 1;   /* index != 0 must stay opaque (Y=0 = sky) */
    return (uint16_t)((best_y << 8) | (best_u4 << 4) | best_v4);
}

/* VCE 0..255 is one shared palette for KING BG0, the combined-VDC BG, and the
 * combined-VDC weapon sprites. Keep the full-bright converted colors here so
 * fades only interpolate cheap Y/U/V fields rather than rerunning RGB->YUV for
 * every color on every step. */
#define PALETTE_FADE_FULL 256
#define PALETTE_FADE_STEPS 16
static uint16_t s_scene_palette[256];
static int s_palette_fade = PALETTE_FADE_FULL;
static int s_scene_palette_valid;

static uint16_t fade_yuv(uint16_t c, int level)
{
    if (level <= 0)
        return 0x0088;                    /* neutral black */
    if (level >= PALETTE_FADE_FULL)
        return c;

    int y = ((c >> 8) * level + 128) >> 8;
    int u = 8 + (((int)((c >> 4) & 15) - 8) * level) / PALETTE_FADE_FULL;
    int v = 8 + (((int)(c & 15) - 8) * level) / PALETTE_FADE_FULL;
    if (y < 1) y = 1;                    /* stay opaque until the black endpoint */
    return (uint16_t)((y << 8) | (u << 4) | v);
}

static inline __attribute__((always_inline))
uint16_t pcfx_palette_color(int i, int mode)
{
    if (mode == PCFX_PAL_BOOT) {
        if (i == PCFX_LOAD_BAR_FRAME_INDEX) return 0x2F88; /* PLAYPAL 109 */
        if (i == PCFX_LOAD_BAR_FILL_INDEX)  return 0xD388; /* PLAYPAL 84  */
        return 0x0088;
    }
    if (i == 0)
        return 0x0088;                /* transparent RAINBOW sky key */
    if (mode == PCFX_PAL_FULL)
        return s_scene_palette[i];
    return fade_yuv(s_scene_palette[i], s_palette_fade);
}

static void upload_scene_palette(void)
{
    /* Index 0 remains transparent so the RAINBOW sky can show during gameplay.
     * The exit transition disables RAINBOW at the black endpoint. */
    pcfx_palette_stage_scene(PCFX_PAL_FADED);
}

/* Put the VDC text overlay in COLOUR right now, for the pre-title screens.
 *
 * Palette uploads are STAGED and pushed by whoever next reaches blanking — the
 * presenter, the fade loop, or I_PrepareLevelLoad_e32. Before the title, none of
 * those run: a boot-time V_SetPalette(0) staged a palette that nothing ever
 * flushed, so the VCE kept the two-grey boot palette and the red DOOM font drew
 * black-on-black. That is what made the save-device chooser an invisible prompt
 * that stopped boot dead until a button was pressed. Stage the FULL (unfaded)
 * colours — the gameplay fade does not exist yet, and PCFX_PAL_FADED at boot is
 * all black — and push them inside blanking ourselves, exactly as the LOADING...
 * screen does. Index 0 stays transparent, so a cleared KING page is unaffected. */
void pcfx_text_palette_now(void)
{
    if (!s_scene_palette_valid)
        return;                       /* no PLAYPAL yet: nothing to show in colour */
    pcfx_palette_stage_scene(PCFX_PAL_FULL);
    pcfx_palette_flush_in_blank();
}

/* rgb_to_yuv runs a per-colour least-squares search (~114 ms for a full 256-entry
 * palette). Doom shifts through 14 fixed PLAYPAL sub-palettes for damage/pickup/
 * radiation tints, re-uploading on every step of a flash. A 4-slot LRU keyed on
 * the sub-palette index used to cache conversions, but a damage flash decays
 * through more distinct indices than 4 — every miss stalled the game ~114 ms
 * (measured 250 ms worst frames = the 0.1% lows). Instead convert ALL 14
 * sub-palettes once, up front, and after that only copy + upload ready-made
 * tables. GAMMA BOOST is deliberately NOT folded into the RGB->YUV conversion:
 * doing so forced the full 14x reconversion (~1.6 s) on every gamma keypress in
 * the menu. The tables are cached at gamma 0 and the boost is applied as the
 * same mid-tone parabola on the Y byte during the 256-entry copy below — the
 * identical brightness lift in luma instead of RGB, so a gamma step now costs
 * the same as a damage-flash palette swap. Static RAM is ~full, so the 7 KB
 * table is zone memory: a one-time PU_STATIC block grabbed at the first palette
 * upload (boot, long before any level's precache arena is sized). */
#define PAL_NUMPALS 14
static uint16_t (*s_pal_all)[256];
static int s_pal_all_valid;

static void convert_palette(const byte *pallete, uint16_t *dst)
{
    /* Index 0 is transparent (Y=0) so the RAINBOW sky shows through the sky-flat
     * pixels the renderer leaves as index 0. rgb_to_yuv keeps all other entries
     * opaque (Y >= 1) so ordinary black texels don't turn into sky. */
    dst[0] = 0x0088;
    for (int i = 1; i < 256; i++)
        dst[i] = rgb_to_yuv(pallete[i * 3 + 0],
                            pallete[i * 3 + 1],
                            pallete[i * 3 + 2]);
}

void I_SetPalletteIndexed_e32(int pal, int gamma, const byte *playpal)
{
    if (!playpal) return;
    if (pal < 0) pal = 0;
    if (pal >= PAL_NUMPALS) pal = PAL_NUMPALS - 1;

    if (!s_pal_all)
        s_pal_all = Z_Malloc(PAL_NUMPALS * 256 * (int)sizeof(uint16_t),
                             PU_STATIC, NULL);

    if (!s_pal_all_valid) {
        for (int p = 0; p < PAL_NUMPALS; p++)
            convert_palette(playpal + p * 256 * 3, s_pal_all[p]);
        s_pal_all_valid = 1;
    }

    if (gamma <= 0) {
        memcpy(s_scene_palette, s_pal_all[pal], sizeof(s_scene_palette));
    } else {
        /* Mid-tone brightness parabola (pins 0 and 255, monotone on 0..255)
         * on luma only; chroma nibbles pass through unchanged, and index 0
         * keeps Y=0 so it stays the transparent sky key. */
        const uint16_t *src = s_pal_all[pal];
        for (int i = 0; i < 256; i++) {
            int y = src[i] >> 8;
            y += (y * (255 - y) * gamma) / (255 * 6);
            if (y > 255) y = 255;
            s_scene_palette[i] = (uint16_t)((y << 8) | (src[i] & 0xFF));
        }
    }
    s_scene_palette_valid = 1;
    upload_scene_palette();
}

/* Reserve the whole-run zone buffers that would otherwise be allocated lazily on the
 * first rendered frame. By then the first level's arena is reserved and the zone rover
 * sits deep inside the big free region, so a lazy block is stranded mid-heap and, being
 * unpurgeable PU_STATIC, survives every level free as a WALL splitting the free space.
 * That split is what makes W_PrecacheReserve's Z_LargestFreeBlock() read a fraction of
 * the real capacity and drop level loads off the one-read map-pack path. Allocating at
 * boot lands them beside the other statics at the bottom of the heap. */
void I_PreallocStatics_e32(void)
{
    if (!s_pal_all)
        s_pal_all = Z_Malloc(PAL_NUMPALS * 256 * (int)sizeof(uint16_t),
                             PU_STATIC, NULL);
    if (!s_vce_pal_stage)
        s_vce_pal_stage = Z_Malloc(256 * (int)sizeof(uint16_t),
                                   PU_STATIC, NULL);
    pcfx_text_prealloc();
    pcfx_weapon_prealloc();
}

void I_FadePalette_e32(int fade_in)
{
    if (!s_scene_palette_valid)
        return;

    const int start = s_palette_fade;
    const int target = fade_in ? PALETTE_FADE_FULL : 0;
    if (start == target)
        return;

    for (int step = 1; step <= PALETTE_FADE_STEPS; step++) {
        video_wait_vsync();
        s_palette_fade = start + ((target - start) * step) / PALETTE_FADE_STEPS;
        upload_scene_palette();
        /* video_wait_vsync returned at the leading edge of blanking, so this
         * step can land immediately without a field of extra fade latency. */
        pcfx_palette_flush();
    }
}

void I_PrepareLevelLoad_e32(void)
{
    /* A cleared KING page is transparent index 0. Always remove the direct-YUV
     * sky first, including restarts from active gameplay, so it cannot appear
     * behind LOADING... before the new scene and sprites have been presented. */
    I_SetRainbowActive_e32(0);

    /* Static intermission mode can leave g_pcfx_fb_base aimed at either of its
     * two pre-rendered pages. Hand control back to the normal presenter first.
     * WI_End's later pcfx_im_end() call is intentionally a no-op. The displayed
     * buffer is tracked explicitly (it can be any of the three during
     * gameplay); the I_SetRainbowActive_e32(0) above already flushed any
     * pending deferred flip, so s_display_buf is final here. */
    pcfx_im_end();
    const int display_page = s_display_buf;

    /* The palette is black on the intermission path, so clearing the displayed
     * KING page cannot tear visibly. Use a true index-0 page so restoring the
     * font colors below reveals only the VDC loading text.
     *
     * Then clear the OTHER two flip buffers as well. All three re-enter the
     * rotation as soon as gameplay resumes, and the renderer only repaints the
     * 3D view — BG0 rows 208..239 are never drawn, because the opaque VDC HUD
     * tiles cover them (that is exactly why the deferred presenter flips in that
     * band). Any pixel there keeps the PREVIOUS level's scene, or, on the first
     * load of a run, KRAM power-up garbage, for as long as the buffer lives.
     * Buffer 2 was already being cleared once for this reason when the triple
     * rotation was first entered; buffers 0 and 1 were not, and a level load is
     * the one moment with seconds to spare, so clear the set. Each buffer is
     * its own fill: 0/1 sit in KRAM bank A and 2 in bank B, and C6272_1 1.3
     * forbids one transfer crossing the bank boundary. */
    pcfx_kram_fill_guarded(pcfx_fb_page_base(display_page), 0, KFB_PAGE_WORDS);
    for (int buf = 0; buf < KFB_NUM_BUFS; buf++)
        if (buf != display_page)
            pcfx_kram_fill_guarded(pcfx_fb_page_base(buf), 0, KFB_PAGE_WORDS);

    pcfx_weapon_hide();
    pcfx_text_begin();
    pcfx_text_puts((PCFX_TEXT_COLS - 10) / 2,
                   (PCFX_TEXT_ROWS - 1) / 2, "LOADING...");

    /* Keep s_palette_fade at black so the first gameplay frame can fade in, but
     * temporarily expose the cached full colors for the VDC font. KING is an
     * all-zero page, so sharing VCE 0..255 cannot reveal stale scene pixels. */
    if (s_scene_palette_valid) {
        pcfx_palette_stage_scene(PCFX_PAL_FULL);
    }

    video_wait_vsync();
    pcfx_palette_flush();
    pcfx_weapon_present();
    pcfx_text_present();
}

/* -------------------------------------------------------------- present ---- */
/* No blit: the drawers already wrote this frame directly into KRAM page
 * s_back_page. Presenting is just a vblank-synced hardware page flip, then we
 * point the render target at the other page for the next frame. */
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
extern uint32_t g_bl_vsync, g_bl_upload, g_bl_rearm;
#endif
void pcfx_mouse_poll(void);   /* input section, below */

void I_FinishUpdate_e32(const byte *srcBuffer, const byte *pallete,
                        const unsigned int width, const unsigned int height)
{
    (void)srcBuffer; (void)pallete; (void)width; (void)height;

    /* Second mouse poll of the frame.  I_StartTic polls at the top of the game
     * loop; this one lands roughly half a loop period later, so each read spans
     * half as much motion and the mouse's ±127 count saturation clips half as
     * often on a fast turn.  Measured cost on E1M1 (automove benchmark, vs the
     * same build without this call): +0.076 ms/frame of a 66.8 ms frame, all of
     * it in the presentation phase — 0.11%, far below the 16.7 ms field quantum
     * that frame time is actually rounded to. */
    pcfx_mouse_poll();

    /* Intermission/finale: no page flip. The intermission already rendered both
     * framebuffers; just latch the one it wants shown (buffer select for the YAH
     * blink) in vblank, then flush the VDC text overlay (stats / level name). */
    if (s_im_mode)
    {
        video_wait_vsync();
        pcfx_palette_flush();
        king_set_display_page(s_im_show);
        pcfx_weapon_present();   /* upload the hidden-weapon SAT (D_Display parked it) */
        pcfx_text_present();
        return;
    }

    const int page = s_back_page;      /* the page the drawers just rendered */

#ifdef DEV_RAINBOW_TEST
    /* dev: blank the whole page to transparent index 0 so the RAINBOW should
     * fill the entire screen — confirms whether it composites at all. */
    pcfx_kram_fill_guarded(pcfx_fb_page_base(page), 0, KFB_PAGE_WORDS);
#endif

    if (s_triple)
    {
        /* Deferred present (gameplay): no raster wait. The only wait left is a
         * still-outstanding previous flip — this frame rendered faster than
         * the raster reached a window — which costs exactly what the old
         * synchronous presenter paid every frame. */
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        uint64_t _tf = itu_ticks();
#endif
        pcfx_present_flush();
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        {
            uint32_t _spin = (uint32_t)(itu_ticks() - _tf);
            g_bl_vsync += _spin;
#ifdef DEV_FRAME_TRACE_WORK
            g_rp_vsync = _spin;
#endif
        }
#endif
        pcfx_text_stage();           /* snapshot the finished VDC text tilemap */
        s_pend_buf = page;
        g_pcfx_present_pend = 1;
        s_back_page = 3 - s_display_buf - page;  /* neither shown nor pending */
        g_pcfx_fb_base = pcfx_fb_page_base(s_back_page);
        return;
    }

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    uint64_t _tv = itu_ticks();
#endif
#ifndef DEV_NO_PRESENT_VSYNC
    /* DEV_NO_PRESENT_VSYNC measures what triple buffering is worth WITHOUT a
     * third buffer.  Skipping the wait flips BG0's CG base mid-scanline: the
     * lines already scanned came from the old page and the rest come from the
     * new one, so it costs a tear line but frees the old page immediately --
     * exactly the vsync-quantization saving a third buffer would buy, and the
     * fps is identical.  Tearing is not shippable; this is an oracle for
     * pricing the no-tear version before paying for it. */
    video_wait_present_vsync();
    /* video_wait_present_vsync may reuse a blank already at its very end.
     * A full palette needs a few lines, so wait for the next leading edge. */
    if (g_pcfx_palette_pend && pcfx_tetsu_raster_stable() > 258u)
        video_wait_vsync();
#else
    /* The tearing oracle may arrive during active display; leave a staged
     * palette for a later safe poll instead of adding VCE noise to the test. */
    {
        unsigned r = pcfx_tetsu_raster_stable();
        if (r >= 240u && r <= 258u)
            pcfx_palette_flush();
    }
#endif
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    {
        uint32_t _spin = (uint32_t)(itu_ticks() - _tv);
        g_bl_vsync += _spin;
#ifdef DEV_FRAME_TRACE_WORK
        g_rp_vsync = _spin;
#endif
    }
#endif
#ifndef DEV_NO_PRESENT_VSYNC
    pcfx_palette_flush();
#endif
    king_set_display_page(page);       /* tear-free: latch in vblank */

#ifndef DEV_RB_NO_REARM
    /* Re-arm the RAINBOW for the next field at the bottom raster (~248). This runs
     * FIRST, right after the vsync — the raster is at ~240 (start of vblank), so
     * the wait to 248 is only ~8 lines (~0.5 ms). Doing it AFTER the VDC uploads
     * (as before) let the ~2 ms of upload advance the raster past 248 and wrap it,
     * forcing a ~234-line (~15 ms/frame!) spin to climb back round to 248. */
    if (s_rainbow_active)
    {
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        uint64_t _tr = itu_ticks();
#endif
        {
            uint32_t spin = 0;
            while (pcfx_tetsu_raster_stable() < RAINBOW_RESTART_RASTER
                   && spin++ < 200000u) { }
        }
        rainbow_rearm();
        s_rb_armed_field = 1;
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        g_bl_rearm += (uint32_t)(itu_ticks() - _tr);
#endif
    }
#endif

    /* Flush the VDC weapon sprites, still inside vblank / at the top of the frame,
     * well before the beam reaches the bottom-of-screen weapon — so the pattern
     * write can't race the VDC's sprite fetch (which tore the weapon on the frame
     * it changed, e.g. the muzzle-flash frame right after firing). */
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    { uint64_t _t = itu_ticks();
#endif
    pcfx_weapon_present();
    pcfx_text_present();   /* flush the BG font tilemap (also vblank, after the weapon) */
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
      g_bl_upload += (uint32_t)(itu_ticks() - _t);
    }
#endif

    s_back_page ^= 1;                  /* next frame renders the other page */
    g_pcfx_fb_base = pcfx_fb_page_base(s_back_page);
}

/* -------------------------------------------------------------- input ------ */
/* PC-FX mouse (port 2, signature 13).  Data word layout, per the PC-FX device
 * manual (GMAKER DOC/DEVICE/FXGABOAD.WRI, "②マウス"):
 *
 *   D31..D28 = signature (13)     D17 = SW-R      D16 = SW-L   ("1" = pressed)
 *   D15..D8  = X7..X0             D7..D0 = Y7..Y0
 *
 * X and Y are signed 8-bit *relative counts accumulated since the previous
 * read*, and the mouse's internal counter SATURATES rather than wraps: the
 * manual's count-range table maps "Under" and -127 both to 0x81, and +127 and
 * "Over" both to 0x7F (0x80 is never produced).  So any motion faster than
 * 127 counts per read is silently clipped — on real hardware, with the ~200
 * CPI PC-FX mouse and DOOM's ~15-20 fps loop, that is only about 0.6 inch of
 * travel per poll, which a normal fast turn exceeds easily.  Emulators hide
 * this (they feed scaled host-mouse deltas), which is why the port felt fine
 * in the emulator and sluggish on hardware.
 *
 * Mitigation: poll twice per frame into an accumulator instead of once, which
 * doubles the headroom before clipping, and hand the engine the sum.  Buttons
 * are OR-ed across the polls in an interval so a short click between tics is
 * not dropped.
 *
 * Sampling vs consumption.  D_BuildNewTiccmds runs `while (newtics--) {
 * I_StartTic(); G_BuildTiccmd(); }`, so when a rendered frame spans two or
 * more tics those polls all happen microseconds apart: the first drains
 * everything the mouse counted since the previous frame, the rest read ~0
 * because the counter was just cleared.  Per-tic that is [N, 0, N, 0, ...] —
 * the view snaps from position to position instead of turning smoothly, and
 * the bigger the per-count scale the more obvious it is.
 *
 * The engine tells us the burst length up front (I_SetTicBurst, called from
 * D_BuildNewTiccmds with its `newtics`), so the sample can just be divided
 * evenly over the tics that will consume it: [N/2, N/2] or [N/4, N/4, N/4,
 * N/4].  Exact, no filter latency, and the division remainder stays in the
 * accumulator so slow drags are never truncated away to nothing. */
static int      s_mouse_ax, s_mouse_ay;      /* counts not yet handed to a tic */
static uint32_t s_mouse_btn;
static int      s_mouse_seen;
static int      s_mouse_burst = 1;           /* tics left in this build burst  */

void pcfx_mouse_poll(void)
{
    uint32_t mraw = contrlr_pad_read(1);
    if ((mraw >> 28) != CONTRLR_TYPE_MOUSE)
        return;

    s_mouse_seen = 1;
    s_mouse_btn |= (mraw >> 16) & 3u;
    s_mouse_ax  += (int)(int8_t)(mraw >> 8);
    s_mouse_ay  -= (int)(int8_t)mraw;
}

void I_SetTicBurst(int ntics)
{
    s_mouse_burst = (ntics > 1) ? ntics : 1;
}

/* Hand this tic its 1/n share of the pending counts, leaving the remainder
 * (including the whole of any sub-share motion) in the accumulator. */
static int mouse_share(int *acc, int n)
{
    int out = *acc / n;

    *acc -= out;
    return out;
}

/* FX-Pad lives on port 1 (index 0) and is always decoded as buttons/D-pad.
 * A PC-FX mouse, when present, lives on port 2 (index 1) — the classic
 * combo-pack wiring — so it's polled and typed independently of the pad and
 * can be used at the same time. */
void I_ProcessKeyEvents(void)
{
    static const struct { uint32_t mask; int key; } map[] = {
        { 1u << 8,  KEYD_UP     },
        { 1u << 10, KEYD_DOWN   },
        { 1u << 11, KEYD_LEFT   },
        { 1u << 9,  KEYD_RIGHT  },
        { JOY_I,     KEYD_A      },  /* A / I: use/confirm  */
        { JOY_II,    KEYD_B      },  /* B / II: fire         */
        { JOY_III,   KEYD_C      },  /* C / III: cycle weapon*/
        { JOY_IV,    KEYD_X      },  /* X / IV: strafe left   */
        { JOY_V,     KEYD_Y      },  /* Y / V: strafe right   */
        { JOY_VI,    KEYD_Z      },  /* Z / VI: automap       */
        { 1u << 7,  KEYD_START  },   /* RUN   */
        { JOY_SELECT, KEYD_SELECT }, /* run modifier          */
    };
    static uint32_t prev = 0;

#ifdef DEV_TIC_PROFILE
    uint64_t _tp = itu_ticks();
#endif
    uint32_t raw = contrlr_pad_read(0);
#ifdef DEV_TIC_PROFILE
    g_rpa_pad += (uint32_t)(itu_ticks() - _tp);
    g_rpa_pad_calls++;
#endif
    uint32_t held = raw & 0x0FFFu;
    uint32_t down = held & ~prev;
    uint32_t up   = prev & ~held;
    prev = held;

    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        event_t ev;
        if (down & map[i].mask) { ev.type = ev_keydown; ev.data1 = map[i].key; D_PostEvent(&ev); }
        if (up   & map[i].mask) { ev.type = ev_keyup;   ev.data1 = map[i].key; D_PostEvent(&ev); }
    }

    pcfx_mouse_poll();
    if (s_mouse_seen) {
        /* Sticky once a mouse has answered, so the tail of a burst still gets
         * its share posted on tics where the port itself reported nothing. */
        int n = s_mouse_burst;
        event_t ev;
        ev.type  = ev_mouse;
        ev.data1 = (int)s_mouse_btn;
        ev.data2 = mouse_share(&s_mouse_ax, n);
        ev.data3 = mouse_share(&s_mouse_ay, n);
        D_PostEvent(&ev);
        s_mouse_btn = 0;

        if (s_mouse_burst > 1)
            s_mouse_burst--;
    }
}

/* -------------------------------------------------------------- lifecycle -- */
void I_InitScreen_e32(void)
{
    contrlr_pad_init(0);
    contrlr_pad_init(1);   /* port 2: PC-FX mouse, when a combo pack is plugged in */
    king_video_init();
    /* VDC weapon-sprite hardware MUST be initialised BEFORE the final tetsu
     * video-mode set (done in pcfx_rainbow_init): vdc_init_5MHz zeroes the
     * VDC display-timing registers, and the tetsu must be (re)programmed after
     * that or the composite output blanks.  (Mirrors the KING+VDC init order in
     * the PCFX3Dproject reference.) */
    pcfx_weapon_init();    /* two-VDC 256-colour weapon sprites (over the scene) */
    pcfx_text_init();      /* two-VDC 256-colour BG font tiles (over the scene, under text
                            * present); adds the BG layer to the same VDCs. MUST precede the
                            * final tetsu mode below so the tilemap is blank before bg7up on */
    pcfx_rainbow_init();   /* RAINBOW sky behind the KING framebuffer + final tetsu mode */
    pcfx_time_init();
#ifdef DEV_CD_MATRIX
    {
        /* Diagnostic burn: full CD read-path matrix on the freshly-initialised
         * video/text stack, then halt (never returns). See pcfx_cdmatrix.c. */
        extern void pcfx_cdmatrix_run(void);
        pcfx_cdmatrix_run();
    }
#endif
}

void I_CreateBackBuffer_e32(void)
{
    king_clear_back_page();
}

int I_GetTime_e32(void)
{
    return timer_tics();   /* 32-bit frames*7/12; avoids the 64-bit divide libcall */
}

#define MAX_MESSAGE_SIZE 256
void I_Error(const char *error, ...)
{
    char msg[MAX_MESSAGE_SIZE];
    va_list v;
    va_start(v, error);
    vsnprintf(msg, sizeof(msg), error, v);
    va_end(v);
    pcfx_fatal_blink(msg);
}

void I_Quit_e32(void)
{
    for (;;) { }
}
