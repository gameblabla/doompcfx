/* CD-DMA matrix self-test — one burn answers the whole arm-shape question.
 *
 * Reads a known on-disc extent (from the CD IWAD blob at
 * BINARY_LBA_SRC_GENERATED_PCFX_IWAD_BIN) through every combination of:
 *
 *   path      KING SCSI->KRAM DMA / RAM DMA bounce / CPU-PIO->RAM / CPU-PIO->KRAM
 *   arm shape count-0 phase-driven (current production shape) vs count-bounded
 *             with check_dma polling (the reverted e4501be shape) vs the
 *             "retail" shape (count-bounded, INT-armed, REG.0B-D0-retired
 *             interior chunks with C6272_2 3.3.4 step 9B's Sequential-DMA
 *             REG.02 3->1->0 walk between them, and a phase-retired final
 *             chunk)
 *   chunking  526 B (Makeruna's retail burst), 2 KiB (sector), 32 KiB (default)
 *   size      16 KiB, which fits in ONE arm of any shape, vs 128 KiB, which is
 *             two bytes past REG.0A's documented ceiling (0x1FFFE) and so can
 *             only be read by a path that chunks correctly — the case every
 *             real asset load in this port hits
 *   region    KRAM word 0x08000 (fb1, low bit-17 half) vs 0x28000 (BG-cache
 *             slot, high half) — catches address/half-dependent engine bugs
 *
 * and compares each result against a CRC32 computed at BUILD time from the
 * very bytes the CD image contains. No read path can influence the reference,
 * so agreement means bit-exact delivery and a mismatch pinpoints exactly which
 * cell(s) lie.
 *
 * RESULT OF THE 2026-07-24 BURN -- read libpcfx/docs/CD_DMA_MATRIX_RESULTS.md
 * before touching this file again. Summary:
 *
 *   KC0 at LO passed bit-exact.  KRTL failed every valid way (silently
 *   transferring nothing as a single arm, wedging when chunked) and KBND
 *   wedged.  RDMA / RPIO / KPIO all passed.
 *
 *   The third reading below fired: KC0 passed at LO and transferred NOTHING at
 *   HI, so rows 4/5/9/10/11 measured an address problem, not an arm shape.
 *   Cause: this file hands flat word addresses to the SCSI-DMA engine, but
 *   REG.09 carries D17 = A/B bank, D16 = A16, D15..D0 = KA15..0 and no A17 --
 *   the engine's page comes from REG.0F SCP, which this harness never
 *   programs.  The CPU read-back cursor resolves the same number differently.
 *   ANY v2 of this harness must program the engine page explicitly and point
 *   the read-back cursor at the matching page before a HI row means anything.
 *
 *   New lead worth a v2: rows 2 and 12 run the byte-identical arm/retire/
 *   disarm sequence at the same chunk size and disagree.  Row 12 re-arms one
 *   fixed scratch address with a copy-out between chunks; row 2 advances the
 *   destination with no gap.  Isolate those two variables (address advance vs
 *   elapsed time), and also try arming REG.0A with size<<1 / size>>1 -- its
 *   figure labels D15..D1 as A14..A0, so the flat byte count row 3 wrote may
 *   not be the count the engine received.
 *
 *   NOTE the citations in this file are sound: DEVICE_EN's C6272_2.md is a
 *   CONDENSED translation whose 3.3.4 stops at six steps, but the full one at
 *   libpcfx/examples/034_king_rotate_bisect/docs/C6272_English_Markdown/
 *   C6272_2_English.md carries all nine verbatim.  The burn refutes the
 *   documented BEHAVIOUR, not the documentation.
 *
 * Original reading key, retained: the three K-prefixed arm shapes are the
 * whole experiment. If KRTL passes where KC0 and KBND fail, that procedure is
 * confirmed and eris_cd_read_kram_retail becomes the production path. If the
 * 128K rows fail where the 16K rows of the same shape pass, the defect is in
 * chunk continuation, not in arming. If a shape fails at HI but not LO, it is
 * an address decode problem and not an arm-shape problem at all.
 *
 * KRAM destinations are poisoned before the read (a stale-but-correct region
 * from an earlier cell must not pass), then CRC'd back out through the KING
 * read cursor.
 *
 * Two hardware constraints shape everything below; both were live defects in
 * this file's first draft and both would have produced meaningless results:
 *
 *  - Output goes through the KING framebuffer's boot-safe 3x5 font
 *    (pcfx_boot_dev_text), NOT pcfx_text_*. At this point in boot PLAYPAL has
 *    not loaded, so 254 of the 256 VCE entries are still Y=0 and anything the
 *    VDC text layer draws is black on black.
 *  - Every CPU KRAM burst (poison and CRC read-back) is raster-guarded and
 *    interrupt-atomic, exactly like the boot bar's own writes. Unguarded CPU
 *    access loses to the BG fetch on real silicon; in a harness whose entire
 *    verdict is a CRC of KRAM that manufactures failures out of nothing.
 *
 * Destination addresses avoid the DISPLAYED page (words 0x00000..0x07FFF, where
 * the grid itself lives) and the A16=1 halves, which only 4-Mbit mode populates
 * (see the KRAM map in pcfx_kram.h) — this harness must stay valid in EITHER
 * KRAM mode, since one of the things it can be pointed at is a build whose mode
 * programming is itself under suspicion, and a probe into the dotted expansion
 * half would fail for reasons unrelated to the arm shape under test. That leaves
 * 0x08000..0x0FFFF and 0x20000..0x2FFFF, and only the latter is large enough for
 * the 128K cell. Note the KING address field carries the A/B bank at D17 and has
 * no A17, so a single transfer must not cross 0x20000 either.
 *
 * The grid is redrawn after every cell, so even a wedged cell leaves a
 * photographable partial result. Never returns. */

