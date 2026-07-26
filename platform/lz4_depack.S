/* lz4_depack.S -- LZ4 *block* decompressor for the NEC V810 (PC-FX), GAS syntax.
 *
 * Ported from Martin Wendt's lz4_v810_decode (lz4_depack.vbasm, 3-clause BSD),
 * adapted to the v810-gcc C ABI and corrected. Decodes CD-streamed assets
 * (backgrounds, later maps) that are too bulky to keep resident in 2 MB RAM.
 *
 * On-disc format (matches the reference): a 2-byte little-endian header giving
 * the compressed block length, then one raw LZ4 block (as produced by
 * `lz4 -12 -B4`, frame header/tail stripped). Blocks are <= 64 KB.
 *
 * C signature:  unsigned lz4_depack(const void *src, void *dst);
 *   src -> the 2-byte length header + the block; dst -> output buffer sized to
 *   the known uncompressed length. Returns the number of bytes written.
 *
 * Changes vs the reference asm:
 *   - C-callable: args arrive in r6/r7 (not r10/r11); result returned in r10.
 *   - Leaf function: the reference's fetch_length / copy_data subroutines are
 *     inlined, so there is no `jal`, no lp save, and no stack frame.
 *   - Correct termination: the reference always read an offset+match after the
 *     literals, which over-reads/over-writes on the standard LZ4 literals-only
 *     final sequence. We re-check `src >= send` right after the literal copy and
 *     stop there, so no bytes are written past the real output. All other
 *     registers used (r6..r16) are caller-saved on v810-gcc, so nothing is
 *     clobbered that must be preserved.
 *
 * NB: byte value-loads are LD.B + `andi 0xff` (zero-extend), NOT the reference's
 * IN.B. Two V810 subtleties bite here: (1) LD.B *sign-extends*, so a token/offset/
 * extension byte >= 0x80 becomes 0xFFFFFF80.., corrupting the `>> 4` literal
 * length, the 16-bit offset and the `== 0xff` extension test; (2) IN.B targets the
 * external/IO bus and reads 0 from main RAM on this hardware/emulator (a boot
 * self-test proved the reference's `in.b` returns 0 for our RAM source). LD.B is
 * the only load that reads RAM, so we mask off the sign extension by hand. Pure
 * copy-loads (r14 in the two copy loops) skip the mask: ST.B stores only the low
 * byte, so the sign extension is harmless there.
 *
 * Registers: r6=src (advances), r7=dst (advances), r8=dst start, r9=255 const,
 *   r10=scratch/return, r11=send (end of block), r12=token, r13=run length,
 *   r14/r15=scratch, r16=match source pointer.
 */
	.text
	.global	_lz4_depack

_lz4_depack:
	mov	r7, r8			/* r8 = dst start (for the returned length) */
	movea	0xff, r0, r9		/* r9 = 255, for the extension-byte compare */

	/* 2-byte little-endian compressed-block length header -> r11 = send */
	ld.b	0[r6], r11
	andi	0xff, r11, r11
	ld.b	1[r6], r10
	andi	0xff, r10, r10
	shl	8, r10
	or	r10, r11		/* r11 = clen */
	add	2, r6			/* skip header */
	add	r6, r11			/* r11 = send = one past last compressed byte */

1:	/* fetch_token */
	cmp	r11, r6
	bnl	5f			/* src >= send -> done */
	ld.b	0[r6], r12		/* token */
	andi	0xff, r12, r12
	add	1, r6

	/* --- literals: length = token >> 4 (15 => extend by 0xff bytes) --- */
	mov	r12, r13
	shr	4, r13
	be	3f			/* literal length 0 -> no literals */
	cmp	15, r13
	bne	2f			/* < 15 -> length is final */
6:	ld.b	0[r6], r14		/* literal-length extension */
	andi	0xff, r14, r14
	add	1, r6
	add	r14, r13
	cmp	r9, r14
	be	6b			/* 0xff -> another byte follows */
2:	ld.b	0[r6], r14		/* copy r13 literal bytes src -> dst (no mask: */
	st.b	r14, 0[r7]		/*   st.b keeps only the low byte)             */
	add	1, r6
	add	1, r7
	add	-1, r13
	bne	2b

3:	/* fetch_offset -- but stop first if the block ended on those literals */
	cmp	r11, r6
	bnl	5f			/* literals-only final sequence -> done */
	ld.b	0[r6], r14		/* 2-byte little-endian back-offset */
	andi	0xff, r14, r14
	ld.b	1[r6], r15
	andi	0xff, r15, r15
	shl	8, r15
	or	r15, r14		/* r14 = offset */
	add	2, r6
	mov	r7, r16
	sub	r14, r16		/* r16 = dst - offset = match source */

	/* --- match: length = (token & 15) + 4 (15 => extend by 0xff bytes) --- */
	andi	15, r12, r13
	cmp	15, r13
	bne	4f
7:	ld.b	0[r6], r14		/* match-length extension */
	andi	0xff, r14, r14
	add	1, r6
	add	r14, r13
	cmp	r9, r14
	be	7b
4:	add	4, r13			/* minimum match length is 4 */
8:	ld.b	0[r16], r14		/* copy r13 match bytes (byte-wise: handles */
	st.b	r14, 0[r7]		/*   overlap when offset < length, i.e. RLE)  */
	add	1, r16
	add	1, r7
	add	-1, r13
	bne	8b
	br	1b			/* next token */

5:	/* done: return bytes written = dst - dst_start */
	mov	r7, r10
	sub	r8, r10
	jmp	[lp]
