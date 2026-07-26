/* pcfx_wad.h -- CD read primitive for the streamed IWAD.
 *
 * The full doom1.wad (4 MB) cannot be resident in 2 MB RAM, so it lives on the CD
 * as a sector-aligned blob (tools/bake_wad.py --cd-wad, cdlink `append`ed). w_wad.c
 * keeps only the directory resident and streams lump data on demand through this
 * primitive, which owns the blob's base LBA (emitted by the CD linker into lbas.h,
 * exactly like the RAINBOW sky). */
#ifndef PCFX_WAD_H
#define PCFX_WAD_H

/* Read `bytes` (rounded up to whole 2048-byte sectors by the CD hardware) from the
 * IWAD blob, starting `sector_off` sectors into it, into `buf`. `buf` must be large
 * enough for the sector-rounded size. */
void pcfx_wad_read(unsigned sector_off, void *buf, unsigned bytes);

/* Shared CD -> RAM primitive with the automatic (and sticky) KING-DMA -> CPU-PIO
 * real-hardware fallback; I_Errors with libpcfx's failure diagnostics if both
 * modes fail. `what` names the requester in the fatal message ("WAD", "MAP PACK"). */
void pcfx_cd_read_ram(const char *what, unsigned lba, void *buf, unsigned bytes);

/* The loader starts in safe PIO for the self-validating IWAD header.  Once that
 * establishes the checksum tables, enable the fast DMA bounce.  Any checksum
 * rejection calls force_pio(), which is sticky for the rest of the run. */
void pcfx_cd_ram_enable_fast(void);
void pcfx_cd_ram_force_pio(void);

/* "DMA" / "PIO" — which CD->RAM mode the fallback has settled on. */
const char *pcfx_cd_ram_mode_name(void);

#endif
