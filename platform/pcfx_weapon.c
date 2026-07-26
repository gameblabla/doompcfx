/* pcfx_weapon.c -- first-person weapon rendered as HuC6270 VDC hardware sprites
 * in 256-COLOUR combined-VDC mode.
 *
 * WHY hardware sprites: the DOOM software renderer used to draw the weapon into
 * the KING KRAM framebuffer -- half horizontal resolution, re-composited every
 * frame, and hand-clipped.  The two HuC6270 VDCs can overlay it for free at full
 * 256px horizontal resolution and clip it above the HUD via their own vertical
 * display window.
 *
 * 256-COLOUR SPRITE CONTRACT (mednafen/king.c mixer, verified in pcfxemu):
 *   - Both VDCs carry the SAME sprite geometry (x, y, pattern, size).  The
 *     mixer combines them:  index = (VDC0.pixel4bpp << 4) | VDC1.pixel4bpp
 *     so VDC0 holds the HIGH nibble, VDC1 the LOW nibble of the 8-bit index.
 *   - A pixel is TRANSPARENT iff VDC1's low nibble is 0 (the converter keeps
 *     every opaque colour in a slot whose low nibble != 0; slot 0 is the only
 *     transparent slot).
 *   - The SPR-combine only fires when VDC1's sprite has palette-bank bit3 set
 *     in its SAT flags (0x8).  So VDC1 sprites use flags bank 8, VDC0 bank 0.
 *   - The 8-bit index is a DOOM PLAYPAL index and reads the same VCE 0..255 bank
 *     as KING. Damage/bonus/radiation palettes and display fades therefore tint
 *     the weapon together with the scene and VDC BG text/HUD.
 *
 * PATTERN VRAM LAYOUT (HuC6270 16x64 sprite): a tall sprite is 4 vertical 16x16
 * blocks addressed at pattern numbers no, no+2, no+4, no+6 (word offsets
 * no*64, +128, +256, +384).  Each block = 4 bitplanes x 16 rows.  The generated
 * cell data stores the 4 blocks contiguously (256 words); on upload we scatter
 * block b to VRAM word (cellbase + b*128).
 *
 * The generated pcfx_weapon_frames/cells/pat0/pat1 (tools/gen_pcfx_weapons.py)
 * hold every frame's patterns in main RAM; we upload the active frame's cells to
 * an INACTIVE VDC VRAM bank only when the frame changes, then publish a SAT that
 * points at the completed bank.  Rebuild the small SAT every frame (the weapon
 * bobs).  Double-buffering the patterns is essential: a changed frame is too large
 * to update in-place before scanout starts on every frame, so the VDC could otherwise
 * fetch a half-old/half-new weapon even though the upload began during vblank.
 */

#include <pcfx/types.h>
#include <pcfx/tetsu.h>
#include <pcfx/vdc.h>

#include "pcfx_weapons.h"   /* generated */
#include "pcfx_weapon.h"
#include "pcfx_present.h"   /* flush a deferred present before re-staging the SAT */
#include "lz4_depack.h"     /* per-frame weapon pattern blocks are LZ4 (decoded on frame change) */
#include "z_zone.h"         /* the decode scratch is boot-time PU_STATIC zone memory */
/* Vertical display window for the VDCs: the FULL 240-line active area (vdispwid = 239).
 * The status bar now lives on the VDC BG-tile layer (platform/pcfx_text.c), so the VDC
 * must render the bottom 32 HUD lines too. The weapon is no longer hardware-clipped
 * above the HUD; instead the opaque HUD BG tiles (BG priority > sprite priority, set in
 * i_system_pcfx.c) cover any weapon pixels that reach the HUD region. */
#define PCFX_WEP_VDISPWID 239

/* ------------------------------------------------------------ VRAM map ----- */
/* Each cell = 16x64 sprite = 8 pattern slots (no, no+1..no+7; we use the even
 * blocks 0/2/4/6) = 512 VRAM words.  Each psprite slot has TWO disjoint banks so
 * changed art is never written over patterns that the current scanout may still
 * be fetching.  The SAT is DMA'd from the top of VRAM. */
