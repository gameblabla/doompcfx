/* pcfx_mappack.c -- CD read primitive for the per-map asset packs (see pcfx_mappack.h).
 *
 * Owns pcfx_mappacks.bin's base LBA, emitted by the CD linker into lbas.h (same
 * mechanism as the IWAD blob). A placeholder keeps code size stable before the first
 * link pass generates the real value; the `cd` target recompiles this file with
 * -DHAVE_GENERATED_LBAS so the real LBA is baked in. */
#include "pcfx_mappack.h"
#include "pcfx_boot.h"
#include "pcfx_wad.h"    /* pcfx_cd_read_ram: shared DMA->PIO fallback path */

void pcfx_cd_account(unsigned lba, unsigned nsect);

#ifdef HAVE_GENERATED_LBAS
#include "lbas.h"
#endif
#ifndef BINARY_LBA_SRC_GENERATED_PCFX_MAPPACKS_BIN
#define BINARY_LBA_SRC_GENERATED_PCFX_MAPPACKS_BIN 1400u
#endif
#define MAPPACK_BASE_LBA (BINARY_LBA_SRC_GENERATED_PCFX_MAPPACKS_BIN)

void pcfx_mappack_read(unsigned sector_off, void *buf, unsigned bytes)
{
    unsigned lba = MAPPACK_BASE_LBA + sector_off;
    unsigned nsect = (bytes + 2047u) / 2048u;
    pcfx_boot_progress_set_io("MAP PACK", lba, nsect);
    pcfx_cd_account(lba, nsect);
    pcfx_cd_read_ram("MAP PACK", lba, buf, bytes);
}
