/* pcfx_wad.c -- CD read primitive for the streamed IWAD (see pcfx_wad.h).
 *
 * Owns the IWAD blob's base LBA, emitted by the CD linker into lbas.h (the same
 * mechanism as the RAINBOW sky). A placeholder keeps code size stable before the
 * first link pass generates the real value; the `cd` target then recompiles this
 * file with -DHAVE_GENERATED_LBAS so the real LBA is baked in. */
#include "pcfx_wad.h"
#include "pcfx_boot.h"   /* boot loading bar: tick once per CD read */
#include "pcfx_kram.h"
#include "lprintf.h"
#include <eris/cd.h>
#include <eris/cdda.h>   /* CD-DA music: a data read stops the drive's audio */

#ifdef HAVE_GENERATED_LBAS
#include "lbas.h"
#endif
#ifndef BINARY_LBA_SRC_GENERATED_PCFX_IWAD_BIN
#define BINARY_LBA_SRC_GENERATED_PCFX_IWAD_BIN 900u
#endif
#define IWAD_BASE_LBA (BINARY_LBA_SRC_GENERATED_PCFX_IWAD_BIN)

/* Diagnostic counters: one SCSI READ(10) command per call, and the sectors it
 * moved. Each command that doesn't start where the last read ended costs a CD SEEK
 * in pcfxemu (33 ms min, 150–283 ms for a jump), so the command count is the seek
 * proxy the level-load coalescing (w_wad.c W_PrecacheEnd) drives down. */
unsigned g_cd_read_cmds = 0;
unsigned g_cd_read_sectors = 0;
unsigned g_cd_seek_ms = 0;        /* estimated seek time (mirrors pcfxemu's model)     */
static unsigned s_head_sector = 0xffffffffu;   /* sector just past the last read        */

/* Estimate one seek's cost the way pcfxemu's seektime_pcfx.c does: an ABUTTING read
 * (starts where the last ended) is free; anything else pays a rising penalty with the
 * jump distance. Coarse tiers are enough to compare load strategies (~1 frame=16.7ms). */
static unsigned est_seek_ms(unsigned from_sector, unsigned to_sector)
{
    if (from_sector == 0xffffffffu) return 0;                 /* first read: no prior head */
    unsigned d = (to_sector > from_sector) ? (to_sector - from_sector)
                                           : (from_sector - to_sector);
    if (d <= 1)   return 0;                                   /* abutting: free            */
    if (d <= 3)   return 33;                                  /* 2 frames                  */
    if (d < 7)    return 150;                                 /* 9 frames                  */
    if (d < 400)  return 283;                                 /* ~17 frames (short travel) */
    return 350;                                               /* long travel               */
}

/* Charge one read against the diagnostics (shared by the IWAD and map-pack readers so
 * their seek/transfer costs sum into the W_Load metric). `lba` is absolute. */
void pcfx_cd_account(unsigned lba, unsigned nsect)
{
    g_cd_read_cmds++;
    g_cd_read_sectors += nsect;
    g_cd_seek_ms += est_seek_ms(s_head_sector, lba);
    s_head_sector = lba + nsect;
    /* One CD read = one progress step. Ticks the active load bar (boot OR level
     * load); no-op when neither is active (see pcfx_boot.c). Placed here — not in
     * pcfx_wad_read — so map-pack reads (pcfx_mappack_read -> pcfx_cd_account) also
     * advance the level-load bar. */
    pcfx_boot_progress_tick();
    /* A data read stops the drive's CD-DA engine, so tell the music manager the
     * score needs restarting once the load settles (I_StartFrame pumps it).
     * Same reasoning as the progress tick: here, not in pcfx_wad_read, so
     * map-pack reads are counted too. */
    eris_cdda_notify_cd_read();
}

