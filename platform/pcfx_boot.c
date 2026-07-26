/* pcfx_boot.c — CD-load progress indicator (boot AND level load).
 *
 * A full-screen load streams many lumps from the CD before anything new can be
 * drawn, during which the framebuffer holds a stale frame and looks hung. Draw a
 * simple progress bar so a load reads as working. Two users:
 *
 *   BOOT (pcfx_boot_progress_*): reaching the title streams the whole static UI
 *   set. Page 0 is the DISPLAYED page throughout boot (the presenter doesn't flip
 *   until D_DoomLoop starts), and the count of CD reads is roughly known, so the
 *   bar draws to page 0 and fills LINEARLY toward BOOT_READS_EST.
 *
 *   LEVEL LOAD (pcfx_load_progress_*): P_SetupLevel + R_PrecacheLevel stream the
 *   map and its graphics while the previous frame (title / intermission) stays on
 *   screen. The read count varies wildly — a packed map is a couple dozen reads, a
 *   scattered map is hundreds — so there is no good linear estimate. The bar draws
 *   to the currently DISPLAYED page and fills ASYMPTOTICALLY (ticks/(ticks+K)): it
 *   always advances on each read and never falsely reaches 100%, then snaps full
 *   when the load ends. So it reads as "working" for any map.
 *
 * Both are ticked once per CD read from pcfx_cd_account (platform/pcfx_wad.c), which
 * every wad AND map-pack read passes through. Ticks are ignored once _end() runs, so
 * the per-lump streaming that continues during gameplay never scribbles the live frame.
 *
 * The bar never writes the VCE palette. Its frame/fill pixels use two grayscale
 * entries already present in DOOM's base PLAYPAL (109 and 84). The video initializer
 * seeds those same colors while the rest of the boot palette is black, so this also
 * works before PLAYPAL is available without any load-time palette mutation.
 */
#include "pcfx.h"
#include "pcfx_kram.h"
#include "pcfx_boot.h"
#include "pcfx_time.h"   /* video_wait_vsync() -- see bar_kram_guard() */
#include "buildid.h"     /* generated per build (Makefile): PCFX_BUILD_ID */
#ifndef PCFX_BUILD_ID    /* editor tooling / stale generated dir fallback */
#define PCFX_BUILD_ID "UNSTAMPED"
#endif

/* Bar geometry in framebuffer words (x: 0..127) and rows (y: 0..239). */
#define BAR_X0   20u
#define BAR_X1   108u
#define BAR_W    (BAR_X1 - BAR_X0)
#define BAR_Y0   206u
#define BAR_Y1   214u

#define STATUS_Y0        197u
#define STATUS_FONT_W    3u
#define STATUS_ADVANCE   4u
#define STATUS_LABEL_MAX 17u
#define DIAG_SOURCE_MAX  11u
#define DIAG_NOTE_MAX    15u
#define DIAG_PANEL_Y0    4u
#define DIAG_PANEL_Y1    32u
#define FATAL_PANEL_Y1   76u   /* extended for the build-id row at y=68 */

#define IDX_FRAME PCFX_LOAD_BAR_FRAME_INDEX
#define IDX_FILL  PCFX_LOAD_BAR_FILL_INDEX

/* Approx CD reads to reach the title (boot's LINEAR estimate; only scales the bar). */
#define BOOT_READS_EST 14u
/* Asymptotic softness for the LEVEL bar: at K reads the bar is 50% full. Chosen so a
 * packed map (~2 dozen reads) reaches ~2/3 and a scattered map (~hundreds) nears full. */
#define LOAD_SOFT_K    12u

