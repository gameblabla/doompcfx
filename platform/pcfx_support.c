/* pcfx_support.c — millisecond clock and misc platform stubs.
 *
 * These satisfy the small non-e32 platform hooks the portable engine calls:
 *   - timer_ms_real()/timer_ms_gettime64()/video_wait_vsync() : the clock that
 *                backs I_GetTime(). Driven by polling the VCE vblank bit — no IRQ
 *                needed, which keeps first-boot bring-up simple and robust.
 */
#include <stdint.h>
#include <string.h>

#include "pcfx.h"
#include "pcfx_time.h"

#include <pcfx/tetsu.h>
#include <pcfx/v810.h>       /* irq_set_mask/level/handler, irq_enable */
#include <pcfx/timer.h>      /* timer_init/set_period/start/ack_irq/read_counter */

/* Vblank is detected from the HuC6261 (tetsu) raster counter, NOT the VDC's VD
 * status bit.  FXVCE_STATUS (port 0x400) is actually the HuC6270 VDC-A status
 * register, and its VD (0x20) bit "remains latched under pcfxemu" once the VDC
 * is initialised (see the PCFX3Dproject reference) — polling it after the weapon
 * VDCs come up froze this port's frame timer and hung the game loop.  The tetsu
 * raster is independent of VDC state.  Active display is lines 0..239 in the
 * 262-line mode; vblank is raster >= 240. */
#define PCFX_VBLANK_RASTER 240
static inline int pcfx_in_vblank(void)
{
    /* Stable double-read: a single raw tetsu_get_raster() can latch a
     * bogus transitional value (Tetsu HW bug), which would flip this predicate
     * at the wrong scanline and mistime the page flip / sprite uploads. */
    return pcfx_tetsu_raster_stable() >= PCFX_VBLANK_RASTER;
}

/* ---------------------------------------------------- IRQ millisecond clock --
 * A hardware interval timer (CPUclk/15 = 1.4318 MHz) fires an IRQ every ~1 ms
 * and bumps g_ms_irq. This replaces the old polled vblank-edge counter, which
 * only advanced when the game loop happened to sample it across a vblank: once
 * a frame took longer than a field (~16.7 ms) -- i.e. any time DOOM ran below
 * 60 fps -- it silently lost vblank edges, so the clock ran slow, I_GetTime
 * fell behind real time, and the game dragged into slow motion (and the FPS
 * read was wrong). The IRQ counter can't miss, so time stays real regardless of
 * frame rate. Mirrors PCFX3Dproject/.../accuratefps_cd.
 *
 * Only the interval Timer (V810 IRQ level 9 / mask bit d6) is unmasked; the
 * KING/SCSI source (d2) stays masked, so CD DMA keeps working through the
 * existing polled scsi_clear_phase_irq() path -- the ISR never touches KING. */
#define PCFX_TIMER_PERIOD 1432          /* 1432 / 1.4318 MHz ~= 1.0001 ms */

volatile uint32_t g_ms_irq = 0;         /* milliseconds since pcfx_time_init */

/* noinline so the ISR is treated as a non-leaf and saves the full register set
 * (interrupt attribute); matches the liberis hello_interrupt pattern. */
__attribute__((noinline)) static void tick_ms(void) { g_ms_irq++; }

__attribute__((interrupt)) void pcfx_timer_irq(void)
{
    timer_ack_irq();
    tick_ms();
}

void pcfx_time_init(void)
{
    g_ms_irq = 0;
    irq_set_mask(0x7F);                          /* mask every maskable source  */
    irq_set_raw_handler(0x9, pcfx_timer_irq);    /* level 9 = interval Timer     */
    timer_init();
    timer_set_period(PCFX_TIMER_PERIOD);
    timer_start(1);                          /* 1 = raise an IRQ on expiry   */
    irq_set_mask(0x3F);                          /* enable Timer only (d6=0)      */
    irq_set_level(8);                            /* accept maskable levels >= 8   */
    irq_enable();
}

uint64_t timer_ms_real(void)
{
    return (uint64_t)g_ms_irq;                   /* single 32-bit read: atomic    */
}

/* Monotonic 35 Hz DOOM tic from the ms clock: ms*35/1000 == ms*7/200 (exact;
 * 32-bit, no libcall; ms*7 wraps only past ~7 days). */
int timer_tics(void)
{
    return (int)((g_ms_irq * 7u) / 200u);
}