#ifdef DEV_CD_MATRIX

#include <eris/cd.h>
#include <pcfx/king.h>

#include "pcfx_cdmatrix.h"
#include "pcfx.h"
#include "pcfx_kram.h"
#include "pcfx_boot.h"
#include "buildid.h"
#include "cdmatrix_ref.h"

#ifdef HAVE_GENERATED_LBAS
#include "lbas.h"
#else
#error "DEV_CD_MATRIX needs src/generated/lbas.h (run the CD link once first)"
#endif

#define MTX_LBA        BINARY_LBA_SRC_GENERATED_PCFX_IWAD_BIN
#define MTX_BYTES      (CDMATRIX_SECTORS * 2048u)      /*  16 KiB, one arm   */
#define MTX_BIG_BYTES  (CDMATRIX_BIG_SECTORS * 2048u)  /* 128 KiB, must chunk */

/* Mode-independent (A16=0), never the displayed page. See the header note. */
#define KRAM_LO    0x08000u   /* fb1 slot, low bit-17 half:  0x08000..0x0FFFF */
#define KRAM_HI    0x28000u   /* BG-cache slot, high half:   0x28000..0x2FFFF */
#define KRAM_BIG   0x20000u   /* fb2 slot; the only 0x10000-word contiguous run */

/* RAM-path buffer borrowed from the (unused) zone reserve; a dedicated
 * static array of MTX_BIG_BYTES overflows the PC-FX RAM link budget. */
unsigned char* Z_DevScratch(void);
#define mtx_rambuf (Z_DevScratch())

/* Words per guarded KRAM burst. Matches the boot bar's own conservative
 * words-per-blank-line budget, so a burst always fits the window the guard
 * granted. */
#define KRAM_BURST 64u

/* ---- CRC32 (zlib polynomial, reflected, nibble table) ------------------- */
static const unsigned long crc_tab[16] = {
    0x00000000ul, 0x1DB71064ul, 0x3B6E20C8ul, 0x26D930ACul,
    0x76DC4190ul, 0x6B6B51F4ul, 0x4DB26158ul, 0x5005713Cul,
    0xEDB88320ul, 0xF00F9344ul, 0xD6D6A3E8ul, 0xCB61B38Cul,
    0x9B64C2B0ul, 0x86D3D2D4ul, 0xA00AE278ul, 0xBDBDF21Cul
};

static unsigned long crc_byte(unsigned long c, unsigned char b)
{
    c ^= b;
    c = (c >> 4) ^ crc_tab[c & 15u];
    c = (c >> 4) ^ crc_tab[c & 15u];
    return c;
}