static int      s_active;
static unsigned s_ticks;
static uint32_t s_base;       /* framebuffer word base of the page we draw into    */
static int      s_asymptotic; /* 0 = linear/BOOT_READS_EST, 1 = ticks/(ticks+K)     */
static int      s_complete;
static char     s_label[STATUS_LABEL_MAX + 1];
static char     s_io_source[DIAG_SOURCE_MAX + 1];
static unsigned s_io_lba;
static unsigned s_io_sectors;
static int      s_io_valid;
static int      s_io_panel_drawn;  /* diag panel background already painted     */
static char     s_note[DIAG_NOTE_MAX + 1];   /* e.g. "CD MODE PIO"              */
static unsigned s_fill_w;     /* bar-fill width already on screen (delta draws)    */
static unsigned s_status_pct; /* percent last drawn by status_draw (~0u = never)   */

/* Boot-safe 3x5 uppercase font. Each row occupies three bits in a packed u16,
 * top row first. It deliberately uses only the same pre-seeded grayscale index
 * as the bar, so status text is visible before PLAYPAL or WAD fonts exist. */
#define GLYPH(a,b,c,d,e) \
    (uint16_t)(((a) << 12) | ((b) << 9) | ((c) << 6) | ((d) << 3) | (e))
static const uint16_t s_font36[36] = {
    /* 0-9 */
    GLYPH(7,5,5,5,7), GLYPH(2,6,2,2,7), GLYPH(7,1,7,4,7),
    GLYPH(7,1,7,1,7), GLYPH(5,5,7,1,1), GLYPH(7,4,7,1,7),
    GLYPH(7,4,7,5,7), GLYPH(7,1,2,2,2), GLYPH(7,5,7,5,7),
    GLYPH(7,5,7,1,7),
    /* A-Z */
    GLYPH(2,5,7,5,5), GLYPH(6,5,6,5,6), GLYPH(3,4,4,4,3),
    GLYPH(6,5,5,5,6), GLYPH(7,4,6,4,7), GLYPH(7,4,6,4,4),
    GLYPH(3,4,5,5,3), GLYPH(5,5,7,5,5), GLYPH(7,2,2,2,7),
    GLYPH(1,1,1,5,2), GLYPH(5,5,6,5,5), GLYPH(4,4,4,4,7),
    GLYPH(5,7,7,5,5), GLYPH(5,7,7,7,5), GLYPH(2,5,5,5,2),
    GLYPH(6,5,6,4,4), GLYPH(2,5,5,7,3), GLYPH(6,5,6,5,5),
    GLYPH(3,4,2,1,6), GLYPH(7,2,2,2,2), GLYPH(5,5,5,5,7),
    GLYPH(5,5,5,5,2), GLYPH(5,5,7,7,5), GLYPH(5,5,2,5,5),
    GLYPH(5,5,2,2,2), GLYPH(7,1,2,4,7)
};
#undef GLYPH

/* Every bar draw below writes straight into the currently DISPLAYED page (there
 * is no hidden page to draw into and flip during boot/level-load) — so an
 * unsynchronized write lands at whatever raster position the KING happens to be
 * scanning out at that instant. On pcfxemu that's invisible (no KRAM
 * bus-arbitration model at all), but on real hardware the HuC6272 manual is
 * explicit about CPU KRAM data access: wait for an accessible K-BUS interval or
 * obey the busy indication — BG fetch holds top K-BUS priority during active
 * display. Violations are what garbled the load bar and boot text on silicon.
 *
 * The previous fix waited for vblank ONCE per draw group
 * (video_wait_present_vsync), which has two holes: it returns immediately even
 * at the last microsecond of a blank, and a draw group (a panel clear is ~7000
 * KRAM words) is far bigger than one blank window anyway — so most words still
 * landed mid-scanout. This guard closes both: every KRAM burst asks for the
 * words it is about to write, and only proceeds if the raster is inside the
 * vertical blank with enough lines left (conservative words-per-line budget);
 * otherwise it waits for the next blank RISING EDGE (full window). Bursts here
 * are at most one 128-word row, so after a fresh edge the check always passes
 * and the loop terminates. Big draws simply spread across several fields. */