uint64_t timer_ms_gettime64(void)
{
    return (uint64_t)g_ms_irq;
}

/* High-res monotonic tick for the render profiler: the ms epoch scaled to timer
 * ticks plus the fraction elapsed in the current period (~0.7 us resolution).
 * The retry guards against g_ms_irq incrementing between the two reads. Kept in
 * 32-bit (one hardware `mul`, no __muldi3): it wraps ~every 50 min but is only
 * ever consumed as sub-frame deltas cast back to uint32, so the wrap is inert. */
uint64_t itu_ticks(void)
{
    uint32_t before, after;
    uint16_t counter;
    do {
        before  = g_ms_irq;
        counter = (uint16_t)timer_read_counter();
        after   = g_ms_irq;
    } while (before != after);
    return (uint32_t)(after * PCFX_TIMER_PERIOD + (PCFX_TIMER_PERIOD - (uint32_t)counter));
}

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
/* Coarse phase buckets stay available without SERIAL_LOG's hot counters, tuple
 * hashes, sampled timers, and changed zone layout. */
uint32_t g_rp_tic, g_rp_bsp, g_rp_plane, g_rp_spr, g_rp_blit, g_rp_frame;
uint32_t g_rpa_tic, g_rpa_bsp, g_rpa_plane, g_rpa_spr, g_rpa_blit, g_rpa_frame;
uint32_t g_rp_sound, g_rp_display, g_rpa_sound, g_rpa_display;
uint32_t g_rp_view, g_rp_hud, g_rpa_view, g_rpa_hud;
uint32_t g_rpa_n;
uint32_t g_bl_vsync, g_bl_upload, g_bl_rearm;   /* present sub-phase accumulators */
#endif

#ifdef SERIAL_LOG
/* Detailed profiler storage.  Use it for event ratios and same-build A/B work;
 * use COARSE_RENDER_PROFILE for release-like absolute timing. */
uint32_t g_rprof_ctr;
uint32_t g_rp_seg, g_rp_draw, g_rp_draw_samples, g_rp_draw_pixels;
uint32_t g_rp_fetch, g_rp_fetch_samples, g_rp_wall_columns;
uint32_t g_rp_bbox, g_rp_bbox_samples, g_rp_bbox_calls;
uint32_t g_rp_addline, g_rp_addline_samples, g_rp_addline_calls;
uint32_t g_rp_clip, g_rp_clip_samples, g_rp_clip_calls;
uint32_t g_rp_store, g_rp_store_samples, g_rp_store_calls;
uint32_t g_rp_plane_setup, g_rp_plane_draw, g_rp_plane_samples, g_rp_plane_spans;
uint32_t g_rp_bsp_nodes, g_rp_bsp_subsectors, g_rp_bsp_earlyouts;
uint32_t g_rp_wall_pixels, g_rp_plane_pixels;
uint32_t g_rp_masked_wall_pixels, g_rp_sprite_pixels;
uint32_t g_rp_wall_page_cmaps, g_rp_wall_column_cmaps;
uint32_t g_rp_wall_page_cmap_overflow, g_rp_wall_column_cmap_overflow;
uint32_t g_rp_wall_lit_hits, g_rp_wall_lit_misses;
uint32_t g_rp_wall_lit_bakes, g_rp_wall_lit_evictions;
uint32_t g_rpa_seg, g_rpa_draw, g_rpa_draw_samples, g_rpa_draw_pixels;
uint32_t g_rpa_fetch, g_rpa_fetch_samples, g_rpa_wall_columns;
uint32_t g_rpa_bbox, g_rpa_bbox_samples, g_rpa_bbox_calls;
uint32_t g_rpa_addline, g_rpa_addline_samples, g_rpa_addline_calls;
uint32_t g_rpa_clip, g_rpa_clip_samples, g_rpa_clip_calls;
uint32_t g_rpa_store, g_rpa_store_samples, g_rpa_store_calls;
uint32_t g_rpa_plane_setup, g_rpa_plane_draw, g_rpa_plane_samples, g_rpa_plane_spans;
uint32_t g_rpa_bsp_nodes, g_rpa_bsp_subsectors, g_rpa_bsp_earlyouts;
uint32_t g_rpa_wall_pixels, g_rpa_plane_pixels;
uint32_t g_rpa_masked_wall_pixels, g_rpa_sprite_pixels;
uint32_t g_rpa_wall_page_cmaps, g_rpa_wall_column_cmaps;
uint32_t g_rpa_wall_page_cmap_overflow, g_rpa_wall_column_cmap_overflow;
uint32_t g_rpa_wall_lit_hits, g_rpa_wall_lit_misses;
uint32_t g_rpa_wall_lit_bakes, g_rpa_wall_lit_evictions;
uint32_t g_rpm_wall_page_cmaps, g_rpm_wall_column_cmaps;
void itu_resample(void) { }
#endif