static unsigned long crc_ram(const unsigned char *p, unsigned long n)
{
    unsigned long c = 0xFFFFFFFFul;
    while (n--)
        c = crc_byte(c, *p++);
    return c ^ 0xFFFFFFFFul;
}

/* CRC of a KRAM extent, low byte first per word — the same byte order the
 * SCSI-DMA engine deposits and eris_cd_read_dma's copy-out assumes. Words are
 * pulled out in guarded, interrupt-atomic bursts and CRC'd OUTSIDE the window:
 * the guard's budget assumes a bare streaming loop, so folding the checksum
 * into it would overrun the blank the guard just granted. */
static unsigned long crc_kram(unsigned long word, unsigned long words)
{
    unsigned long c = 0xFFFFFFFFul;
    unsigned short buf[KRAM_BURST];

    while (words) {
        unsigned long n = (words < KRAM_BURST) ? words : KRAM_BURST;
        unsigned long i;
        uint32_t psw;

        pcfx_boot_dev_kram_guard((unsigned)n);
        psw = pcfx_irq_save();
        king_kram_set_read(word, 1);
        for (i = 0; i < n; i++)
            buf[i] = read_kram();
        pcfx_irq_restore(psw);

        for (i = 0; i < n; i++) {
            c = crc_byte(c, (unsigned char)buf[i]);
            c = crc_byte(c, (unsigned char)(buf[i] >> 8));
        }
        word  += n;
        words -= n;
    }
    return c ^ 0xFFFFFFFFul;
}

static void poison_kram(unsigned long word, unsigned long words)
{
    while (words) {
        unsigned long n = (words < KRAM_BURST) ? words : KRAM_BURST;
        unsigned long i;
        uint32_t psw;

        pcfx_boot_dev_kram_guard((unsigned)n);
        psw = pcfx_irq_save();
        king_kram_set_cursor(word, 1);
        for (i = 0; i < n; i++)
            write_kram(0x5A5Au);
        pcfx_irq_restore(psw);

        word  += n;
        words -= n;
    }
}

/* ---- line formatting (the 3x5 font is uppercase + a few punctuation) ---- */
#define LINE_MAX 31

static unsigned put_str(char *dst, unsigned n, const char *s)
{
    while (*s && n < LINE_MAX)
        dst[n++] = *s++;
    dst[n] = 0;
    return n;
}

static unsigned put_uint(char *dst, unsigned n, unsigned long v, unsigned pad)
{
    char rev[12];
    unsigned nr = 0;
    do { rev[nr++] = (char)('0' + (unsigned)(v % 10ul)); v /= 10ul; } while (v && nr < sizeof rev);
    while (nr < pad && nr < sizeof rev)
        rev[nr++] = ' ';
    while (nr && n < LINE_MAX)
        dst[n++] = rev[--nr];
    dst[n] = 0;
    return n;
}

static unsigned put_hex32(char *dst, unsigned n, unsigned long v)
{
    static const char h[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < 8 && n < LINE_MAX; i++)
        dst[n++] = h[(v >> (28 - 4 * i)) & 15ul];
    dst[n] = 0;
    return n;
}

/* ---- the matrix --------------------------------------------------------- */
enum { K_C0, K_BND, K_RTL, R_DMA, R_PIO, K_PIO };

struct mtx_cell {
    const char   *name;      /* 11 cols max */
    int           kind;
    unsigned long arg;       /* chunk bytes (K_BND/K_RTL) / scratch words (R_DMA) */
    unsigned long kram;      /* destination word, KRAM kinds only */
    int           big;       /* 0 = 16 KiB extent, 1 = 128 KiB extent */
};

/* Ordered so a burn that wedges partway still answers the most valuable
 * question first: the three arm shapes at the same size and address, then the
 * chunk-size sweep, then the size only a correct chunker survives, then the
 * non-DMA controls that establish the disc itself is readable. */
