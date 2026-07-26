/* pcfx_text.c -- DOOM hu_font text as HuC6270 VDC background tiles (see pcfx_text.h).
 *
 * The two VDCs are already set up for the 256-colour weapon SPRITES (platform/
 * pcfx_weapon.c); this adds the BG layer on the SAME two chips. A glyph tile is split
 * across the chips exactly like a weapon cell: VDC0 carries the high nibble, VDC1 the
 * low nibble of each pixel's palette index, the mixer recombines them
 * (index = VDC0<<4 | VDC1), and a pixel is transparent where the low nibble is 0 so the
 * KING scene shows through (mednafen king.c VDC_PIXELMIX). The BG-combine is armed by
 * VDC1's BAT palette-bank bit3 (BAT entry | 0x8000). Because the BG palette offset is 0,
 * the combined 8-bit index reads VCE 0..255 == the KING scene palette, so text FADES
 * with the scene automatically (no separate font palette).
 *
 * VRAM map (per VDC, 0x10000 words):
 *   0x0000..0x03FF  BG BAT (32x32 tilemap, 1 word/cell, base 0 on the HuC6270)
 *   0x0400..0x07FF  font CG (64 tiles x 16 words: 63 glyphs + 1 blank)
 *   0x1000..0x31FF  weapon sprite slot 0, pattern bank 0
 *   0x3400..0x55FF  weapon sprite slot 0, pattern bank 1
 *   0x6000..0x81FF  weapon sprite slot 1, pattern bank 0
 *   0x9000..0xB1FF  weapon sprite slot 1, pattern bank 1
 *   0xFF00          SATB
 * BAT cell = tile_number (bits 0-11) | palette_bank<<12. VDC0 uses bank 0, VDC1 bank 8.
 */
#include <pcfx/types.h>
#include <pcfx/vdc.h>
#include "pcfx_font.h"     /* generated: pcfx_font_pat0/pat1/width, PCFX_FONT_* */
#include "pcfx_text.h"
#include "w_wad.h"         /* W_CacheLumpName: PLAYPAL for the HUD low-nibble remap */
#include "z_zone.h"

/* Font tiles live at tile number 0x40.. so their CG (tile*16) starts at VRAM 0x400,
 * just past the 32x32 BAT at 0..0x3FF. */
#define FONT_TILE_BASE   0x40
#define BAT_W            32                 /* HuC6270 32-wide BAT (256px/8)          */
#define BAT_CELLS        (BAT_W * 32)       /* 32x32 map words at VRAM 0              */
#define BLANK_TILE       (FONT_TILE_BASE + PCFX_FONT_BLANK)
#define VDC1_BANK        0x8000             /* BAT palette bank 8 -> arms BG combine  */

/* Status-bar tiles: 128 tiles (4 rows x 32 cols) at tile number 0x80.. (CG at VRAM
 * 0x800..0xFFF, just past the font CG and below the relocated weapon slot 0 at 0x1000).
 * They occupy the bottom PCFX_TEXT_HUD_ROWS cell rows. Unlike the font CG (static), the
 * HUD CG is re-uploaded when a widget changes (pcfx_text_hud_update). */
#define HUD_TILE_BASE    0x80
#define HUD_ROW0         (PCFX_TEXT_ROWS - PCFX_TEXT_HUD_ROWS)   /* first HUD cell row */
#define HUD_STRIP_W      256

/* Persistent shadow tilemap (tile numbers); only changed intervals are flushed. */
static u16 s_bat[BAT_CELLS];
static int s_dirty_first = BAT_CELLS;
static int s_dirty_last;
static int s_hud_this_frame;
static int s_hud_visible;
static int s_hud_overwritten;
static int s_bat_initialized;
static int s_text_first = BAT_CELLS;
static int s_text_last;

static inline void bat_set(int index, u16 value)
{
    if (s_bat[index] == value)
        return;
    s_bat[index] = value;
    if (index < s_dirty_first) s_dirty_first = index;
    if (index > s_dirty_last) s_dirty_last = index;
}

/* Fast VDC VRAM streaming (identical to pcfx_weapon.c): select the write-address reg
 * once, then stream words to the auto-incrementing data port. Chip 0 ports 0x400/0x404,
 * chip 1 ports 0x500/0x504. */
