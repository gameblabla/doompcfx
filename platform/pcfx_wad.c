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

/* ---- CD -> RAM: verified large-window DMA, sticky CPU-PIO fallback --------
 *
 * CD -> system RAM is the correctness-critical path: it carries the WAD header,
 * the lump directory and the map-pack index, and a single wrong word there does
 * not look like a read failure, it looks like a corrupt game
 * ("W_GetNumForName: TEXTURE1 not found").
 *
 * There is no DMA engine straight to system RAM, so the fast path bounces
 * through the otherwise-unused 256 KiB page-1/bank-B KRAM run.  Hudson
 * C6272_2 3.3.1 rates the asynchronous SCSI path at 1.5 MB/s; 3.2.2 says a
 * lower-priority CPU KRAM request is HELD until it wins arbitration, not lost.
 * The old 2 KiB bounce squandered that path by issuing one READ(10) per sector.
 *
 * DMA is still not trusted blindly.  W_Init reads the self-checking header by
 * the silicon-proven CPU-PIO path, and only then enables DMA.  Every generated
 * WAD/map-pack region is checksum-verified by w_wad.c; the first mismatch
 * calls pcfx_cd_ram_force_pio(), repairs through PIO/duplicate media, and
 * permanently leaves the run on the safe path.  This avoids the old one-shot
 * probe failure: DMA must keep agreeing with build-time truth on EVERY read.
 *
 * The full DMA call is interrupt-atomic.  Its copy-out owns KING's KRAM read
 * cursor, and a timer IRQ between register select/data accesses is a
 * silicon-observed corruption source.  Loading is already blocking, and the
 * PIO implementation likewise pauses the timer for its command. */
#define CD_MODE_DMA       1
#define CD_MODE_PIO       2
static int s_cd_ram_mode = CD_MODE_PIO;
static int s_cd_fast_enabled;
static int s_cd_dma_rejected;

static int cd_ram_try_dma(unsigned lba, void *buf, unsigned bytes)
{
    int ok;
    /* Route SCSI to page 1, where bank B is the dedicated 256 KiB bounce.
     * Keep the complete DMA + CPU copy-out atomic: both use shared KING
     * register/cursor state which the 1 ms timer ISR must not interrupt. */
    uint32_t psw = pcfx_irq_save();
    king_set_kram_pages(1, 0, 1, 1);
    ok = eris_cd_read_dma(lba, (unsigned char *)buf, bytes,
                          KRAM_CD_RAM_SCRATCH_WORD,
                          KRAM_CD_RAM_SCRATCH_WORDS);
    pcfx_irq_restore(psw);
    return ok;
}

static int cd_ram_try_pio(unsigned lba, void *buf, unsigned bytes)
{
    return eris_cd_read(lba, (unsigned char *)buf, bytes) != 0;
}

const char *pcfx_cd_ram_mode_name(void)
{
    if (s_cd_ram_mode == CD_MODE_DMA) return "DMA";
    return "PIO";
}

void pcfx_cd_ram_enable_fast(void)
{
#ifndef PCFX_CD_FORCE_PIO
    s_cd_fast_enabled = 1;
    if (!s_cd_dma_rejected)
    {
        s_cd_ram_mode = CD_MODE_DMA;
        pcfx_boot_progress_set_note("CD MODE DMA VERIFY");
    }
#endif
}

void pcfx_cd_ram_force_pio(void)
{
    if (s_cd_ram_mode == CD_MODE_DMA)
        pcfx_boot_progress_set_note("CD DMA BAD - PIO");
    s_cd_dma_rejected = 1;
    s_cd_ram_mode = CD_MODE_PIO;
}

void pcfx_cd_read_ram(const char *what, unsigned lba, void *buf, unsigned bytes)
{
    unsigned nsect = (bytes + 2047u) / 2048u;
    int ok;

    if (s_cd_ram_mode == CD_MODE_DMA) {
        ok = cd_ram_try_dma(lba, buf, bytes);
        if (!ok) {
            pcfx_cd_ram_force_pio();
            ok = cd_ram_try_pio(lba, buf, bytes);
        }
    } else {
        ok = cd_ram_try_pio(lba, buf, bytes);
        /* Last ditch only: if the safe path cannot read at all, DMA is better
         * than an immediate fatal error.  It remains rejected/sticky-PIO for
         * subsequent calls and its caller still checksum-validates the data. */
        if (!ok && s_cd_fast_enabled)
            ok = cd_ram_try_dma(lba, buf, bytes);
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