/* ---- CD -> RAM: CPU-PIO by default, DMA only as a last resort --------------
 *
 * CD -> system RAM is the correctness-critical path: it carries the WAD header,
 * the lump directory and the map-pack index, and a single wrong word there does
 * not look like a read failure, it looks like a corrupt game
 * ("W_GetNumForName: TEXTURE1 not found").
 *
 * There is no DMA engine to system RAM, so the "fast" path is a bounce: SCSI
 * DMA into a KRAM scratch window, then a CPU copy-out of that window. That
 * makes it strictly WORSE than plain PIO on this console, on two counts.
 *
 *   1. The DMA half is the count-0 arm, which two hardware burns put at 3 of 6.
 *   2. The copy-out half is a tight CPU loop on the KRAM data port, and
 *      C6272_2 3.2.2 ranks the CPU LAST on the K-BUS -- so it can lose words
 *      even when the DMA was perfect.
 *
 * eris_cd_read() has neither half: the CPU pumps the SCSI data register
 * straight into RAM and never touches KRAM at all. It is the only path with an
 * unbroken record on this console (4 of 4 across both burns) and the one that
 * has always progressed deep into boot.
 *
 * WHY THIS IS NOT A PROBE ANY MORE. It used to be: read the first request
 * through BOTH paths, compare checksums, and make the winner sticky. That is
 * what regressed the 2026-07-24 22:00 burn. Rebuilding eris_cd_read_dma on
 * count-0 arms made DMA good enough to WIN the one-shot comparison, after
 * which it stuck for the whole run and silently corrupted a later read -- the
 * fatal TEXTURE1 photo, taken with "CD MODE DMA" on the panel. One agreement
 * is not evidence about a path that works most of the time; a 50/50 path that
 * passes a single trial and is then trusted forever is worse than no DMA.
 *
 * So DMA is now only the LAST-DITCH swap if PIO itself fails, and libpcfx
 * verifies every DMA destination before reporting success (see
 * eris_cd_read_kram). EXTRA=-DPCFX_CD_TRY_DMA restores the probe for anyone
 * measuring the DMA path on hardware. */
#define CD_MODE_UNDECIDED 0
#define CD_MODE_DMA       1
#define CD_MODE_PIO       2
#ifdef PCFX_CD_TRY_DMA
/* Debug knob: probe DMA against PIO on the first read and make the winner
 * sticky, the pre-22:00 behaviour. For measurement only -- see above for why
 * this is not what ships. */
static int s_cd_ram_mode = CD_MODE_UNDECIDED;
#else
static int s_cd_ram_mode = CD_MODE_PIO;
#endif

/* Attempt budget for a mode PROBE (fast verdict) vs an established mode. */
#define CD_PROBE_ATTEMPTS 4
#define CD_FULL_ATTEMPTS  40

static int cd_ram_try_dma(unsigned lba, void *buf, unsigned bytes)
{
    /* Route SCSI (and the CPU KRAM read port) to page 0 for the bounce.
     * Interrupt-atomic: this is a KING select+data sequence issued with the
     * timer IRQ live (libpcfx pauses the timer only INSIDE eris_cd_*). */
    uint32_t psw = pcfx_irq_save();
    king_set_kram_pages(0, 0, 1, 1);
    pcfx_irq_restore(psw);
    return eris_cd_read_dma(lba, (unsigned char *)buf, bytes,
                            KRAM_CD_DMA_SCRATCH_WORD, KRAM_CD_DMA_SCRATCH_WORDS);
}

static int cd_ram_try_pio(unsigned lba, void *buf, unsigned bytes)
{
    return eris_cd_read(lba, (unsigned char *)buf, bytes) != 0;
}

const char *pcfx_cd_ram_mode_name(void)
{
    if (s_cd_ram_mode == CD_MODE_DMA) return "DMA";
    if (s_cd_ram_mode == CD_MODE_PIO) return "PIO";
    return "PROBING";
}

/* Order-independent byte checksum (FNV-1a) for the probe's DMA-vs-PIO data
 * comparison below. */