#define BLANK_FIRST_RASTER    240u  /* == PCFX_VBLANK_RASTER (pcfx_support.c)  */
#define BLANK_LAST_RASTER     261u  /* TETSU_LINES_262: lines 240..261 blank   */
#define BLANK_WORDS_PER_LINE  64u   /* conservative CPU->KRAM words per line   */

static void bar_kram_guard(unsigned words)
{
    for (;;) {
        unsigned r = pcfx_tetsu_raster_stable();
        if (r >= BLANK_FIRST_RASTER && r <= BLANK_LAST_RASTER) {
            unsigned lines_left = BLANK_LAST_RASTER + 1u - r;
            if (lines_left * BLANK_WORDS_PER_LINE >= words)
                return;
        }
        video_wait_vsync();     /* next rising edge = a full blank window */
    }
}

/* Bulk CPU->KRAM fill, sliced and interrupt-atomic (declared in pcfx_kram.h).
 *
 * This is the fix for the stale noise bands that a whole-page clear failed to
 * remove on the 2026-07-24 hardware burns -- three full-width bands, at the
 * SAME rasters in two different runs, on a page king_video_init had just filled
 * with zero.
 *
 * It is the silicon-verified KING rule from pcfx.h applied where it had never
 * been applied: an interval-timer IRQ taken inside a KING 0x600/0x604
 * select/data sequence corrupts the access (maka/tank3d bring-up, libpcfx
 * probes 027-029, this same console), so every multi-write KING sequence must
 * run with interrupts off. king_kram_fill() was the one place that flatly could
 * not -- it seats the write cursor ONCE and then streams, so a 65536-word page
 * clear is tens of milliseconds of continuous KING traffic against a ~1 ms
 * timer: dozens of hits, every one of them inside the sequence. Uniform fills
 * hide single-word damage, but not damage that costs the loop its cursor: from
 * that point the writes land elsewhere and the region being cleared keeps its
 * power-on contents, which is a band. pcfxemu performs KING accesses
 * atomically, which is exactly why none of it shows in emulation.
 *
 * Slicing is what makes the rule affordable here. Masking interrupts across a
 * whole page clear would stall the millisecond clock for tens of milliseconds;
 * per-slice masking bounds both the IRQ latency and, because each slice re-seats
 * its own cursor, the blast radius of anything that still gets through.
 *
 * NOTE: this deliberately does NOT call bar_kram_guard(). K-BUS contention is
 * not a data-loss mechanism -- C6272_2 3.1.2 has the KING assert -BUSY and stall
 * the CPU until the access completes, so the priority order in 3.2.2 costs time,
 * not words. Gating bulk fills on the blank window would add a field wait per
 * slice (seconds per page clear) against a problem that isn't there. The boot
 * bar keeps its own guard: its bursts are tiny and it is already paid for. */
void pcfx_kram_fill_guarded(uint32_t kram_word, uint16_t value, uint32_t words)
{
    while (words) {
        uint32_t n = words > KRAM_BURST_SLICE_WORDS ? KRAM_BURST_SLICE_WORDS : words;
        uint32_t psw = pcfx_irq_save();

        king_kram_set_cursor(kram_word, 1);
        for (uint32_t i = 0; i < n; i++)
            write_kram(value);
        pcfx_irq_restore(psw);

        kram_word += n;
        words     -= n;
    }
}

static void bar_hline(unsigned y, unsigned x0, unsigned x1, unsigned idx)
{
    uint32_t psw;
    bar_kram_guard(x1 - x0);
    /* Interrupt-atomic: a timer IRQ inside the seek+stream corrupts KING on
     * real hardware (see pcfx_irq_save in pcfx.h) — this, not raster overlap,
     * garbled the panel text while the solid bar looked clean. */
    psw = pcfx_irq_save();
    king_kram_set_cursor(s_base + y * KFB_ROW_WORDS + x0, 1);
    for (unsigned x = x0; x < x1; x++)
        write_kram((unsigned short)(idx | (idx << 8)));
    pcfx_irq_restore(psw);
}

