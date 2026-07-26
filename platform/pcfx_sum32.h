/* pcfx_sum32.h -- runtime side of the CD asset integrity check.
 *
 * An imperfect burn can hand the drive data that reads with GOOD SCSI status and is
 * still wrong (the reported symptom: garbled enemy sprites on real hardware). The
 * build stamps a 32-bit checksum on every CD-streamed region and writes each region
 * TWICE; w_wad.c verifies what it read here and re-reads the duplicate copy on a
 * mismatch. Bit-identical to sum32() in tools/pcfx_sum32.py.
 *
 * WORD-wise on purpose: a level load verifies ~500 KB and the V810 pays per access —
 * this is ~5 ops per 4 bytes (~40 ms a load), where a table-driven byte CRC32 costs
 * ~10x that. Burn corruption is gross (dropped/repeated sectors, open-bus fill), and
 * this catches any single differing word plus any length change (the length seeds it). */
#ifndef PCFX_SUM32_H
#define PCFX_SUM32_H

/* Checksum `n` bytes at `p`. One out-of-line copy (platform/pcfx_sum32.c): it is
 * called from a dozen places in w_wad.c and inlining it everywhere cost more program
 * image than this console has to spare. */
unsigned pcfx_sum32(const void *p, unsigned n);

#endif