#define WEP_MAX_CELLS   PCFX_WEP_MAX_CELLS   /* largest frame (generated); sizes the decode scratch */
#define WEP_NOS_PER_CELL 8            /* pattern-number stride per 16x64 cell  */
/* Slot 0 bank 0 starts above the BG BAT/font/HUD CG region (VRAM 0..0x0FFF).
 * A maximum 17-cell frame occupies 0x2200 words, so all four ranges below are
 * disjoint even if a non-flash frame is accidentally submitted in slot 1. */
#define SLOT0_BANK0_BASE_NO  0x040    /* weapon bank 0: VRAM 0x1000..0x31FF */
#define SLOT0_BANK1_BASE_NO  0x0D0    /* weapon bank 1: VRAM 0x3400..0x55FF */
#define SLOT1_BANK0_BASE_NO  0x180    /* flash  bank 0: VRAM 0x6000..0x81FF */
#define SLOT1_BANK1_BASE_NO  0x240    /* flash  bank 1: VRAM 0x9000..0xB1FF */
#define SATB_VRAM_ADDR  0xFF00        /* SAT source for the VRAM->SATB DMA     */
#define NUM_SLOTS       2

/* SAT flags: height=64 (bits12-13 = 2<<12), width=16 (bit8=0), palette bank in low
 * nibble.  VDC1 (low nibble) MUST set bank bit3 (0x8) to arm the mixer's SPR-combine;
 * VDC0's bank is irrelevant to the combined colour.
 *
 * The sprite PRIORITY bit (0x80) is deliberately CLEARED so the weapon draws BEHIND the
 * VDC BG layer, not in front of it (vdc_video.c: a sprite overwrites a BG pixel only
 * when the BG pixel is transparent OR the sprite has priority). The BG is blank
 * (transparent) over the 3D scene, so the weapon shows there; the BG is the OPAQUE
 * status-bar tiles over the bottom 32 lines, so the HUD covers the weapon there. This
 * restores the old vdispwid=207 weapon clip now that the VDC window spans the full 240
 * lines for the HUD tiles. (Opaqueness is per-VDC-nibble, so HUD tile colours are
 * remapped to have BOTH nibbles non-zero — see platform/pcfx_text.c build_remap.) */
#define SAT_FLAGS_BASE  (0x2000)
#define SAT_FLAGS_VDC0  (SAT_FLAGS_BASE | 0x0000)
#define SAT_FLAGS_VDC1  (SAT_FLAGS_BASE | 0x0008)

static const u16 slot_base_no[NUM_SLOTS][2] = {
    { SLOT0_BANK0_BASE_NO, SLOT0_BANK1_BASE_NO },
    { SLOT1_BANK0_BASE_NO, SLOT1_BANK1_BASE_NO }
};

/* Write a 16-bit HuC6261 (tetsu) register directly.  liberis' tetsu_set_
 * 7up_palette uses out.b, which drops the high byte where the SPRITE palette
 * offset lives, so it can only ever leave sprites reading VCE 0..255 (the KING
 * scene palette).  Ports: 0x300 = register select, 0x304 = data (see tetsu.S). */
static inline void tetsu_write_reg(int reg, u16 val)
{
    __asm__ volatile ("out.h %0, 0x300[r0]" : : "r"(reg));
    __asm__ volatile ("out.h %0, 0x304[r0]" : : "r"((int)val));
}

/* One shared SAT (x/y/pattern common to both chips; word3 flags differ). */
static u16 s_sat[64 * 4];
static int s_nsprites;
/* s_slot_fidx[slot] = frame add() requested for this slot this frame (-1 = the
 * slot is unused).  s_slot_bank is the bank encoded into this frame's RAM SAT;
 * s_active_bank is the bank named by the last SAT we published.  Each bank tracks
 * its resident frame independently.  A changed frame always targets the OTHER
 * bank, so uploading can never corrupt the patterns named by the old SAT. */