static unsigned status_percent(void)
{
    unsigned p;
    if (s_complete)
        return 100;
    if (s_asymptotic)
        p = (100u * s_ticks) / (s_ticks + LOAD_SOFT_K);
    else
        p = (100u * s_ticks) / BOOT_READS_EST;
    return p > 99u ? 99u : p;
}

static unsigned status_glyph(int ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch -= 'a' - 'A';
    if (ch >= '0' && ch <= '9')
        return s_font36[ch - '0'];
    if (ch >= 'A' && ch <= 'Z')
        return s_font36[10 + ch - 'A'];
    if (ch == '%')
        return (unsigned)((5u << 12) | (1u << 9) | (2u << 6) | (4u << 3) | 5u);
    if (ch == ':')
        return (unsigned)((0u << 12) | (2u << 9) | (0u << 6) | (2u << 3) | 0u);
    if (ch == '-')
        return (unsigned)(7u << 6);
    if (ch == '.')
        return 2u;
    if (ch == '/')
        return (unsigned)((1u << 12) | (1u << 9) | (2u << 6) | (4u << 3) | 4u);
    if (ch == '=')
        return (unsigned)((7u << 9) | (7u << 3));
    return 0;
}

static void tiny_putc(unsigned x, unsigned y, int ch,
                      unsigned fg, unsigned bg)
{
    unsigned glyph = status_glyph(ch);
    uint32_t psw;
    bar_kram_guard(5u * STATUS_FONT_W);
    psw = pcfx_irq_save();   /* interrupt-atomic, see bar_hline */
    for (unsigned row = 0; row < 5; row++) {
        unsigned bits = (glyph >> (12u - row * 3u)) & 7u;
        king_kram_set_cursor(s_base + (y + row) * KFB_ROW_WORDS + x, 1);
        for (unsigned col = 0; col < STATUS_FONT_W; col++) {
            unsigned idx = (bits & (4u >> col)) ? fg : bg;
            write_kram((unsigned short)(idx | (idx << 8)));
        }
    }
    pcfx_irq_restore(psw);
}

static void tiny_line(unsigned y, const char *text, unsigned fg, unsigned bg)
{
    unsigned n = 0;
    while (text[n] && n < 32u)
        n++;
    unsigned width = n ? n * STATUS_ADVANCE - 1u : 0u;
    unsigned x = (KFB_ROW_WORDS - width) / 2u;
    for (unsigned i = 0; i < n; i++, x += STATUS_ADVANCE)
        tiny_putc(x, y, text[i], fg, bg);
}

static unsigned append_text(char *dst, unsigned n, unsigned cap, const char *src)
{
    if (src)
        while (*src && n + 1u < cap)
            dst[n++] = *src++;
    dst[n] = 0;
    return n;
}