static const struct mtx_cell cells[] = {
    /* --- the count-0 questions, FIRST, so each runs from as clean a bus as
     * the harness can give it.  Burn 2 left exactly one thing unexplained:
     * count-0 passed at LO (2/2) and at MID/128K (1/2) but failed at HI (0/2),
     * and HI always ran straight after a silently-failing counted cell.  Rows
     * 1-4 now answer that with the counted cells out of the way. */
    { "KC0 -16K LO", K_C0,  0,    KRAM_LO,  0 },  /* the known-good baseline  */
    { "KC0 -16K HI", K_C0,  0,    KRAM_HI,  0 },  /* 0/2 before; clean now    */
    { "KC0 128 MID", K_C0,  0,    KRAM_BIG, 1 },  /* 128K bit-exact in burn 2 */
    { "KC0 -16K LO", K_C0,  0,    KRAM_LO,  0 },  /* repeat: did HI poison it?*/

    /* --- the rebuilt production RAM path (count-0 per piece, one READ(10)
     * each) at two window sizes: 1 sector (what doom passes today) and 8. */
    { "RDMA-1sec",   R_DMA, 1024, KRAM_CD_DMA_SCRATCH_WORD, 0 }, /* doom's own */
    { "RDMA-8sec",   R_DMA, 8192, KRAM_BIG, 0 },  /* 16 KiB window: 1 command */

    /* --- the non-DMA controls: 4/4 across both burns, so a failure here means
     * the disc or drive is bad and nothing else in the grid is readable. */
    { "RPIO",        R_PIO, 0,    0,        0 },
    { "KPIO     LO", K_PIO, 0,    KRAM_LO,  0 },

    /* --- the refuted counted shapes, LAST.  They went 1-for-18 across two
     * burns; they are kept only to confirm the verdict reproduces, and they
     * are ordered after everything that matters so their wedges cannot
     * contaminate a cell anyone is relying on. */
    { "KRTL-32K LO", K_RTL, 0,    KRAM_LO,  0 },  /* 16K < 32K: ONE counted arm,
                                                   * the sharp pair with row 1 */
    { "KBND-2K  LO", K_BND, 2048, KRAM_LO,  0 },
    { "KRTL-2K  LO", K_RTL, 2048, KRAM_LO,  0 },  /* forces 8 seq chunks      */
    { "KRTL-526 LO", K_RTL, 526,  KRAM_LO,  0 },  /* Makeruna's burst size    */
    { "KBND-526 LO", K_BND, 526,  KRAM_LO,  0 },
    { "KRTL128 M2K", K_RTL, 2048, KRAM_BIG, 1 },  /* 128K: 64 arms            */
};
#define NCELLS ((int)(sizeof cells / sizeof cells[0]))

/* Pixel geometry of the grid on the 128-word x 240-line KING page. */
#define GRID_X      2u    /* KRAM word column of the first glyph  */
#define GRID_Y0     40u   /* first cell row                        */
#define GRID_DY     7u    /* 5-px glyph + 2 px of leading          */

static char  s_result[NCELLS][LINE_MAX + 1];
static char  s_line[LINE_MAX + 1];

static void draw_cell(int i)
{
    unsigned n = 0;
    n = put_uint(s_line, n, (unsigned long)(i + 1), 2);
    n = put_str(s_line, n, " ");
    n = put_str(s_line, n, cells[i].name);
    while (n < 15u && n < LINE_MAX)
        s_line[n++] = ' ';
    s_line[n] = 0;
    n = put_str(s_line, n, s_result[i]);
    pcfx_boot_dev_text(GRID_X, GRID_Y0 + (unsigned)i * GRID_DY, s_line);
}

