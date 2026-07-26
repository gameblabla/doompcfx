/* pcfx_kram.h — direct-to-KRAM framebuffer for the DOOM renderer.
 *
 * There is NO system-RAM backbuffer. The DOOM draw routines write pixels straight
 * into the hidden KING KRAM page through the KING data port, and I_FinishUpdate
 * just flips BG0's CG base to show it. This removes the render->RAM->KRAM double
 * copy: every pixel is written exactly once, directly to video memory.
 *
 * The framebuffer is "fat pixel" / low-detail: one logical pixel = one 16-bit
 * KRAM word holding the palette index in both bytes (idx | idx<<8), i.e. two
 * identical screen pixels. That is the 16-bit-write low-detail mode the PC-FX
 * wants, and it makes every write word-aligned (no read-modify-write).
 *
 * Geometry: logical framebuffer is SCREENWIDTH(128) x SCREENHEIGHT(240) words,
 * filling the 256x240 (128-word-stride) KING BG0 plane. Two pages live at
 * KRAM word 0 and 32768; g_pcfx_fb_base points at logical pixel (0,0) of whichever
 * page is currently hidden (the render target), updated by the presenter.
 */
#ifndef PCFX_KRAM_H
#define PCFX_KRAM_H

#include <stdint.h>
#include "pcfx.h"

#define KFB_ROW_WORDS   128u          /* KRAM stride: 256 px / 2 px-per-word     */
#define KFB_PAGE_WORDS  (KFB_ROW_WORDS * 256u) /* 256x256 8bpp page = 32768 words */
#define KFB_X_WORDS     0u            /* (256-240)/2 = 8 px = 4 words left margin */
#define KFB_Y_ROWS      0u            /* 240 lines fill the 240-line plane        */

/* ---- KRAM memory map ------------------------------------------------------
 * KRAM is 2 PAGES x 262144 WORDS = 1 MB, and every word of it is real —
 * PROVIDED the KING is in 4-Mbit mode. i_system_pcfx.c king_video_init() now
 * writes REG.61 = 1 as its first KING access; read the comment there for why
 * that one register was the root cause of the 2026-07-24 burn symptoms.
 *
 * Addressing (C6272_1 figures d63cfa1d / 3d1c3015, the KRP/KWP pointers):
 * a KRAM address is D17 = −A/B BANK select + D16..D0 = 17-bit word address.
 * "Page" is NOT part of the address: each engine gets its page independently
 * from REG.0F (king_set_kram_pages / king_set_page_setting). So the flat word
 * numbers used below decompose as bank:offset —
 *
 *   words 0x00000..0x1FFFF = bank A (D17=0), offsets 0x00000..0x1FFFF
 *   words 0x20000..0x3FFFF = bank B (D17=1), offsets 0x00000..0x1FFFF
 *
 * and that pair is ONE page; the other page is the same 262144 words reached
 * by pointing an engine's REG.0F selector at page 1. Per C6272_1 1.3 a single
 * CPU or DMA transfer must not cross a bank or page boundary, and auto-increment
 * wraps within the 17-bit address field (bank bit held) — which is why every
 * region below is placed wholly inside one bank, and why the BG microprogram
 * programs bank B (slots 8..15) as a mirror of bank A (king_video_init).
 *
 *   page 0: framebuffers 0/1 at words 0x00000/0x08000 (bank A), CD DMA scratch
 *           at 0x0F800 (bank A), framebuffer 2 at 0x20000 and the resident BG
 *           cache at 0x28000 (bank B) — see pcfx_fb_page_base().
 *           Bank A 0x10000..0x1FFFF and bank B 0x30000..0x3FFFF are FREE
 *           (64 KB each); they read as open bus only in 1-Mbit mode, where the
 *           Hudson map draws them as the dotted expansion half.
 *   page 1: the RAINBOW sky stream from word 0, then the ADPCM sample bank.
 *
 * king_set_kram_pages(scsi, bg, rainbow, adpcm) does the REG.0F routing; page-1
 * CPU access sets D31 on the KRAM pointer (king_kram_set_cursor_page). Note that
 * in 1-MBIT mode C6272_1 1.3 REQUIRES every page selector to be zero, so all of
 * this page routing was silently inert before REG.61 was programmed. */