static inline void vdc_vram_seek(int chip, u16 addr)
{
    if (chip == 0) {
        __asm__ volatile ("out.h %0, 0x400[r0]" : : "r"(0));
        __asm__ volatile ("out.h %0, 0x404[r0]" : : "r"((int)addr));
        __asm__ volatile ("out.h %0, 0x400[r0]" : : "r"(2));
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

void pcfx_text_begin(void)
{
    /* The VDC BAT persists. Clear last frame's transient text, but leave the
     * resident HUD rows alone until present() knows whether ST_Drawer placed
     * the bar this frame. This avoids rewriting 2,048 BAT words in ordinary
     * gameplay where neither text nor HUD placement changed. */
    if (!s_bat_initialized)
        for (int i = 0; i < BAT_CELLS; i++)
            bat_set(i, BLANK_TILE);
    else if (s_text_first < BAT_CELLS)
        for (int i = s_text_first; i <= s_text_last; i++)
            bat_set(i, BLANK_TILE);
    s_bat_initialized = 1;
    s_text_first = BAT_CELLS;
    s_text_last = 0;
    s_hud_this_frame = 0;
}

void pcfx_text_putc(int col, int row, int ch)
{
    if (col < 0 || col >= PCFX_TEXT_COLS || row < 0 || row >= PCFX_TEXT_ROWS)
        return;
    if (ch < PCFX_FONT_FIRST || ch >= PCFX_FONT_FIRST + PCFX_FONT_COUNT)
        return;                                   /* space / non-printable -> leave blank */
    int index = row * BAT_W + col;
    bat_set(index,
            (u16)(FONT_TILE_BASE + (ch - PCFX_FONT_FIRST)));
    if (index < s_text_first) s_text_first = index;
    if (index > s_text_last) s_text_last = index;
    if (row >= HUD_ROW0)
        s_hud_overwritten = 1;
}

void pcfx_text_puts(int col, int row, const char *s)
{
    for (; *s && col < PCFX_TEXT_COLS; s++, col++)
        pcfx_text_putc(col, row, (unsigned char)*s);
}

/* PLAYPAL remap for the HUD strip so every bar pixel is OPAQUE ON BOTH VDCs. Two things
 * key on transparency: the mixer's combine keys on the COMBINED low nibble (VDC1), and
 * the per-VDC sprite-vs-BG test keys on EACH chip's own nibble (VDC0 = the colour's high
 * nibble, VDC1 = its low nibble). The weapon draws behind the BG (no priority bit), so
 * for the HUD tiles to hide it on BOTH chips every bar colour must have BOTH nibbles
 * non-zero — else the weapon pokes through the bar where a nibble is 0. Remap each such
 * index to the nearest-RGB PLAYPAL index with both nibbles set. Built lazily (PLAYPAL
 * must be resident). */
static u8  s_remap[256];
static int s_remap_ready;
static u8 *s_hud_shadow;
static int s_hud_shadow_valid;
static void build_remap(void)
{
    if (s_remap_ready) return;
    const u8 *pal = (const u8 *)W_CacheLumpName("PLAYPAL");
    if (!pal) return;
    for (int i = 0; i < 256; i++) {
        if ((i & 0x0F) && (i & 0xF0)) { s_remap[i] = (u8)i; continue; }  /* both nibbles set */
        int best = 0x11, bd = 1 << 30;
        for (int j = 1; j < 256; j++) {
            if (!(j & 0x0F) || !(j & 0xF0)) continue;
            int dr = pal[i*3] - pal[j*3], dg = pal[i*3+1] - pal[j*3+1], db = pal[i*3+2] - pal[j*3+2];
            int d = dr*dr + dg*dg + db*db;
            if (d < bd) { bd = d; best = j; }
        }
        s_remap[i] = (u8)best;
    }
    s_remap_ready = 1;
}

/* Stamp the BAT cells of the bottom PCFX_TEXT_HUD_ROWS rows with the HUD tiles. Called
 * every frame the bar is visible (pcfx_text_begin clears the whole map first). */
void pcfx_text_hud_place(void)
{
    s_hud_this_frame = 1;
    if (s_hud_visible && !s_hud_overwritten)
        return;
    for (int r = 0; r < PCFX_TEXT_HUD_ROWS; r++)
        for (int c = 0; c < PCFX_TEXT_COLS; c++)
            bat_set((HUD_ROW0 + r) * BAT_W + c,
                    (u16)(HUD_TILE_BASE + r * PCFX_TEXT_COLS + c));
    s_hud_overwritten = 0;
}

/* Re-upload the 128 HUD tiles' CG from the composited 256x32 PLAYPAL strip, split into
 * VDC0 (high nibble) / VDC1 (low nibble) like the font. One tile per (tr,tc); seeked per
 * tile (128 x2 — cheap, and only on a widget change). */
/* Reserve the HUD shadow at boot rather than on the first bar update. It is whole-run
 * PU_STATIC, so allocating it lazily costs the same RAM but strands it mid-heap (the
 * rover is past the level arena by the first HUD frame), leaving a permanent 8 KB wall
 * across the free block — see I_PreallocStatics_e32 / W_PreloadStatics. */
void pcfx_text_prealloc(void)
{
    if (!s_hud_shadow)
        s_hud_shadow = Z_Malloc(HUD_STRIP_W * PCFX_TEXT_HUD_ROWS * 8,
                                PU_STATIC, NULL);
}

void pcfx_text_hud_update(const unsigned char *strip)
{
    build_remap();
    pcfx_text_prealloc();

    for (int tr = 0; tr < PCFX_TEXT_HUD_ROWS; tr++)
        for (int tc = 0; tc < PCFX_TEXT_COLS; tc++) {
            int changed = !s_hud_shadow_valid;
            for (int y = 0; y < 8 && !changed; y++) {
                const u8 *src = strip + (tr * 8 + y) * HUD_STRIP_W + tc * 8;
                const u8 *old = s_hud_shadow +
                    (tr * 8 + y) * HUD_STRIP_W + tc * 8;
                const uint32_t *src32 = (const uint32_t *)src;
                const uint32_t *old32 = (const uint32_t *)old;
                if (src32[0] != old32[0] || src32[1] != old32[1])
                    changed = 1;
            }
            if (!changed)
                continue;

            unsigned tile = HUD_TILE_BASE + tr * PCFX_TEXT_COLS + tc;
            u16 cg0[16] = {0}, cg1[16] = {0};
            for (int y = 0; y < 8; y++) {
                u16 w01_0 = 0, w23_0 = 0, w01_1 = 0, w23_1 = 0;
                const unsigned char *row = strip + (tr * 8 + y) * HUD_STRIP_W + tc * 8;
                uint32_t *old32 = (uint32_t *)(s_hud_shadow +
                    (tr * 8 + y) * HUD_STRIP_W + tc * 8);
                const uint32_t *row32 = (const uint32_t *)row;
                old32[0] = row32[0];
                old32[1] = row32[1];
                for (int x = 0; x < 8; x++) {
                    unsigned idx = s_remap[row[x]];
                    unsigned hi = (idx >> 4) & 0xF, lo = idx & 0xF;
                    int b = 7 - x;
                    if (hi & 1) w01_0 |= 1 << b;     if (hi & 2) w01_0 |= 1 << (b + 8);
                    if (hi & 4) w23_0 |= 1 << b;     if (hi & 8) w23_0 |= 1 << (b + 8);
                    if (lo & 1) w01_1 |= 1 << b;     if (lo & 2) w01_1 |= 1 << (b + 8);
                    if (lo & 4) w23_1 |= 1 << b;     if (lo & 8) w23_1 |= 1 << (b + 8);
                }
                cg0[y] = w01_0; cg0[8 + y] = w23_0;
                cg1[y] = w01_1; cg1[8 + y] = w23_1;
            }
            vdc_vram_seek(0, tile * 16); for (int i = 0; i < 16; i++) vdc_vram_put(0, cg0[i]);
            vdc_vram_seek(1, tile * 16); for (int i = 0; i < 16; i++) vdc_vram_put(1, cg1[i]);
        }
    s_hud_shadow_valid = 1;
}

/* The tilemap upload is split into STAGE and FLUSH so the deferred gameplay
 * presenter (platform/pcfx_present.h) can push a frame's COMPLETED text to the
 * VDCs from a poll while the next frame's D_Display is already re-building the
 * shadow map (pcfx_text_begin runs at the very top of D_Display, long before
 * the previous frame's flip window may arrive). Stage snapshots the finished
 * map's dirty interval at present time; flush streams the snapshot to the
 * VDCs at flip time. The classic synchronous presenter calls both back to
 * back (pcfx_text_present below), which is byte-identical to the old code. */
static u16 s_bat_snap[BAT_CELLS];
static int s_snap_first = BAT_CELLS;
static int s_snap_last;

/* Reconcile HUD-row ownership and snapshot the dirty interval of the finished
 * shadow tilemap. Called once per presented frame, when the map is complete. */
void pcfx_text_stage(void)
{
    if (s_hud_visible && !s_hud_this_frame)
        for (int r = 0; r < PCFX_TEXT_HUD_ROWS; r++)
            for (int c = 0; c < PCFX_TEXT_COLS; c++) {
                int i = (HUD_ROW0 + r) * BAT_W + c;
                u16 hud = (u16)(HUD_TILE_BASE + r * PCFX_TEXT_COLS + c);
                if (s_bat[i] == hud)
                    bat_set(i, BLANK_TILE);
            }
    s_hud_visible = s_hud_this_frame;

    if (s_dirty_first >= BAT_CELLS)
        return;

    for (int i = s_dirty_first; i <= s_dirty_last; i++)
        s_bat_snap[i] = s_bat[i];
    if (s_dirty_first < s_snap_first) s_snap_first = s_dirty_first;
    if (s_dirty_last  > s_snap_last)  s_snap_last  = s_dirty_last;

    s_dirty_first = BAT_CELLS;
    s_dirty_last = 0;
}

/* Flush the staged snapshot to both VDCs' BAT (VRAM 0). VDC0 gets the raw tile
 * number (bank 0); VDC1 gets tile | 0x8000 (bank 8) to arm the 256-colour BG
 * combine. Runs in vblank so the VDCs read a complete map for the frame. */
void pcfx_text_flush(void)
{
    if (s_snap_first >= BAT_CELLS)
        return;

    vdc_vram_seek(0, (u16)s_snap_first);
    for (int i = s_snap_first; i <= s_snap_last; i++)
        vdc_vram_put(0, s_bat_snap[i]);

    vdc_vram_seek(1, (u16)s_snap_first);
    for (int i = s_snap_first; i <= s_snap_last; i++)
        vdc_vram_put(1, (u16)(s_bat_snap[i] | VDC1_BANK));

    s_snap_first = BAT_CELLS;
    s_snap_last = 0;
}

void pcfx_text_present(void)
{
    pcfx_text_stage();
    pcfx_text_flush();
}

/* One-time setup: enable the BG layer on both VDCs (CR bit7, keeping sprites' bit6),
 * upload the font CG to VRAM 0x400.. on each chip (VDC0<-pat0 high nibble, VDC1<-pat1
 * low nibble), and clear the tilemap to blank. Called from I_InitScreen_e32 right after
 * pcfx_weapon_init (which set the VDC timing / access width / scroll) and BEFORE the
 * final tetsu mode enables bg7up, so the layer is transparent from its first frame. */
void pcfx_text_init(void)
{
    for (int chip = 0; chip < 2; chip++) {
        /* CR = 0xC0: BG on (bit7) + sprites on (bit6), VRAM auto-increment +1. The
         * weapon set 0x40 (sprites only); we OR in BG. Written directly like the weapon
         * (set_control's RMW of the partly write-only CR is unreliable). */
        vdc_setreg(chip, VDC_REG_CR, VDC_CR_SB | VDC_CR_BB);

        /* Font CG: 64 tiles x 16 words at VRAM FONT_TILE_BASE*16 (= 0x400). */
        const u16 *pat = chip == 0 ? pcfx_font_pat0 : pcfx_font_pat1;
        vdc_vram_seek(chip, FONT_TILE_BASE * 16);
        for (int i = 0; i < PCFX_FONT_NTILES * PCFX_FONT_TILE_WORDS; i++)
            vdc_vram_put(chip, pat[i]);
    }

    pcfx_text_begin();     /* blank tilemap */
    pcfx_text_present();   /* push it so the BG is transparent before it is shown */
}