void pcfx_cdmatrix_run(void)
{
    int i;
    unsigned n;

    pcfx_boot_dev_begin();

    pcfx_boot_dev_text(GRID_X, 4u,  "CD-DMA MATRIX");
    pcfx_boot_dev_text(GRID_X, 11u, PCFX_BUILD_ID);

    n = put_str(s_line, 0, "S");
    n = put_hex32(s_line, n, (unsigned long)CDMATRIX_REF_CRC);
    n = put_str(s_line, n, " B");
    n = put_hex32(s_line, n, (unsigned long)CDMATRIX_BIG_CRC);
    pcfx_boot_dev_text(GRID_X, 18u, s_line);

    n = put_str(s_line, 0, "LBA ");
    n = put_uint(s_line, n, (unsigned long)MTX_LBA, 0);
    pcfx_boot_dev_text(GRID_X, 25u, s_line);

    for (i = 0; i < NCELLS; i++) {
        put_str(s_result[i], 0, "-");
        draw_cell(i);
    }

    /* A cell probing a mode that simply doesn't work on this silicon should
     * fail fast, not burn the full marginal-media retry budget. */
    eris_cd_set_attempts(2);

    for (i = 0; i < NCELLS; i++) {
        const struct mtx_cell *c = &cells[i];
        int ok = 0;
        unsigned long bytes = c->big ? MTX_BIG_BYTES : MTX_BYTES;
        unsigned long want  = c->big ? (unsigned long)CDMATRIX_BIG_CRC
                                     : (unsigned long)CDMATRIX_REF_CRC;
        unsigned long crc, k;

        put_str(s_result[i], 0, "..");
        draw_cell(i);                 /* wedge here -> photo still says where */

        /* Cells MUST be independent.  Burns 1 and 2 disagreed on rows 10/11/12
         * with identical machine code, and every disagreeing row sits directly
         * downstream of a cell that failed silently or wedged -- so "the
         * previous cell left the target mid-phase" is a live alternative to
         * every per-cell conclusion.  Drain to BUS FREE first. */
        eris_cd_bus_settle();

        if (c->kind == R_DMA || c->kind == R_PIO) {
            for (k = 0; k < bytes; k++)
                mtx_rambuf[k] = 0x5A;
            /* R_DMA's `kram` is a scratch window, not a destination: poison it
             * too, so a piece that silently transfers nothing shows up as
             * poison in the copy-out rather than as the previous piece. */
            if (c->kind == R_DMA)
                poison_kram(c->kram, c->arg);
        } else {
            poison_kram(c->kram, bytes / 2u);
        }

        switch (c->kind) {
        case K_C0:  ok = eris_cd_read_kram(MTX_LBA, c->kram, bytes); break;
        case K_BND: ok = eris_cd_read_kram_bounded(MTX_LBA, c->kram, bytes,
                                                   c->arg); break;
        case K_RTL: ok = eris_cd_read_kram_retail(MTX_LBA, c->kram, bytes,
                                                  c->arg); break;
        /* R_DMA reuses `kram` as the scratch WINDOW BASE (it has no KRAM
         * destination of its own -- the data lands in mtx_rambuf), so a cell
         * can test a window bigger than doom's 2 KiB framebuffer dead tail. */
        case R_DMA: ok = eris_cd_read_dma(MTX_LBA, mtx_rambuf, bytes,
                                          c->kram, c->arg) != 0; break;
        case R_PIO: ok = eris_cd_read(MTX_LBA, mtx_rambuf, bytes) != 0; break;
        case K_PIO: ok = eris_cd_read_kram_pio(MTX_LBA, c->kram, 1,
                                               bytes); break;
        }

        if (!ok) {
            int ph = 0, st = 0;
            eris_cd_get_diag(0, &ph, &st, 0);
            n = put_str(s_result[i], 0, "RD P");
            n = put_uint(s_result[i], n, (unsigned long)ph, 0);
            n = put_str(s_result[i], n, " S");
            if (st < 0) {
                n = put_str(s_result[i], n, "-");
                st = -st;
            }
            put_uint(s_result[i], n, (unsigned long)st, 0);
        } else {
            crc = (c->kind == R_DMA || c->kind == R_PIO)
                    ? crc_ram(mtx_rambuf, bytes)
                    : crc_kram(c->kram, bytes / 2u);
            if (crc == want) {
                put_str(s_result[i], 0, "OK");
            } else {
                n = put_str(s_result[i], 0, "CRC ");
                put_hex32(s_result[i], n, crc);
            }
        }
        draw_cell(i);
    }

    pcfx_boot_dev_text(GRID_X, GRID_Y0 + (unsigned)NCELLS * GRID_DY + GRID_DY,
                       "DONE - SAFE TO POWER OFF");

    for (;;) { }
}

#endif /* DEV_CD_MATRIX */