#define KRAM_RAINBOW_WORD  0x00000u   /* sky compressed stream, hw page 1 word 0  */
/* The ADPCM SFX bank follows the sky as ONE contiguous run inside page-1 bank A.
 * (It was briefly split in two to dodge a 0x10000..0x1FFFF "hole" that turned out
 * to be the 1-Mbit dotted half, not missing silicon.) Base and size live in the
 * generated src/generated/pcfx_sfx.h; see platform/i_sound_pcfx.c. */

/* CD -> system-RAM reads use libpcfx's eris_cd_read_dma(), which bounces each
 * chunk through KRAM because the KING cannot DMA directly to main RAM.
 *
 * Scratch address history, real-hardware burns (all taken while the console was
 * still in 1-Mbit mode, i.e. before REG.61 was programmed):
 *   0x20000 (bank B)      -> first WAD read FAILED at SCSI level, both armings.
 *   0x1F800 (A16 set)     -> DMA reported GOOD status but the copy-out returned
 *                            a constant 0x5555 pattern ("UU..") - the open-bus
 *                            signature of the dotted expansion half that only
 *                            4-Mbit mode populates.
 * Keep this small, page-0 window for the CD matrix diagnostic. */
#define KRAM_CD_DMA_SCRATCH_WORD   0x0F800u
#define KRAM_CD_DMA_SCRATCH_WORDS  0x00400u

/* Production CD -> RAM bounce window.
 *
 * Page 1 bank B is otherwise unused: RAINBOW and ADPCM occupy bank A, while
 * every framebuffer/background buffer is on page 0.  A whole bank is one
 * legal non-wrapping KRAM run (C6272_1 1.3), so it gives eris_cd_read_dma a
 * 256 KiB window instead of the diagnostic's 2 KiB tail.  The rebuilt
 * count-0 bounce issues one READ(10) per window: a 500-700 KiB map therefore
 * needs 2-3 consecutive commands instead of one command PER SECTOR.
 *
 * D31 selects page 1 for the CPU KRAM cursor only.  REG.09 receives the
 * within-page 0x20000 bank-B address; REG.0F's SCP bit routes SCSI to page 1. */
#define KRAM_CD_RAM_SCRATCH_WORD   0x80020000u
#define KRAM_CD_RAM_SCRATCH_WORDS  0x00020000u

/* Number of gameplay framebuffers. Buffer 2 exists so the presenter can defer
 * the vsync page flip: frame N+1 renders into the third buffer while frame N
 * waits (pending) for the raster to reach the tear-safe window, instead of the
 * CPU spinning up to a whole field for it. See platform/pcfx_present.h. */
#define KFB_NUM_BUFS    3

/* Fourth full-screen slot (page-0 words 0x28000..0x2FFFF = bank B offset
 * 0x8000..0xFFFF): resident background CACHE, never part of the flip rotation
 * and never cleared. A CD full-screen asset (WIMAP0 / INTERPIC / TITLEPIC) is
 * DMA'd here ONCE and every later repaint is a fast KRAM->KRAM copy — the
 * intermission used to re-stream ~61 KB from the CD on every stage change.
 * Its previous home, 0x18000, is in the half that 1-Mbit mode leaves
 * unpopulated (see the map notes above): the cache never held data there,
 * which is why the 07-24 burn showed the intermission repainting stale
 * framebuffer content. Bank B keeps it valid in either mode. */
#define KFB_BG_CACHE_BUF 3u


/* ---- interrupt-atomic bulk CPU->KRAM bursts --------------------------------
 * pcfx.h states the silicon-verified rule: an interval-timer IRQ taken inside a
 * KING 0x600/0x604 select/data sequence corrupts the access, so every multi-write
 * KING sequence must run between pcfx_irq_save() and pcfx_irq_restore().
 *
 * king_kram_fill() cannot obey it as written -- it seats the KRAM write cursor
 * once and then streams, and a whole-page clear is 32768-65536 words, i.e. tens
 * of milliseconds of unbroken KING traffic against a ~1 ms timer. A hit that
 * costs the loop its cursor sends every remaining word somewhere else and leaves
 * the region being cleared holding its power-on contents: a full-width band.
 * That is the stale banding the 2026-07-24 burns showed at identical rasters
 * across runs, and that pcfxemu (KING accesses atomic) never reproduces.
 *
 * Use this for every bulk fill. It slices the run and takes each slice with
 * interrupts off, re-seating the cursor per slice, so the timer keeps its ~0.1 ms
 * latency and no single hit can damage more than one slice. It does NOT wait on
 * the raster: per C6272_2 3.1.2 the KING stalls the CPU with -BUSY rather than
 * dropping the access, so K-BUS priority (3.2.2) costs time, not data.
 *
 * The renderer is not routed through this: it seats a fresh cursor per span or
 * column, which already bounds IRQ exposure to a few words. */