static int s_slot_fidx[NUM_SLOTS]     = { -1, -1 };
static int s_slot_bank[NUM_SLOTS]     = {  0,  0 };
static int s_active_bank[NUM_SLOTS]   = {  0,  0 };
static int s_uploaded_fidx[NUM_SLOTS][2] = { { -1, -1 }, { -1, -1 } };
static int s_ready;

/* --------------------------------------------------------- vram upload ----- */
/* Fast VDC VRAM streaming.  libpcfx's vdc_vram_write re-selects the data
 * register (reg 2) and pays a function-call per word; a full weapon frame is
 * thousands of words, so that upload is slow enough to spill out of the vblank /
 * top-of-frame window and race the VDC's per-scanline sprite fetch.  Here we
 * select the VRAM write-address register (reg 0) once, write the address, select
 * the data register (reg 2) once, then stream words straight to the data port,
 * which auto-increments — ~4x fewer I/O writes.  VDC ports (7up.S): chip 0
 * select=0x400 data=0x404, chip 1 select=0x500 data=0x504. */
static inline void vdc_vram_seek(int chip, u16 addr)
{
    if (chip == 0) {
        __asm__ volatile ("out.h %0, 0x400[r0]" : : "r"(0));          /* sel reg0 */
        __asm__ volatile ("out.h %0, 0x404[r0]" : : "r"((int)addr));  /* wr addr  */
        __asm__ volatile ("out.h %0, 0x400[r0]" : : "r"(2));          /* sel reg2 */
    } else {
        __asm__ volatile ("out.h %0, 0x500[r0]" : : "r"(0));
        __asm__ volatile ("out.h %0, 0x504[r0]" : : "r"((int)addr));
        __asm__ volatile ("out.h %0, 0x500[r0]" : : "r"(2));
    }
}
static inline void vdc_vram_put(int chip, u16 v)
{
    if (chip == 0) __asm__ volatile ("out.h %0, 0x404[r0]" : : "r"((int)v));
    else           __asm__ volatile ("out.h %0, 0x504[r0]" : : "r"((int)v));
}

static void upload_cell(int chip, const u16 *pat, u16 pat_ofs, u16 cell_no)
{
    const u16 vram_base = cell_no * 64;
    for (int b = 0; b < 4; b++) {          /* 4 vertical 16x16 blocks */
        vdc_vram_seek(chip, vram_base + b * 128);
        const u16 *src = pat + pat_ofs + b * 64;
        for (int w = 0; w < 64; w++)
            vdc_vram_put(chip, src[w]);
    }
}

/* One plane's worth of a decoded frame (the largest frame's cells). The weapon patterns
 * live in the program as per-frame LZ4 blocks (pcfx_weapon_pat0c/pat1c) — ~84 KB smaller
 * than the old flat arrays, RAM reclaimed for the zone heap so every map's pack fits. On
 * a frame CHANGE (a few times/second) we decode the active frame's block for one plane
 * into this scratch, upload its cells to VDC VRAM, then do the other plane. This runs in
 * the deferred vblank upload (pcfx_weapon_present), off the per-scanline path — the same
 * lz4_depack the renderer already runs, and a frame's ~4-9 KB decodes in well under the
 * top-of-frame budget (verified: no frame-rate change with the weapon firing).
 *
 * The 8.7 KB scratch is ZONE memory, not static: static RAM sits a few hundred
 * bytes below the link-time overflow assert, and the deferred-present work
 * needed that headroom back (same PU_STATIC boot-bottom pattern as s_pal_all /
 * s_hud_shadow — see I_PreallocStatics_e32). */
static u16 *s_wep_scratch;

void pcfx_weapon_prealloc(void)
{
    if (!s_wep_scratch)
        s_wep_scratch = Z_Malloc(WEP_MAX_CELLS * PCFX_WEP_CELL_WORDS *
                                 (int)sizeof(u16), PU_STATIC, NULL);
}