static unsigned append_uint(char *dst, unsigned n, unsigned cap, unsigned value)
{
    char rev[10];
    unsigned nr = 0;
    do {
        rev[nr++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && nr < sizeof(rev));
    while (nr && n + 1u < cap)
        dst[n++] = rev[--nr];
    dst[n] = 0;
    return n;
}

static void panel_clear(unsigned y0, unsigned y1, unsigned idx)
{
    for (unsigned y = y0; y < y1; y++)
        bar_hline(y, 0, KFB_ROW_WORDS, idx);
}

/* Redraw one text row of the diag panel: blank its 5-px band (plus 1 px of
 * spacing below) back to the panel colour, then write the line. Bounded work —
 * the full panel background is painted once (io_panel_draw), not per update. */
static void io_panel_line(unsigned y, const char *text)
{
    for (unsigned yy = y; yy < y + 6u; yy++)
        bar_hline(yy, 0, KFB_ROW_WORDS, IDX_FRAME);
    tiny_line(y, text, IDX_FILL, IDX_FRAME);
}

static void io_panel_draw(void)
{
    char line[32];
    unsigned n;
    if (!s_io_valid)
        return;

    if (!s_io_panel_drawn) {
        panel_clear(DIAG_PANEL_Y0, DIAG_PANEL_Y1, IDX_FRAME);
        s_io_panel_drawn = 1;
    }
    n = append_text(line, 0, sizeof(line), "CD ");
    n = append_text(line, n, sizeof(line), s_io_source);
    io_panel_line(7, line);

    n = append_text(line, 0, sizeof(line), "LBA ");
    n = append_uint(line, n, sizeof(line), s_io_lba);
    n = append_text(line, n, sizeof(line), " SECTORS ");
    append_uint(line, n, sizeof(line), s_io_sectors);
    io_panel_line(14, line);

    if (s_note[0])
        io_panel_line(21, s_note);
}

static void status_draw(void)
{
    char line[5 + STATUS_LABEL_MAX + 1];
    unsigned p = status_percent();
    unsigned n = 0;

    if (p == s_status_pct)
        return;                 /* nothing visible changed since last draw */
    s_status_pct = p;

    line[n++] = p == 100u ? '1' : ' ';
    line[n++] = p >= 10u ? (char)('0' + (p / 10u) % 10u) : ' ';
    line[n++] = (char)('0' + p % 10u);
    line[n++] = '%';
    line[n++] = ' ';
    for (unsigned i = 0; s_label[i] && n < sizeof(line) - 1; i++)
        line[n++] = s_label[i];
    line[n] = 0;

    for (unsigned y = STATUS_Y0; y < STATUS_Y0 + 5u; y++)
        bar_hline(y, BAR_X0, BAR_X1, 0);

    unsigned width = n ? n * STATUS_ADVANCE - 1u : 0u;
    unsigned x = BAR_X0 + (BAR_W - width) / 2u;
    for (unsigned i = 0; i < n; i++, x += STATUS_ADVANCE)
        tiny_putc(x, STATUS_Y0, line[i], IDX_FILL, 0);
}

static void bar_fill(void)
{
    unsigned w;
    if (s_asymptotic)
        w = (BAR_W * s_ticks) / (s_ticks + LOAD_SOFT_K);
    else
        w = (BAR_W * s_ticks) / BOOT_READS_EST;
    if (w > BAR_W) w = BAR_W;
    if (w <= s_fill_w)
        return;                 /* the fill only ever grows within one bar */
    for (unsigned y = BAR_Y0; y < BAR_Y1; y++)
        bar_hline(y, BAR_X0 + s_fill_w, BAR_X0 + w, IDX_FILL);
    s_fill_w = w;
}

static void set_label(const char *label)
{
    unsigned i = 0;
    if (label)
        while (label[i] && i < STATUS_LABEL_MAX) {
            s_label[i] = label[i];
            i++;
        }
    s_label[i] = 0;
}

/* Draw the empty bar frame on `base` and start counting ticks. */
static void progress_begin(uint32_t base, int asymptotic, const char *label)
{
    s_base       = base;
    s_asymptotic = asymptotic;
    s_active     = 1;
    s_ticks      = 0;
    s_complete   = 0;
    s_fill_w     = 0;
    s_status_pct = ~0u;
    s_io_panel_drawn = 0;
    set_label(label);

    /* Solid frame block; the fill (inset by 1 px on every side) leaves a border. */
    for (unsigned y = BAR_Y0 - 1; y <= BAR_Y1; y++)
        bar_hline(y, BAR_X0 - 1, BAR_X1 + 1, IDX_FRAME);
    status_draw();

    /* Boot bar only (page-0 blank screen): stamp the build id above the
     * status row so every hardware photo identifies its exact build. The
     * asymptotic LEVEL bar draws over a live game frame -- skip it there. */
    if (!asymptotic)
        tiny_line(STATUS_Y0 - 9u, PCFX_BUILD_ID, IDX_FILL, IDX_FRAME);
}

/* One CD read happened: advance the fill (shared by both bars). No-op when inactive. */
void pcfx_boot_progress_tick(void)
{
    if (!s_active)
        return;
    s_ticks++;
    bar_fill();
    status_draw();
}

void pcfx_boot_progress_set_label(const char *label)
{
    if (!s_active)
        return;
    set_label(label);
    s_status_pct = ~0u;         /* label changed: force the status redraw */
    status_draw();
}

void pcfx_boot_progress_set_io(const char *source, unsigned lba, unsigned sectors)
{
    unsigned i = 0;
    if (source)
        while (source[i] && i < DIAG_SOURCE_MAX) {
            s_io_source[i] = source[i];
            i++;
        }
    s_io_source[i] = 0;
    s_io_lba = lba;
    s_io_sectors = sectors;
    s_io_valid = 1;

    if (s_active)
        io_panel_draw();
}

/* Sticky one-line annotation on the diag panel (e.g. which CD transfer mode
 * the DMA->PIO fallback settled on). Kept across bars and shown by the fatal
 * panel too, so a hardware photo reports the verdict either way. */
void pcfx_boot_progress_set_note(const char *note)
{
    unsigned i = 0;
    if (note)
        while (note[i] && i < DIAG_NOTE_MAX) {
            s_note[i] = note[i];
            i++;
        }
    s_note[i] = 0;
    if (s_active && s_io_valid)
        io_panel_draw();
}

static void progress_end(void)
{
    if (!s_active)
        return;
    s_ticks = BAR_W; s_asymptotic = 0;   /* force the fill computation to full */
    s_complete = 1;
    s_fill_w = 0;                        /* repaint the whole fill solid */
    for (unsigned y = BAR_Y0; y < BAR_Y1; y++)
        bar_hline(y, BAR_X0, BAR_X1, IDX_FILL);
    s_fill_w = BAR_W;
    status_draw();
    s_active = 0;
}

/* ---- BOOT: page 0 is displayed throughout boot; linear estimate. ---- */
void pcfx_boot_progress_begin(void)
{
    progress_begin(pcfx_fb_page_base(0), 0, "INITIALIZING");
}
void pcfx_boot_progress_end(void)   { progress_end(); }

/* ---- LEVEL LOAD: draw over the frozen on-screen frame; asymptotic fill. ---- */
void pcfx_load_progress_begin(void)
{
    progress_begin(pcfx_fb_display_base(), 1, "LEVEL DATA");
}
void pcfx_load_progress_end(void)   { progress_end(); }

/* ~0.3s class busy-wait; same pacing family as the CD retry delays in libpcfx. */
static void fatal_blink_delay(void)
{
    volatile uint32_t n = 0x300000u;
    while (n--) { }
}

/* Fatal-error indicator: I_Error's last act before it halts forever. It leaves a
 * persistent top panel containing the formatted error, last stage and last CD
 * request, while alternately filling/framing the load-bar geometry below.
 * Safe to call at any point in the program's life: it only writes the two indices
 * this file's header seeds before PLAYPAL loads / DOOM's PLAYPAL provides after,
 * and it re-reads the displayed page every cycle rather than assuming boot's
 * fixed page 0. Never returns. */
static void fatal_message_draw(const char *message)
{
    char line[33];
    unsigned line_len = 0;
    unsigned y = 17;
    unsigned lines = 0;

    while (message && *message && lines < 4u) {
        int ch = (unsigned char)*message++;
        if (ch == '\n' || line_len == 31u) {
            line[line_len] = 0;
            tiny_line(y, line, IDX_FILL, IDX_FRAME);
            y += 7u;
            lines++;
            line_len = 0;
            if (ch == '\n')
                continue;
        }
        line[line_len++] = (char)ch;
    }
    if (line_len && lines < 4u) {
        line[line_len] = 0;
        tiny_line(y, line, IDX_FILL, IDX_FRAME);
    }
}

static void fatal_panel_draw(const char *message)
{
    char line[32];
    unsigned n;

    panel_clear(DIAG_PANEL_Y0, FATAL_PANEL_Y1, IDX_FRAME);
    tiny_line(7, "FATAL ERROR", IDX_FILL, IDX_FRAME);
    fatal_message_draw(message);

    n = append_text(line, 0, sizeof(line), "STAGE ");
    append_text(line, n, sizeof(line), s_label[0] ? s_label : "UNKNOWN");
    tiny_line(47, line, IDX_FILL, IDX_FRAME);

    if (s_io_valid) {
        n = append_text(line, 0, sizeof(line), "CD ");
        n = append_text(line, n, sizeof(line), s_io_source);
        n = append_text(line, n, sizeof(line), " LBA ");
        n = append_uint(line, n, sizeof(line), s_io_lba);
        n = append_text(line, n, sizeof(line), " SEC ");
        append_uint(line, n, sizeof(line), s_io_sectors);
        tiny_line(54, line, IDX_FILL, IDX_FRAME);
    }

    if (s_note[0])
        tiny_line(61, s_note, IDX_FILL, IDX_FRAME);

    /* Build identity, always last: every photo of this panel must be
     * attributable to an exact doom-pcfx + libpcfx tree (two 07-24 burns
     * were misread because the disc didn't match the assumed source). */
    tiny_line(68, PCFX_BUILD_ID, IDX_FILL, IDX_FRAME);
}

void pcfx_fatal_blink(const char *message)
{
    s_base = pcfx_fb_display_base();
    fatal_panel_draw(message);

    for (;;) {
        s_base = pcfx_fb_display_base();
        for (unsigned y = BAR_Y0 - 1; y <= BAR_Y1; y++)
            bar_hline(y, BAR_X0 - 1, BAR_X1 + 1, IDX_FILL);
        fatal_blink_delay();

        s_base = pcfx_fb_display_base();
        for (unsigned y = BAR_Y0 - 1; y <= BAR_Y1; y++)
            bar_hline(y, BAR_X0 - 1, BAR_X1 + 1, IDX_FRAME);
        fatal_blink_delay();
    }
}

#ifdef DEV_CD_MATRIX
/* ------------------------------------------------------ DEV_CD_MATRIX only *
 * Services the CD-DMA matrix harness (platform/pcfx_cdmatrix.c) borrows from
 * this file, because it runs at the same moment as the boot bar and is bound
 * by the same two constraints:
 *
 *  - No PLAYPAL yet. king_video_init seeds exactly two non-black VCE entries
 *    (IDX_FRAME / IDX_FILL) and blacks out the other 254, so the VDC text
 *    layer that pcfx_text_* draws into is INVISIBLE at this point: every glyph
 *    pixel is a PLAYPAL index that is still Y=0. Only this file's 3x5 font on
 *    the KING framebuffer shows up. (The harness's first build drew through
 *    pcfx_text_* and produced a black screen for exactly this reason.)
 *  - CPU KRAM access races the BG fetch. Every burst has to sit inside the
 *    vertical blank and be interrupt-atomic or the words land garbled -- which,
 *    for a harness whose entire verdict is a CRC of KRAM, would manufacture
 *    failures out of nothing. bar_kram_guard is exported for the harness's own
 *    poison/CRC bursts.
 */
void pcfx_boot_dev_begin(void)
{
    s_base = pcfx_fb_display_base();
    panel_clear(0u, 240u, IDX_FRAME);
}

/* Left-aligned (tiny_line centres, which a column grid cannot use). x is a
 * KRAM word column, y a pixel row; characters outside the 3x5 font's repertoire
 * render blank. */
void pcfx_boot_dev_text(unsigned x, unsigned y, const char *text)
{
    for (; *text && x + STATUS_FONT_W <= KFB_ROW_WORDS;
         text++, x += STATUS_ADVANCE)
        tiny_putc(x, y, *text, IDX_FILL, IDX_FRAME);
}

void pcfx_boot_dev_kram_guard(unsigned words)
{
    bar_kram_guard(words);
}
#endif /* DEV_CD_MATRIX */
