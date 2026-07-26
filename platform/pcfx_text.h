/* pcfx_text.h -- DOOM HUD/menu font drawn as HuC6270 VDC BACKGROUND TILES.
 *
 * All message / menu / finale text (the hu_font glyphs) is drawn on an 8x8 tile grid
 * overlaid on top of the KING framebuffer by the two HuC6270 VDCs in 256-colour
 * combined-BG mode -- the same nibble combine the weapon sprites use, but for the BG
 * layer (see platform/pcfx_text.c, tools/gen_pcfx_font.py). The tile palette IS the
 * KING scene palette (VCE 0..255), so the text fades with the scene for free.
 *
 * Cell grid: 32 cols x 30 rows (256x240 / 8). Usage per frame: begin() clears every
 * cell, putc/puts place glyphs, present() flushes the tilemap to the VDCs in vblank
 * (called by the presenter after the weapon flush). */
#ifndef PCFX_TEXT_H
#define PCFX_TEXT_H

#define PCFX_TEXT_COLS 32
#define PCFX_TEXT_ROWS 30

#define PCFX_TEXT_HUD_ROWS 4                   /* bottom 4 cell rows = the 32px HUD bar */

void pcfx_text_init(void);                     /* upload font CG, enable the BG layer  */
void pcfx_text_begin(void);                    /* clear all cells (blank tile)         */
void pcfx_text_putc(int col, int row, int ch); /* place one glyph                      */
void pcfx_text_puts(int col, int row, const char *s);
void pcfx_text_present(void);                  /* stage + flush (classic sync presenter)*/
void pcfx_text_stage(void);                    /* snapshot finished tilemap (present)   */
void pcfx_text_flush(void);                    /* stream staged snapshot to VDC VRAM    */

/* Push the full PLAYPAL to the VCE now, so text drawn BEFORE the title is visible.
 * Palette uploads are otherwise staged and flushed by the presenter / fade / level
 * load, none of which run at boot — see the definition in i_system_pcfx.c. Call it
 * after V_SetPalette() on any pre-title screen. */
void pcfx_text_palette_now(void);

/* Status bar on the VDC layer. The engine composites the 256x32 bar (background +
 * digits + face + icons) into a PLAYPAL-indexed RAM strip (src/st_lib.c); here it
 * becomes 128 BG tiles in the bottom 4 cell rows. hud_update re-uploads the tile CG
 * (call on a widget change); hud_place stamps the BAT cells for those rows (call every
 * frame the bar is visible, after pcfx_text_begin clears them). */
void pcfx_text_hud_update(const unsigned char *strip256x32);
void pcfx_text_hud_place(void);

/* Allocate the HUD shadow buffer now (boot) instead of on the first bar update, so it
 * cannot strand itself mid-heap and split the zone's big free block. */
void pcfx_text_prealloc(void);

#endif