#define KRAM_BURST_SLICE_WORDS 256u   /* ~0.1 ms of IRQ latency per slice */
void pcfx_kram_fill_guarded(uint32_t kram_word, uint16_t value, uint32_t words);

/* KRAM word address of logical pixel (0,0) in the current render (back) page. */
extern uint32_t g_pcfx_fb_base;

/* KRAM word base of buffer 0..3 (three flip buffers + the BG cache slot).
 * NOT a linear array: buffers 0/1 are in bank A and buffers 2/3 in bank B, so
 * that no buffer straddles the bank boundary — see the map notes above. */
static inline uint32_t pcfx_fb_page_base(int page)
{
    static const uint32_t base[4] = { 0x00000u, 0x08000u, 0x20000u, 0x28000u };
    return base[page & 3] + KFB_Y_ROWS * KFB_ROW_WORDS + KFB_X_WORDS;
}

/* Inverse of pcfx_fb_page_base for the flip buffers: which buffer does a
 * framebuffer base address belong to? (Replaces the old base/KFB_PAGE_WORDS
 * division, which the non-linear layout broke.) */
static inline unsigned pcfx_fb_buf_index(uint32_t base)
{
    if (base >= 0x28000u) return 3u;
    if (base >= 0x20000u) return 2u;
    if (base >= 0x08000u) return 1u;
    return 0u;
}

/* KRAM base of the currently DISPLAYED buffer (the one on screen). With three
 * gameplay buffers this can no longer be inferred from g_pcfx_fb_base; the
 * presenter tracks it explicitly (platform/i_system_pcfx.c). Used to draw a
 * loading indicator over the frozen on-screen frame during a level load, when
 * the main loop isn't flipping pages. */
uint32_t pcfx_fb_display_base(void);

/* Row offset (in KRAM words) for logical row y. Replaces the old *SCREENPITCH
 * addressing; every framebuffer-write site computes base + FB_YOFFSET(y) + x. */
#define FB_YOFFSET(y)   ((unsigned)(y) * KFB_ROW_WORDS)

/* Begin a vertical run (column) at logical (x,y): cursor advances one row/write.
 * always_inline: GCC 4.9.4 otherwise emits an out-of-line _FB_col and calls it
 * once per drawn column (hot). */
static inline __attribute__((always_inline)) void FB_col(unsigned x, unsigned y)
{
    king_kram_set_cursor(g_pcfx_fb_base + FB_YOFFSET(y) + x, (int)KFB_ROW_WORDS);
}

/* Begin a horizontal run (span) at logical (x,y): cursor advances one word/write. */
static inline __attribute__((always_inline)) void FB_span(unsigned x, unsigned y)
{
    king_kram_set_cursor(g_pcfx_fb_base + FB_YOFFSET(y) + x, 1);
}

/* Begin an arbitrary run at KRAM word `addr` with signed increment `inc`. */
static inline __attribute__((always_inline)) void FB_at(uint32_t addr, int inc) { king_kram_set_cursor(addr, inc); }

/* Write one already-doubled fat pixel word to the cursor (auto-increments). */
static inline __attribute__((always_inline)) void FB_put(unsigned short w) { write_kram(w); }

/* Write one 8bpp palette index as a fat pixel. */
static inline void FB_puti(unsigned idx) { write_kram((unsigned short)(idx | (idx << 8))); }

/* CD -> KRAM (page 1) DMA via the KING SCSI engine (platform/i_system_pcfx.c).
 * Used to load the ADPCM SFX bank straight to KRAM at boot. `bytes` rounds up to
 * whole CD sectors. Returns 1 on success, 0 if every retry attempt failed. */
int pcfx_king_dma_cd_to_kram(unsigned lba, unsigned kram_word, unsigned bytes);

/* CD -> KRAM page 0 (framebuffer) DMA — the raw-title fast path (SCSI targets page 0).
 * `kram_word` is a within-page-0 word offset (a framebuffer buffer base). */
void pcfx_king_dma_cd_to_fb(unsigned lba, unsigned kram_word, unsigned bytes);

#endif /* PCFX_KRAM_H */
