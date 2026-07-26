/* pcfx_cdasset.c -- CD full-screen backgrounds (see pcfx_cdasset.h).
 *
 * The intermission map, the finale picture and the title are RAW CD assets: the
 * picture is stored on disc already packed as framebuffer words (2 px/word,
 * 128 words/row x 240 rows), so it can be SCSI-DMA'd straight into KRAM with no
 * decode and no RAM staging.
 *
 * They are served from a resident KRAM CACHE (framebuffer slot KFB_BG_CACHE_BUF,
 * page-0 words 0x18000.., outside the flip rotation): the first request for a
 * picture DMAs it from CD into the cache ONCE, and every repaint after that is
 * a KRAM->KRAM copy (~30720 words, ~13 ms) into the target framebuffer. The
 * previous design re-streamed ~61 KB from the CD for every buffer repaint —
 * twice per intermission STAGE (each stage re-renders both static buffers), on
 * every return to the title, and again for the finale — because an earlier,
 * undersized page-size assumption left no room to keep a copy resident (see
 * pcfx_kram.h: each hardware KRAM page is 512 KB / 0x40000 words). The cache
 * also outlives screens: E1M2's intermission reuses E1M1's cached WIMAP0 with zero CD work,
 * and only a DIFFERENT background (e.g. TITLEPIC) evicts it. */
#include <string.h>
#include "z_zone.h"
#include "pcfx.h"
#include "pcfx_kram.h"
#include "eris/cd.h"
#include "lz4_depack.h"
#include "pcfx_cdasset.h"
#include "pcfx_cdassets.h"      /* generated catalog: pcfx_cdassets[], counts */

/* Base LBA of the appended asset blob, emitted by the CD linker into lbas.h. A
 * placeholder (matching the sky's pattern) keeps code size stable before the
 * first link pass generates the real value. */
#ifdef HAVE_GENERATED_LBAS
#include "lbas.h"
#endif
#ifndef BINARY_LBA_SRC_GENERATED_PCFX_CDASSETS_BIN
#define BINARY_LBA_SRC_GENERATED_PCFX_CDASSETS_BIN 400u
#endif
#define CDASSET_BASE_LBA (BINARY_LBA_SRC_GENERATED_PCFX_CDASSETS_BIN)

/* Which raw asset each framebuffer BUFFER currently holds (DMA'd), so the title's
 * CD read happens at most once per buffer per picture change — see
 * pcfx_cd_background_dma. Invalidated whenever anything else repaints the fb. */
static int       s_dma_buf_pic[KFB_NUM_BUFS] = { -1, -1, -1 };
static void dma_cache_invalidate(void)
{
    for (int i = 0; i < (int)KFB_NUM_BUFS; i++) s_dma_buf_pic[i] = -1;
}

static int find_asset(const char *name)
{
    for (int i = 0; i < PCFX_CDASSET_COUNT; i++)
        if (!strcmp(name, pcfx_cdassets[i].name))
            return i;
    return -1;
}

/* ------------------------------------------------ resident background cache */
/* Which asset the KFB_BG_CACHE_BUF slot holds. Nothing else ever writes that
 * slot (the flip rotation and the boot clear cover buffers 0..2 only), so the
 * cache stays valid across gameplay, menus and screens until a different
 * background evicts it. -1 = KRAM power-up garbage, must load. */
static int s_cache_pic = -1;

/* Ensure `idx` is resident in the cache slot: at most one CD DMA per picture
 * CHANGE, ever. pcfx_king_dma_cd_to_fb notifies the CD-DA manager itself. */
static void bg_cache_load(int idx)
{
    if (s_cache_pic == idx)
        return;
    const pcfx_cdasset_t *a = &pcfx_cdassets[idx];
    pcfx_king_dma_cd_to_fb(CDASSET_BASE_LBA + a->sector,
                           pcfx_fb_page_base(KFB_BG_CACHE_BUF),
                           (unsigned)a->words * 2u);
    s_cache_pic = idx;
}