static void upload_plane(int chip, const unsigned char *blockc, int nc,
                         const pcfx_wep_frame_t *wf, u16 base_no)
{
    pcfx_weapon_prealloc();                     /* lazy fallback; boot preallocates */
    lz4_depack(blockc, s_wep_scratch);          /* whole frame's cells for this plane */
    for (int k = 0; k < nc; k++) {
        const pcfx_wep_cell_t *cell = &pcfx_weapon_cells[wf->cell_start + k];
        upload_cell(chip, s_wep_scratch, cell->pat_ofs, base_no + k * WEP_NOS_PER_CELL);
    }
}

static void upload_frame(int slot, int bank, int fidx)
{
    const pcfx_wep_frame_t *wf = &pcfx_weapon_frames[fidx];
    const u16 base_no = slot_base_no[slot][bank];
    int nc = wf->ncells;
    if (nc > WEP_MAX_CELLS) nc = WEP_MAX_CELLS;
    upload_plane(0, pcfx_weapon_pat0c + wf->c0_off, nc, wf, base_no); /* high nibble */
    upload_plane(1, pcfx_weapon_pat1c + wf->c1_off, nc, wf, base_no); /* low  nibble */
}

/* -------------------------------------------------------------- public ----- */
int pcfx_weapon_lookup(int spritenum, int frame)
{
    for (int i = 0; i < PCFX_WEP_NUM_FRAMES; i++)
        if (pcfx_weapon_frames[i].spr == spritenum &&
            pcfx_weapon_frames[i].frame == frame)
            return i;
    return -1;
}

void pcfx_weapon_begin(void)
{
    /* The deferred gameplay presenter uploads the PREVIOUS frame's staged SAT/
     * patterns from a raster poll (platform/pcfx_present.h). That staging (and
     * the bank bookkeeping below) must not be rebuilt while it is still
     * outstanding, so finish it first. R_DrawPlayerSprites stages at the END
     * of the render pass, tens of ms after the previous present, so the flip
     * window has virtually always been serviced by now — this spin only bites
     * on abnormally fast frames, where it equals the old synchronous wait. */
    pcfx_present_flush();

    s_nsprites = 0;
    s_slot_fidx[0] = s_slot_fidx[1] = -1;   /* no slot placed yet this frame */
    /* Park all 64 SAT entries offscreen (y=0 -> screen line -0x40, fully above
     * the display) so stale sprites never show. */
    for (int i = 0; i < 64; i++) {
        s_sat[i * 4 + 0] = 0;
        s_sat[i * 4 + 1] = 0;
        s_sat[i * 4 + 2] = 0;
        s_sat[i * 4 + 3] = 0;
    }
}

void pcfx_weapon_add(int slot, int fidx, int x_left, int y_top)
{
    if (slot < 0 || slot >= NUM_SLOTS || fidx < 0) return;

    /* Record which frame this slot needs; the pattern VRAM upload is DEFERRED to
     * pcfx_weapon_present() so it runs in vblank instead of mid-scanout.  Only
     * the SAT (positions) is built here, in RAM — no VDC writes. */
    s_slot_fidx[slot] = fidx;

    /* Reuse the active bank when it already contains this frame. Otherwise build
     * the requested frame in the inactive bank.  The old SAT continues to point
     * at the untouched active bank until pcfx_weapon_present publishes the new one. */
    int bank = s_active_bank[slot];
    if (s_uploaded_fidx[slot][bank] != fidx)
        bank ^= 1;
    s_slot_bank[slot] = bank;

    const pcfx_wep_frame_t *wf = &pcfx_weapon_frames[fidx];
    const u16 base_no = slot_base_no[slot][bank];
    int nc = wf->ncells;
    if (nc > WEP_MAX_CELLS) nc = WEP_MAX_CELLS;

    for (int k = 0; k < nc && s_nsprites < 64; k++) {
        const pcfx_wep_cell_t *cell = &pcfx_weapon_cells[wf->cell_start + k];
        int px = x_left + cell->col * PCFX_WEP_CELL_W;
        int py = y_top  + cell->row * PCFX_WEP_CELL_H;
        u16 cell_no = base_no + k * WEP_NOS_PER_CELL;

        int e = s_nsprites++;
        s_sat[e * 4 + 0] = (u16)((py + 0x40) & 0x3FF);   /* y (VDC bias 0x40) */
        s_sat[e * 4 + 1] = (u16)((px + 0x20) & 0x3FF);   /* x (VDC bias 0x20) */
        s_sat[e * 4 + 2] = (u16)((cell_no << 1) & 0x7FF);/* pattern (no<<1)   */
        s_sat[e * 4 + 3] = SAT_FLAGS_VDC0;               /* VDC0 flags; VDC1 ORs 0x8 */
    }
}

