/* pcfx_time.h — IRQ-driven millisecond clock + frame sync.
 * timer_ms_gettime64() (declared in the kos.h shim) is defined here. */
#ifndef PCFX_TIME_H
#define PCFX_TIME_H

#include <stdint.h>

/* Milliseconds since pcfx_time_init, incremented by the ~1 ms interval-timer
 * IRQ (pcfx_support.c). Real-time regardless of frame rate. */
extern volatile uint32_t g_ms_irq;

/* Install the interval-timer IRQ and start the ms clock. Call once at startup. */
void pcfx_time_init(void);

/* Real wall-clock milliseconds from the IRQ timer. Never loses time when the
 * frame loop runs slower than the field rate. */
uint64_t timer_ms_real(void);

/* Monotonic 35 Hz DOOM tic from the ms clock (ms*7/200; 32-bit, no libcall). */
int timer_tics(void);

/* Simulation-clock milliseconds. Also declared in the kos.h shim for the game
 * files; declared here too so platform code gets the correct 64-bit prototype
 * without pulling in the compat shim. */
uint64_t timer_ms_gettime64(void);

/* Block until the next vblank and sample the ITU clock. */
void video_wait_vsync(void);
void video_wait_present_vsync(void);

/* --- instrumentation --- */
uint64_t itu_ticks(void);            /* sample + return raw ITU ticks (1432/ms) */
extern volatile uint32_t g_last_spin;  /* iterations spun in the last vsync wait */
extern uint64_t g_spin_ticks;          /* ITU ticks spent in the last vsync wait */

/* --- render profiling ---
 * COARSE_RENDER_PROFILE records only phase boundaries and whole-frame totals.
 * SERIAL_LOG additionally enables sampled hot-path counters and working sets.
 * Per-frame phase timings (ITU ticks) are collected into these globals by the
 * render sites, then rprof_endframe() accumulates them for a RAM dump. */
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
extern uint32_t g_rp_tic, g_rp_bsp, g_rp_plane, g_rp_spr, g_rp_blit, g_rp_frame;
extern uint32_t g_rp_sound, g_rp_display;
extern uint32_t g_rp_view, g_rp_hud;
void rprof_endframe(void);
#endif

#ifdef DEV_FRAME_TRACE_WORK
/* The two idle spins a frame contains.  Whole-frame time is snapped up to a
 * field multiple and a frame also spins for the 35 Hz tic, so neither the frame
 * time nor (frame - vsync) is real CPU work: compute = frame - vsync - tic spin.
 * A saving on a tic-bound frame is absorbed by its spin, so this split is what
 * says whether a candidate can pay at all.  Static RAM is at its limit, so these
 * live behind the trace define rather than COARSE_RENDER_PROFILE. */
extern uint32_t g_rp_vsync;      /* this frame's wait for the field boundary */
extern uint32_t g_rp_tt_wait;    /* this frame's wait for the next game tic  */
#endif

#ifdef DEV_TIC_PROFILE
/* Game-tic sub-phase accumulators.  P_Ticker/TryRunTics run ~1.5x/frame, so an
 * itu_ticks() pair per sub-phase is safe (the rule against per-call sampling is
 * about hot loops).  Separate from COARSE_RENDER_PROFILE because the extra
 * statics overflow RAM in a plain coarse build. */
extern uint32_t g_rpa_th_player, g_rpa_th_thinkers, g_rpa_th_specials;
extern uint32_t g_rpa_th_count;      /* thinkers dispatched (not a time) */
extern uint32_t g_rpa_tt_wait, g_rpa_tt_gticker, g_rpa_tt_mticker;
extern uint32_t g_rpa_tt_runtics;    /* tics executed (not a time) */
extern uint32_t g_rpa_pad, g_rpa_pad_calls;   /* blocking FX-Pad read */
#endif

#ifdef DEV_FRAME_TRACE_COUNTS
/* Per-frame scene counts, recorded into the frame-time ring instead of the
 * phase times so a single capture pairs each frame's cost with the scene state
 * that produced it (which is what tells a long-frame class apart from a mean
 * shift).  Cheap plain counters -- no itu_ticks() in a hot loop. */
extern uint32_t g_rp_n_drawsegs, g_rp_n_visplanes, g_rp_n_vissprites;
#endif

#ifdef SERIAL_LOG
extern uint32_t g_rprof_ctr;
extern uint32_t g_rp_seg;            /* sub-bucket of bsp: R_RenderSegLoop (col draw) */
extern uint32_t g_rp_draw;           /* sampled wall-column drawer ticks */
extern uint32_t g_rp_draw_samples;   /* columns represented by g_rp_draw */
extern uint32_t g_rp_draw_pixels;    /* pixels represented by g_rp_draw */
extern uint32_t g_rp_fetch;          /* sampled wall-column fetch/compose ticks */
extern uint32_t g_rp_fetch_samples;  /* columns represented by g_rp_fetch */
extern uint32_t g_rp_wall_columns;
extern uint32_t g_rp_bbox;           /* sampled R_CheckBBox ticks */
extern uint32_t g_rp_bbox_samples;
extern uint32_t g_rp_bbox_calls;
extern uint32_t g_rp_addline;        /* sampled R_AddLine projection ticks */
extern uint32_t g_rp_addline_samples;
extern uint32_t g_rp_addline_calls;
extern uint32_t g_rp_clip;           /* sampled solid-column clipping ticks */
extern uint32_t g_rp_clip_samples;
extern uint32_t g_rp_clip_calls;
extern uint32_t g_rp_store;          /* sampled R_StoreWallRange setup ticks */
extern uint32_t g_rp_store_samples;
extern uint32_t g_rp_store_calls;
extern uint32_t g_rp_plane_setup;     /* sampled R_MapPlane setup ticks */
extern uint32_t g_rp_plane_draw;      /* sampled plane drawer ticks */
extern uint32_t g_rp_plane_samples;
extern uint32_t g_rp_plane_spans;
extern uint32_t g_rp_bsp_nodes, g_rp_bsp_subsectors, g_rp_bsp_earlyouts;

/* Logical output pixels actually submitted to the direct KING KRAM drawers. */
extern uint32_t g_rp_wall_pixels, g_rp_plane_pixels;
extern uint32_t g_rp_masked_wall_pixels, g_rp_sprite_pixels;

/* Per-frame dense-wall working sets.  Overflows make undersized diagnostic
 * hash tables visible instead of silently under-counting unique tuples. */
extern uint32_t g_rp_wall_page_cmaps, g_rp_wall_column_cmaps;
extern uint32_t g_rp_wall_page_cmap_overflow, g_rp_wall_column_cmap_overflow;

/* Reserved for the P1 pre-lit wall cache.  They intentionally stay zero until
 * that cache is enabled, giving the RAM-dump/profile reader stable symbols. */
extern uint32_t g_rp_wall_lit_hits, g_rp_wall_lit_misses;
extern uint32_t g_rp_wall_lit_bakes, g_rp_wall_lit_evictions;

void itu_resample(void);             /* re-sample the ITU accumulator (wrap-safety) */
static inline void rprof_sample(void) { if ((++g_rprof_ctr & 63u) == 0) itu_resample(); }
#else
static inline void rprof_sample(void) { }
#endif

#endif
