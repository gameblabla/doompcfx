/* lz4_depack.h -- LZ4 *block* decompressor for CD-streamed assets (V810 asm).
 *
 * The PC-FX has no room to keep bulk assets resident, so backgrounds (and later
 * maps) live on the CD compressed and are decoded into a transient buffer when
 * needed (see docs/cd-streaming-plan.md). The on-disc format matches the
 * lz4_v810_decode reference: a 2-byte little-endian header giving the compressed
 * block length, followed by one raw LZ4 block (as produced by `lz4 -12 -B4`, the
 * frame header/tail stripped). Blocks are <= 64 KB.
 *
 * The decoder is hand-written V810 assembly (platform/lz4_depack.S) — a leaf
 * function, no stack frame. Decode happens only at level-load / screen-change,
 * never per-frame. */
#ifndef LZ4_DEPACK_H
#define LZ4_DEPACK_H

/* Decode the length-prefixed LZ4 block at `src` into `dst`. `dst` must be large
 * enough for the known uncompressed size (the decoder writes exactly that many
 * bytes for a well-formed stream). Returns the number of bytes written. */
unsigned lz4_depack(const void *src, void *dst);

#endif