void pcfx_weapon_end(void)
{
    /* The assembled frame (SAT in RAM + any changed pattern data) is flushed to
     * the VDCs by pcfx_weapon_present(), which the presenter calls inside vblank.
     * Nothing here touches the hardware. */
    s_ready = 1;
}

void pcfx_weapon_hide(void)
{
    /* Assemble an empty frame (all 64 SAT entries parked offscreen) and mark it
     * ready; the next pcfx_weapon_present() flush pushes it to the VDCs so the
     * weapon disappears on non-level screens. */
    pcfx_weapon_begin();
    pcfx_weapon_end();
}

void pcfx_weapon_present(void)
{
    if (!s_ready) return;      /* no fresh frame assembled since the last flush */
    s_ready = 0;

    /* This function starts in vblank / at the top of the frame (the presenter
     * calls it right after the vsync-timed page flip), but a large changed weapon
     * frame can outlast that window.
     *
     * ORDER MATTERS.  The VDC latches its internal sprite table from VRAM (the
     * SATB DMA) at the START of the visible frame, then fetches sprite PATTERN
     * words live during each scanline.  So:
     *
     * 1) Upload changed patterns into the INACTIVE bank.  The currently latched
     *    SAT still points at the other bank, so scanout remains coherent no matter
     *    how long this takes.
     * 2) Only after both VDC planes are complete, publish the SAT that names the
     *    new bank.  If its latch is missed, the old SAT and old bank remain a
     *    coherent pair for one extra frame; if it is caught, the new pair is ready. */

    /* 1) Complete any changed frame in its inactive pattern bank. */
    for (int slot = 0; slot < NUM_SLOTS; slot++) {
        int fidx = s_slot_fidx[slot];
        int bank = s_slot_bank[slot];
        if (fidx >= 0 && fidx != s_uploaded_fidx[slot][bank]) {
            upload_frame(slot, bank, fidx);
            s_uploaded_fidx[slot][bank] = fidx;
        }
    }

    /* 2) Publish the two SAT sources in VRAM.  Use the same direct streaming
     * path as pattern uploads: the liberis per-word helper re-selects reg 2 for
     * every word and made these two small tables take long enough to straddle
     * the VDW boundary on occasional fields.
     *
     * Do NOT start a one-shot SATB DMA here.  The two VDCs reach VDW a few CPU
     * writes apart, so arming VDC0 and VDC1 sequentially near that boundary can
     * make one latch this SAT while the other keeps the preceding SAT for one
     * field.  In 256-colour mode that combines unrelated high/low nibbles and
     * produces the brief purple/white weapon corruption.  Both chips instead
     * run repeated SATB DMA, armed once by pcfx_weapon_init(), and therefore
     * latch these completed sources together at their common VDW transition. */
    vdc_vram_seek(0, SATB_VRAM_ADDR);
    for (int i = 0; i < 64 * 4; i++)
        vdc_vram_put(0, s_sat[i]);

    vdc_vram_seek(1, SATB_VRAM_ADDR);
    for (int i = 0; i < 64; i++) {
        vdc_vram_put(1, s_sat[i * 4 + 0]);
        vdc_vram_put(1, s_sat[i * 4 + 1]);
        vdc_vram_put(1, s_sat[i * 4 + 2]);
        u16 f3 = s_sat[i * 4 + 3];
        if (f3) f3 = SAT_FLAGS_VDC1;                     /* active entry -> VDC1 flags */
        vdc_vram_put(1, f3);
    }

    /* This SAT is now the newest coherent pair. Unused slots stay parked but keep
     * their last active-bank identity for efficient reuse when they reappear. */
    for (int slot = 0; slot < NUM_SLOTS; slot++) {
        if (s_slot_fidx[slot] >= 0)
            s_active_bank[slot] = s_slot_bank[slot];
    }
}