#ifdef DEV_FRAME_TRACE_COUNTS
uint32_t g_rp_n_drawsegs, g_rp_n_visplanes, g_rp_n_vissprites;
#endif

#ifdef DEV_FRAME_TRACE_WORK
uint32_t g_rp_vsync, g_rp_tt_wait;
#endif

#ifdef DEV_TIC_PROFILE
uint32_t g_rpa_th_player, g_rpa_th_thinkers, g_rpa_th_specials, g_rpa_th_count;
uint32_t g_rpa_tt_wait, g_rpa_tt_gticker, g_rpa_tt_mticker, g_rpa_tt_runtics;
uint32_t g_rpa_pad, g_rpa_pad_calls;
#endif

#ifdef DEV_FRAME_TRACE
/* Per-frame time trace for 1%/0.1%-low analysis.  A ring of the last 1024
 * frames' (whole-frame, tic, display, blit) durations in ITU-ticks>>4 units
 * (~11.2 us, saturating at ~733 ms).  Lives in its own fixed RAM buffer so the
 * zone/arena layout of the benchmarked game is untouched.  Recover with
 * tools/read_frame_trace.py from a `--dump ram` image (symbol _g_ftrace,
 * marker "PCFXFTR!"). */
#define FRAME_TRACE_CAP 1024u
static struct {
    char     marker[8];
    uint32_t n;                         /* frames recorded since boot */
    uint16_t rec[FRAME_TRACE_CAP][4];   /* see rprof_endframe below   */
} g_ftrace __attribute__((aligned(4))) =
    { {'P','C','F','X','F','T','R','!'}, 0, {{0}} };

