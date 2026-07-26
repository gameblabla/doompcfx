/* pcfx_cdasset.h -- CD-streamed full-screen backgrounds (intermission / finale).
 *
 * These 256x240 pictures are NOT baked into the RAM program image (that starved
 * Doom's zone heap and hung the boot on a 2 MB PC-FX). They live on the CD as a
 * sector-aligned blob (tools/gen_pcfx_cdassets.py) and are read into a transient
 * RAM buffer only while their screen is on. See docs/cd-streaming-plan.md. */
#ifndef PCFX_CDASSET_H
#define PCFX_CDASSET_H

/* Draw the CD background named `name` (e.g. "WIMAP0", "HELP2") into the current
 * render (back) framebuffer page. Loads it from CD on the first call for a given
 * name (cached until a different one is asked for, or _hide() is called), then
 * blits it every call (the page flips each frame). Returns 1 if `name` is a known
 * CD asset (and was drawn), 0 if it is not — the caller then falls back to its
 * usual patch draw. */
int pcfx_cd_background(const char *name);

/* Like pcfx_cd_background, but for a RAW (uncompressed) asset: DMAs it straight from CD
 * into the hidden framebuffer buffer (no LZ4 decode, no CPU blit — the title fast path).
 * Cached per framebuffer buffer so the CD is read at most once per buffer. Returns 1 if
 * `name` is a known raw asset (and is now resident in this buffer), 0 otherwise. */
int pcfx_cd_background_dma(const char *name);

/* Release the cached background buffer (call when returning to gameplay). */
void pcfx_cd_background_hide(void);

/* --- Intermission/finale "static two-buffer" display (platform/i_system_pcfx.c) ---
 * The intermission pre-renders BOTH framebuffers (buffer 0 = no "you are here",
 * buffer 1 = with it) and page-selects between them each frame, so the blink is a
 * register write, not a repaint, and no RAM background buffer is needed at all.
 *   pcfx_im_begin()     enter the mode (no per-frame page flip); draw target = 0.
 *   pcfx_im_draw_to(b)  aim subsequent drawing (raw-bg DMA + splat/YAH patches) at fb b.
 *   pcfx_im_show(b)     select which fb the presenter latches on screen this frame.
 *   pcfx_im_end()       leave the mode; resume the normal double-buffer flip. */
void pcfx_im_begin(void);
void pcfx_im_end(void);
void pcfx_im_draw_to(int buf);
void pcfx_im_show(int buf);

#endif