/* KRAM->KRAM copy, cache slot -> a framebuffer. Read and write cursors are
 * independent hardware registers sharing the data port (AR 0x0E): read_kram()
 * re-latches the AR and pulls from the read cursor, write_kram() pushes to the
 * write cursor. ~30720 words ~= 13 ms — the intermission/title repaint rate is
 * a few per SCREEN, so this replaces a ~61 KB CD read (plus seek, plus killing
 * any playing CD-DA) with main-thread work two orders of magnitude cheaper. */
static void bg_cache_blit(uint32_t dst_base, unsigned words)
{
    uint32_t src = pcfx_fb_page_base(KFB_BG_CACHE_BUF);
    unsigned i = 0;

    /* Sliced and interrupt-atomic, for the reason in pcfx_kram.h: seating the
     * cursors once and streaming 30720 words is ~13 ms of unbroken KING traffic
     * against a ~1 ms timer, and an IRQ taken inside a KING select/data sequence
     * corrupts it. Losing a cursor mid-copy sends the rest of the picture
     * somewhere else and leaves a band of the destination unwritten. */
    while (i < words) {
        unsigned n = words - i;
        uint32_t psw;

        if (n > KRAM_BURST_SLICE_WORDS)
            n = KRAM_BURST_SLICE_WORDS;
        psw = pcfx_irq_save();
        king_kram_set_read(src + i, 1);
        king_kram_set_cursor(dst_base + i, 1);
        for (unsigned j = 0; j < n; j++)
            write_kram(read_kram());
        pcfx_irq_restore(psw);
        i += n;
    }
}

/* Paint a full-screen CD background into the CURRENT draw framebuffer
 * (g_pcfx_fb_base): resident-cache fill (CD DMA at most once per picture) plus
 * a KRAM->KRAM copy. The intermission calls this for BOTH of its static
 * buffers on every stage change (splat/YAH set changes), which used to mean
 * two fresh ~61 KB CD streams per stage; now only the first stage of the first
 * intermission ever touches the disc, and the "you are here" blink stays a
 * pure page-select (pcfx_im_show). Returns 0 for a non-raw / unknown name so
 * the caller falls back to its patch. */
int pcfx_cd_background(const char *name)
{
    int idx = find_asset(name);
    if (idx < 0 || !pcfx_cdassets[idx].raw)
        return 0;
    bg_cache_load(idx);
    bg_cache_blit(g_pcfx_fb_base, (unsigned)pcfx_cdassets[idx].words);
    dma_cache_invalidate();   /* the title's per-buffer residency is now stale */
    return 1;
}

/* Draw a RAW (uncompressed) CD background into the hidden framebuffer buffer —
 * the title/finale path. Tracked per framebuffer BUFFER: the presenter flips
 * between buffers, so each buffer is painted once per picture change and every
 * later call is a no-op (the static picture already resides there). The paint
 * itself comes from the resident cache (KRAM->KRAM), so the CD is read at most
 * once per PICTURE — not once per buffer, and not at all when returning to a
 * screen whose background is still cached. Returns 1 if `name` is a known RAW
 * asset (and is resident in this buffer), 0 otherwise (caller draws its patch).
 *
 * Correctness note: this assumes nothing else repaints the title framebuffer
 * between appearances. Gameplay (pcfx_cd_background_hide) and the intermission
 * painter both call dma_cache_invalidate(), forcing a repaint on return to the
 * title; menu text rides the VDC tile layer and never touches the framebuffer. */
int pcfx_cd_background_dma(const char *name)
{
    int idx = find_asset(name);
    if (idx < 0 || !pcfx_cdassets[idx].raw)
        return 0;                 /* not a raw DMA asset -> caller draws its patch */

    int buf = (int)pcfx_fb_buf_index(g_pcfx_fb_base);   /* true buffer index */
    if (s_dma_buf_pic[buf] != idx) {
        bg_cache_load(idx);
        bg_cache_blit(g_pcfx_fb_base, (unsigned)pcfx_cdassets[idx].words);
        s_dma_buf_pic[buf] = idx;
    }
    return 1;
}

void pcfx_cd_background_hide(void)
{
    pcfx_im_end();            /* safety: ensure the normal page flip is back for gameplay */
    dma_cache_invalidate();   /* gameplay repaints the framebuffer -> re-DMA next title */
}