static inline uint16_t ftrace_q(uint32_t ticks)
{
    uint32_t q = ticks >> 4;
    return (uint16_t)(q > 0xFFFFu ? 0xFFFFu : q);
}
#endif

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
void rprof_endframe(void)
{
#ifdef DEV_FRAME_TRACE
    {
        uint16_t *r = g_ftrace.rec[g_ftrace.n & (FRAME_TRACE_CAP - 1u)];
#if defined(DEV_FRAME_TRACE_WORK)
        /* Work-split variant.  A frame is: spin for the 35 Hz tic, compute,
         * then spin for the field boundary.  Both spins are idle, so only
         * (work - tic spin) is real CPU work -- and a saving on a tic-bound
         * frame is absorbed by its spin rather than raising fps. */
        r[0] = ftrace_q(g_rp_frame);
        r[1] = ftrace_q(g_rp_frame - g_rp_vsync);   /* work = compute + tic spin */
        r[2] = ftrace_q(g_rp_vsync);                /* idle: waiting for vsync  */
        r[3] = ftrace_q(g_rp_tt_wait);              /* idle: waiting for a tic  */
#elif defined(DEV_FRAME_TRACE_COUNTS)
        /* Scene-count attribution variant: keeps the whole-frame time in r[0]
         * so one capture yields both the long-frame set and its scene state. */
        r[0] = ftrace_q(g_rp_frame);
        r[1] = (uint16_t)g_rp_n_drawsegs;
        r[2] = (uint16_t)g_rp_n_visplanes;
        r[3] = (uint16_t)g_rp_n_vissprites;
#elif defined(DEV_FRAME_TRACE_RENDER)
        /* Render-phase attribution variant: spikes were shown to live in
         * D_Display, so break the render core out per frame instead. */
        r[0] = ftrace_q(g_rp_frame);
        r[1] = ftrace_q(g_rp_bsp);
        r[2] = ftrace_q(g_rp_plane);
        r[3] = ftrace_q(g_rp_spr);
#else
        r[0] = ftrace_q(g_rp_frame);
        r[1] = ftrace_q(g_rp_tic);
        r[2] = ftrace_q(g_rp_display);
        r[3] = ftrace_q(g_rp_blit);
#endif
        g_ftrace.n++;
    }
#endif
    g_rpa_tic += g_rp_tic;   g_rpa_bsp += g_rp_bsp;     g_rpa_plane += g_rp_plane;
    g_rpa_spr += g_rp_spr;   g_rpa_blit += g_rp_blit;   g_rpa_frame += g_rp_frame;
    g_rpa_sound += g_rp_sound; g_rpa_display += g_rp_display;
    g_rpa_view += g_rp_view; g_rpa_hud += g_rp_hud;
#ifdef SERIAL_LOG
    g_rpa_seg += g_rp_seg;   g_rpa_draw += g_rp_draw;   g_rpa_fetch += g_rp_fetch;
    g_rpa_draw_samples += g_rp_draw_samples; g_rpa_draw_pixels += g_rp_draw_pixels;
    g_rpa_fetch_samples += g_rp_fetch_samples; g_rpa_wall_columns += g_rp_wall_columns;
    g_rpa_bbox += g_rp_bbox;       g_rpa_bbox_samples += g_rp_bbox_samples;
    g_rpa_bbox_calls += g_rp_bbox_calls;
    g_rpa_addline += g_rp_addline; g_rpa_addline_samples += g_rp_addline_samples;
    g_rpa_addline_calls += g_rp_addline_calls;
    g_rpa_clip += g_rp_clip;       g_rpa_clip_samples += g_rp_clip_samples;
    g_rpa_clip_calls += g_rp_clip_calls;
    g_rpa_store += g_rp_store;     g_rpa_store_samples += g_rp_store_samples;
    g_rpa_store_calls += g_rp_store_calls;
    g_rpa_plane_setup += g_rp_plane_setup; g_rpa_plane_draw += g_rp_plane_draw;
    g_rpa_plane_samples += g_rp_plane_samples; g_rpa_plane_spans += g_rp_plane_spans;
    g_rpa_bsp_nodes += g_rp_bsp_nodes;
    g_rpa_bsp_subsectors += g_rp_bsp_subsectors;
    g_rpa_bsp_earlyouts += g_rp_bsp_earlyouts;
    g_rpa_wall_pixels += g_rp_wall_pixels;
    g_rpa_plane_pixels += g_rp_plane_pixels;
    g_rpa_masked_wall_pixels += g_rp_masked_wall_pixels;
    g_rpa_sprite_pixels += g_rp_sprite_pixels;
    g_rpa_wall_page_cmaps += g_rp_wall_page_cmaps;
    g_rpa_wall_column_cmaps += g_rp_wall_column_cmaps;
    g_rpa_wall_page_cmap_overflow += g_rp_wall_page_cmap_overflow;
    g_rpa_wall_column_cmap_overflow += g_rp_wall_column_cmap_overflow;
    g_rpa_wall_lit_hits += g_rp_wall_lit_hits;
    g_rpa_wall_lit_misses += g_rp_wall_lit_misses;
    g_rpa_wall_lit_bakes += g_rp_wall_lit_bakes;
    g_rpa_wall_lit_evictions += g_rp_wall_lit_evictions;
    if (g_rp_wall_page_cmaps > g_rpm_wall_page_cmaps)
        g_rpm_wall_page_cmaps = g_rp_wall_page_cmaps;
    if (g_rp_wall_column_cmaps > g_rpm_wall_column_cmaps)
        g_rpm_wall_column_cmaps = g_rp_wall_column_cmaps;
#endif
    g_rpa_n++;
}
#endif

/* Block until the next vblank rising edge (used by the presenter for page-flip
 * timing). Still polled off the tetsu raster -- the page flip wants to land in
 * vblank, which a 1 ms timer IRQ can't pinpoint. Bounded spin so a wedged VCE
 * can never hang the frame loop forever. */
void video_wait_vsync(void)
{
    uint32_t spin = 0;
    /* wait for end of any current vblank */
    while (pcfx_in_vblank() && spin++ < 2000000u) { }
    spin = 0;
    /* wait for start of the next vblank */
    while (!pcfx_in_vblank() && spin++ < 2000000u) { }
}

/* A completed hidden page may be selected during a vblank that is already in
 * progress.  Unlike animation/fade pacing, presentation does not require a new
 * rising edge; waiting through the current blank and a full active field merely
 * repeats the old page for another 16.7 ms. */
void video_wait_present_vsync(void)
{
    if (pcfx_in_vblank())
        return;

    uint32_t spin = 0;
    while (!pcfx_in_vblank() && spin++ < 2000000u) { }
}
