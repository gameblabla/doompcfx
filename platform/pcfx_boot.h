/* pcfx_boot.h — boot-time loading indicator (see pcfx_boot.c). */
#ifndef PCFX_BOOT_H
#define PCFX_BOOT_H

/* Existing neutral grayscale entries in DOOM's base PLAYPAL. The progress bar
 * writes only these pixel indices and never changes VCE colors itself. */
#define PCFX_LOAD_BAR_FRAME_INDEX 109u /* RGB 47,47,47   */
#define PCFX_LOAD_BAR_FILL_INDEX   84u /* RGB 211,211,211 */

/* BOOT bar: page 0 (displayed through boot), linear estimate. */
void pcfx_boot_progress_begin(void); /* draw the empty bar; enable ticks       */
void pcfx_boot_progress_tick(void);  /* advance one step (called per CD read)   */
void pcfx_boot_progress_end(void);   /* snap to full; disable further ticks     */
/* Replace the short operation label shown with the percentage above the bar.
 * Uses a boot-safe built-in font, so it works before PLAYPAL/WAD UI data loads. */
void pcfx_boot_progress_set_label(const char *label);
/* Record the CD operation currently in flight. During loading it is shown in a
 * separate top-screen diagnostic panel; fatal errors retain the last operation. */
void pcfx_boot_progress_set_io(const char *source, unsigned lba, unsigned sectors);
/* Sticky one-line diag-panel annotation (e.g. "CD MODE PIO" from the CD-path
 * fallback); also shown on the fatal panel. */
void pcfx_boot_progress_set_note(const char *note);

/* LEVEL-LOAD bar: drawn over the frozen on-screen frame, asymptotic fill. Shares
 * the tick (pcfx_boot_progress_tick, called per CD read from pcfx_cd_account). */
void pcfx_load_progress_begin(void);
void pcfx_load_progress_end(void);

/* Fatal-error indicator: shows the formatted message plus retained stage/CD
 * context at the top and blinks the load-bar geometry forever. Never returns. */
void pcfx_fatal_blink(const char *message);

#ifdef DEV_CD_MATRIX
/* Boot-safe drawing borrowed by the CD-DMA matrix harness, which runs before
 * PLAYPAL exists (so the VDC text layer is all-black) and reads back KRAM (so
 * every CPU burst must be raster-guarded). See the block at the end of
 * pcfx_boot.c. */
void pcfx_boot_dev_begin(void);   /* claim the displayed page, paint it        */
void pcfx_boot_dev_text(unsigned x, unsigned y, const char *text);
void pcfx_boot_dev_kram_guard(unsigned words); /* wait for blank room for N words */
#endif

#endif /* PCFX_BOOT_H */