static unsigned cd_ram_sum(const unsigned char *p, unsigned n)
{
    unsigned h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

void pcfx_cd_read_ram(const char *what, unsigned lba, void *buf, unsigned bytes)
{
    unsigned nsect = (bytes + 2047u) / 2048u;
    int ok;

    if (s_cd_ram_mode == CD_MODE_PIO) {
        ok = cd_ram_try_pio(lba, buf, bytes);
        if (!ok)
            ok = cd_ram_try_dma(lba, buf, bytes);   /* last-ditch mode swap */
    } else if (s_cd_ram_mode == CD_MODE_UNDECIDED) {
        /* PROBE: decide the mode on this machine. SCSI status alone is NOT
         * enough — the 2026-07-24 hardware burn returned GOOD status from the
         * DMA path while the KRAM bounce delivered a constant open-bus
         * pattern to RAM ("W_INIT: CD IWAD ID MISSING (GOT UU..)"), so the
         * status-only probe locked in a corrupt mode. Now the probe reads the
         * same sectors through BOTH paths and compares checksums: only
         * DMA-with-PIO-agreement selects DMA. On any disagreement (or DMA
         * failure) the PIO copy — CPU-direct from the SCSI data register, no
         * KRAM involved — is what's left in the buffer and PIO becomes the
         * sticky mode. Costs one duplicate read of the first request only. */
        unsigned dma_sum = 0;
        int dma_ok;
        eris_cd_set_attempts(CD_PROBE_ATTEMPTS);
        dma_ok = cd_ram_try_dma(lba, buf, bytes);
        eris_cd_set_attempts(CD_FULL_ATTEMPTS);
        if (dma_ok)
            dma_sum = cd_ram_sum((const unsigned char *)buf, bytes);

        ok = cd_ram_try_pio(lba, buf, bytes);
        if (ok && dma_ok &&
            dma_sum == cd_ram_sum((const unsigned char *)buf, bytes)) {
            s_cd_ram_mode = CD_MODE_DMA;   /* both agree: DMA is fast + valid */
            pcfx_boot_progress_set_note("CD MODE DMA");
        } else if (ok) {
            /* PIO data stands (DMA failed or disagreed) — PIO is the mode. */
            s_cd_ram_mode = CD_MODE_PIO;
            pcfx_boot_progress_set_note(dma_ok ? "CD DMA BAD DATA"
                                               : "CD MODE PIO");
        } else if (dma_ok) {
            /* PIO itself failed on this machine; DMA (unverified) is all we
             * have. Re-fetch so the buffer holds DMA data, and say so. */
            ok = cd_ram_try_dma(lba, buf, bytes);
            if (ok) {
                s_cd_ram_mode = CD_MODE_DMA;
                pcfx_boot_progress_set_note("CD PIO FAILED");
            }
        }
    } else {
        ok = cd_ram_try_dma(lba, buf, bytes);
        if (!ok) {
            ok = cd_ram_try_pio(lba, buf, bytes);
            if (ok) {
                s_cd_ram_mode = CD_MODE_PIO;        /* sticky fallback */
                pcfx_boot_progress_set_note("CD MODE PIO");
            }
        }
    }

    if (!ok) {
        int dpath, dphase, dstatus, dtries;
        eris_cd_get_diag(&dpath, &dphase, &dstatus, &dtries);
        I_Error("CD %s read failed: LBA %u, %u sectors"
                " (path %d phase %d st %d try %d)",
                what, lba, nsect, dpath, dphase, dstatus, dtries);
    }
}

void pcfx_wad_read(unsigned sector_off, void *buf, unsigned bytes)
{
    unsigned nsect = (bytes + 2047u) / 2048u;
    unsigned lba   = IWAD_BASE_LBA + sector_off;
    pcfx_boot_progress_set_io("WAD", lba, nsect);
    pcfx_cd_account(lba, nsect);
    /* All callers provide sector-rounded storage. */
    pcfx_cd_read_ram("WAD", lba, buf, bytes);
}