/* ----------------------------------------------------------------- init ---- */
static void vdc_init_chip(int chip)
{
    /* libpcfx owns both HuC6270s.  Its initializer clears VRAM and programs a
     * safe 5 MHz baseline; the raw settings below deliberately retain Doom's
     * combined-sprite timing and register contract. */
    vdc_init_5MHz(chip);

    /* Force CR/MWR explicitly.  The emulator only draws a VDC's sprites when its
     * CR bit6 is set (vdc_video.c:720); set_control's read-modify-write of a
     * partly write-only CR proved UNRELIABLE for chip 0 — its high-nibble sprites
     * never drew, so the 256-colour combine collapsed to the low nibble only and
     * the weapon lost almost all of its colour.  Writing CR directly fixes it.
     * CR = 0x40: sprites ON, BG off, VRAM auto-increment field (bits 11-12) = 0
     * (=+1, needed for the streaming pattern upload).  MWR = 0 = standard 4-plane
     * sprites (MWR half-fetch mode is bits 2-3 == 4, which we avoid). */
    vdc_setreg(chip, VDC_REG_CR, VDC_CR_SB);               /* sprites on */
    vdc_setreg(chip, VDC_REG_MWR, VDC_MWR_SCREEN_32x32);

    /* Set timing on BOTH VDCs so their sprite pixels land at pixel-identical
     * screen positions — required for the 256-colour nibble combine (chip 0 =
     * high nibble, chip 1 = low nibble).  If only chip 0 has valid timing, chip
     * 1's sprites misalign and the combined image scrambles.  vdispwid clips the
     * active area to the 3D view (see PCFX_WEP_VDISPWID). */
    vdc_setreg(chip, VDC_REG_HSR, 0x0202);
    vdc_setreg(chip, VDC_REG_HDR, 0x041F);
    vdc_setreg(chip, VDC_REG_VPR, 0x1102);
    vdc_setreg(chip, VDC_REG_VDR, PCFX_WEP_VDISPWID);
    vdc_setreg(chip, VDC_REG_VCR, 0x0002);

    vdc_set_scroll(chip, 0, 0);
}

void pcfx_weapon_init(void)
{
    vdc_init_chip(0);   /* VDC-A: high nibble of the combined 256-colour sprite */
    vdc_init_chip(1);   /* VDC-B: low nibble (+ SAT palette-bank bit3)          */

    /* Tetsu palette-offset reg 4: both BG (low byte) and sprites (high byte)
     * start at VCE 0. Generated weapon patterns contain safe DOOM PLAYPAL
     * indices, so KING, VDC BG, and VDC sprites now share every palette change. */
    tetsu_write_reg(4, 0x0000);

    pcfx_weapon_begin();
    pcfx_weapon_end();
    pcfx_weapon_present();  /* initialize both VRAM SAT sources with parked sprites */

    /* Repeated SATB DMA makes both halves of the 256-colour sprite latch only at
     * their common VDW transition.  Arm it only after the empty sources above
     * are complete, so startup cannot DMA uninitialized VRAM into either SAT. */
    vdc_setreg(0, VDC_REG_DCR, VDC_DCR_SATB_AUTO);
    vdc_setreg(1, VDC_REG_DCR, VDC_DCR_SATB_AUTO);
    vdc_set_satb_address(0, SATB_VRAM_ADDR);
    vdc_set_satb_address(1, SATB_VRAM_ADDR);
}
