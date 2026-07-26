/* pcfx_mappack.h -- CD read primitive for the per-map contiguous asset packs.
 *
 * pcfx_mappacks.bin (built by tools/gen_pcfx_packs.py, cdlink `append`ed) holds, per
 * map, every compressed lump that map precaches, laid out GAP-FREE and contiguous so
 * the runtime streams the whole set into the arena in back-to-back sequential reads
 * (each abuts the last -> ~0 CD seek). This owns the blob's base LBA (emitted by the
 * CD linker into lbas.h, exactly like the IWAD blob and the RAINBOW sky). */
#ifndef PCFX_MAPPACK_H
#define PCFX_MAPPACK_H

/* Read `bytes` (rounded up to whole 2048-byte sectors by the CD hardware) from the
 * map-pack blob, starting `sector_off` sectors into it, into `buf`. */
void pcfx_mappack_read(unsigned sector_off, void *buf, unsigned bytes);

#endif
