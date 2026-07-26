/* pcfx_present.h — deferred (poll-serviced) presentation for gameplay frames.
 *
 * With the triple-buffered gameplay presenter, I_FinishUpdate_e32 no longer
 * spins for the raster: it records the finished frame as PENDING and returns,
 * and the flip + RAINBOW re-arm + VDC weapon/text uploads are executed by
 * pcfx_present_poll() from cheap main-thread call sites (tic wait loop, render
 * phase boundaries) whenever the Tetsu raster happens to be inside the safe
 * window. This removes both per-frame field locks (the present-vsync wait and
 * the re-arm spin — see MICRO-OPT-NEXT-STEPS.md "two field locks") without
 * giving up the hardware sky.
 *
 * THREADING/HARDWARE CONTRACT: everything here runs on the main thread. Poll
 * sites must only be placed where no KING KRAM cursor run is open (the poll
 * changes the KING register latch, so a write_kram() stream interrupted by it
 * would land in the wrong register) and where no VDC VRAM stream is open.
 * Every drawer seeks its own cursor before streaming, so any point BETWEEN
 * drawn columns/spans/patches qualifies; the chosen sites are between whole
 * subsectors / visplanes / sprites / tics.
 */
#ifndef PCFX_PRESENT_H
#define PCFX_PRESENT_H

/* Nonzero while a flip (+re-arm) is outstanding. Read by the inline gate below;
 * written only by platform/i_system_pcfx.c. */
extern int g_pcfx_present_pend;

/* Nonzero while the RAINBOW sky is composited. Also read by the gate below,
 * because the sky needs servicing on a schedule the flip does not provide.
 *
 * The HuC6271 decodes exactly ONE frame per arm, so the layer must be re-armed
 * once per FIELD (60 Hz) or it is blank for the fields that were missed. The
 * gate used to be `g_pcfx_present_pend` alone, which meant the poll -- and so
 * the re-arm inside it -- ran once per PRESENTED FRAME. Gameplay does not
 * present at 60 Hz, so most fields were never armed and the sky was there for
 * some and gone for others: visible, but not all of it. Written only by
 * platform/i_system_pcfx.c. */
extern int g_pcfx_rainbow_pend;

/* Nonzero while VCE palette entries are staged for a blanking-safe upload.
 * C6261 2.1.3 (5) warns that palette RAM writes during display put noise on
 * screen, so damage/pickup flashes must be serviced even without a page flip. */
extern int g_pcfx_palette_pend;

/* Execute any outstanding presentation step if the raster is in its window,
 * and re-arm the sky once per field. Cheap no-op when the raster is elsewhere:
 * one raster read plus a compare. */
void pcfx_present_poll(void);

/* Block (bounded) until nothing is pending. Called before anything that must
 * not run while a present is outstanding: staging the next frame's weapon SAT,
 * entering intermission static mode, leaving gameplay, or presenting the next
 * frame when it rendered faster than the raster reached a window. */
void pcfx_present_flush(void);

/* The per-site gate: two loads + branch when nothing needs doing. */
static inline void pcfx_present_tick(void)
{
    if (g_pcfx_present_pend | g_pcfx_rainbow_pend | g_pcfx_palette_pend)
        pcfx_present_poll();
}

#endif /* PCFX_PRESENT_H */
