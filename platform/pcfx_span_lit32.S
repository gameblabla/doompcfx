/* Dense 32x64 lit-flat span kernel for the NEC V810.
 *
 * C signature:
 *   void pcfx_span_lit32(unsigned count, unsigned position,
 *                        unsigned step, const unsigned char *lit);
 *
 * The C wrapper has already programmed KING's KRAM write cursor.  This leaf
 * routine preserves Doom's packed-coordinate arithmetic exactly and writes one
 * doubled 8-bit palette index per logical pixel directly to KRAM.  Registers
 * r6..r14 are caller-saved in the v810-gcc ABI, so no stack frame is needed.
 */
	.section .pcfx_spanhot,"ax"
	.global	_pcfx_span_lit32
	.type	_pcfx_span_lit32, @function

_pcfx_span_lit32:
	cmp	0, r6
	be	9f
	cmp	4, r6
	bl	2f

1:	/* Form four source addresses first.  The V810 interlocks dependent
	 * operations after an external-RAM load; keeping four addresses live lets
	 * the loads issue together and separates each load from its first use. */
	mov	r7, r10
	mov	r7, r14
	shr	5, r10
	shr	27, r14
	andi	0x07e0, r10, r10
	or	r14, r10
	add	r9, r10
	add	r8, r7

	mov	r7, r11
	mov	r7, r14
	shr	5, r11
	shr	27, r14
	andi	0x07e0, r11, r11
	or	r14, r11
	add	r9, r11
	add	r8, r7

	mov	r7, r12
	mov	r7, r14
	shr	5, r12
	shr	27, r14
	andi	0x07e0, r12, r12
	or	r14, r12
	add	r9, r12
	add	r8, r7

	mov	r7, r13
	mov	r7, r14
	shr	5, r13
	shr	27, r14
	andi	0x07e0, r13, r13
	or	r14, r13
	add	r9, r13
	add	r8, r7

	ld.b	0[r10], r10
	ld.b	0[r11], r11
	ld.b	0[r12], r12
	ld.b	0[r13], r13

	andi	0xff, r10, r14
	shl	8, r10
	or	r14, r10
	out.h	r10, 0x604[r0]

	andi	0xff, r11, r14
	shl	8, r11
	or	r14, r11
	out.h	r11, 0x604[r0]

	andi	0xff, r12, r14
	shl	8, r12
	or	r14, r12
	out.h	r12, 0x604[r0]

	andi	0xff, r13, r14
	shl	8, r13
	or	r14, r13
	out.h	r13, 0x604[r0]

	add	-4, r6
	cmp	4, r6
	bnl	1b

2:	cmp	0, r6
	be	9f
3:
	mov	r7, r10
	mov	r7, r11
	shr	5, r10
	shr	27, r11
	andi	0x07e0, r10, r10
	or	r11, r10
	add	r9, r10
	ld.b	0[r10], r10
	andi	0xff, r10, r11
	shl	8, r10
	or	r11, r10
	out.h	r10, 0x604[r0]
	add	r8, r7
	add	-1, r6
	bne	3b

9:	jmp	[lp]
	.size	_pcfx_span_lit32, .-_pcfx_span_lit32
	.text
	/* Preserve the ordinary .text addresses measured for the wall dispatcher.
	 * The live span kernel moved into .pcfx_spanhot; this same-size inert slot
	 * prevents that move from perturbing unrelated direct-mapped-cache layout. */
	.space	0xcc

/* Pre-lit 64-row wall-column kernel.
 *
 * C signature:
 *   void pcfx_column_lit64(unsigned count, unsigned frac,
 *                          unsigned fracstep, const uint16_t *lit);
 *
 * The cache already contains final doubled 16-bit palette words. Form four
 * vertically-scaled source addresses before loading them so the external-RAM
 * latency is not immediately consumed, then submit the words directly through
 * KING's current column cursor. COLBITS is 25 and the page wraps at 64 rows. */
	.section .pcfx_columnhot,"ax"
	.global	_pcfx_column_lit64
	.type	_pcfx_column_lit64, @function
	.macro	pcfx_wall4
	mov	r7, r10
	shr	24, r10
	andi	0x7e, r10, r10
	add	r9, r10
	add	r8, r7

	mov	r7, r11
	shr	24, r11
	andi	0x7e, r11, r11
	add	r9, r11
	add	r8, r7

	mov	r7, r12
	shr	24, r12
	andi	0x7e, r12, r12
	add	r9, r12
	add	r8, r7

	mov	r7, r13
	shr	24, r13
	andi	0x7e, r13, r13
	add	r9, r13
	add	r8, r7

	ld.h	0[r10], r10
	ld.h	0[r11], r11
	ld.h	0[r12], r12
	ld.h	0[r13], r13
	out.h	r10, 0x604[r0]
	out.h	r11, 0x604[r0]
	out.h	r12, 0x604[r0]
	out.h	r13, 0x604[r0]
	.endm

_pcfx_column_lit64:
	cmp	0, r6
	be	39f
	cmp	8, r6
	bl	38f
37:
	pcfx_wall4
	pcfx_wall4
	add	-8, r6
	cmp	8, r6
	bnl	37b
38:
	cmp	0, r6
	be	39f
36:
	mov	r7, r10
	shr	24, r10
	andi	0x7e, r10, r10
	add	r9, r10
	ld.h	0[r10], r10
	out.h	r10, 0x604[r0]
	add	r8, r7
	add	-1, r6
	bne	36b
39:
	jmp	[lp]
	.size	_pcfx_column_lit64, .-_pcfx_column_lit64
	.text
	/* Preserve all ordinary .text addresses while the live leaf occupies an
	 * existing hole in the shaped renderer section. */
	.space	0xcc

/* Bake one dense 64-texel wall column into final doubled palette words.
 *
 * C signature:
 *   void pcfx_bake_litwall64(const uint8_t *source,
 *                            const uint16_t *colormap, uint16_t *dest);
 *
 * The C loop alternated source, colormap and destination DRAM pages for every
 * texel.  Read four packed source bytes at once, issue their four colormap
 * loads together, then write four adjacent results.  The exact same final
 * words are produced with far fewer 2 KiB page changes. */
	.global	_pcfx_bake_litwall64
	.type	_pcfx_bake_litwall64, @function
_pcfx_bake_litwall64:
	/* mov's immediate is signed 5-bit, so 16 would encode as -16. */
	mov	15, r14
	add	1, r14
40:
	ld.w	0[r6], r9
	add	4, r6

	mov	r9, r10
	mov	r9, r11
	mov	r9, r12
	mov	r9, r13
	andi	0xff, r10, r10
	shr	8, r11
	shr	16, r12
	shr	24, r13
	andi	0xff, r11, r11
	andi	0xff, r12, r12
	andi	0xff, r13, r13
	shl	1, r10
	shl	1, r11
	shl	1, r12
	shl	1, r13
	add	r7, r10
	add	r7, r11
	add	r7, r12
	add	r7, r13

	ld.h	0[r10], r10
	ld.h	0[r11], r11
	ld.h	0[r12], r12
	ld.h	0[r13], r13
	st.h	r10, 0[r8]
	st.h	r11, 2[r8]
	st.h	r12, 4[r8]
	st.h	r13, 6[r8]
	add	8, r8
	add	-1, r14
	bne	40b
	jmp	[lp]
	.size	_pcfx_bake_litwall64, .-_pcfx_bake_litwall64

/* Bake one dense 32x64 flat into 8-bit lit palette indices.
 *
 * C signature:
 *   void pcfx_bake_litflat2048(const uint8_t *source,
 *                              const uint16_t *colormap, uint8_t *dest);
 *
 * The scalar loop alternates among three external-DRAM pages for every texel.
 * Batch four source bytes and four colormap reads before writing the adjacent
 * destination bytes, matching the wall baker's page-friendly schedule. */
	.section .pcfx_bakecold,"ax"
	.global	_pcfx_bake_litflat2048
	.type	_pcfx_bake_litflat2048, @function
_pcfx_bake_litflat2048:
	mov	2, r14
	shl	8, r14		/* 2048 texels / four per iteration */
41:
	ld.w	0[r6], r9
	add	4, r6

	mov	r9, r10
	mov	r9, r11
	mov	r9, r12
	mov	r9, r13
	andi	0xff, r10, r10
	shr	8, r11
	shr	16, r12
	shr	24, r13
	andi	0xff, r11, r11
	andi	0xff, r12, r12
	andi	0xff, r13, r13
	shl	1, r10
	shl	1, r11
	shl	1, r12
	shl	1, r13
	add	r7, r10
	add	r7, r11
	add	r7, r12
	add	r7, r13

	ld.h	0[r10], r10
	ld.h	0[r11], r11
	ld.h	0[r12], r12
	ld.h	0[r13], r13
	st.b	r10, 0[r8]
	st.b	r11, 1[r8]
	st.b	r12, 2[r8]
	st.b	r13, 3[r8]
	add	4, r8
	add	-1, r14
	bne	41b
	jmp	[lp]
	.size	_pcfx_bake_litflat2048, .-_pcfx_bake_litflat2048
	.text

/* Miss/record path: shade exactly like pcfx_span_lit32 while saving the final
 * doubled KRAM words contiguously for a future exact span replay. */
	.global	_pcfx_span_lit32_record
	.type	_pcfx_span_lit32_record, @function
_pcfx_span_lit32_record:
	cmp	0, r6
	be	12f
	movhi	hi(_g_pcfx_plane_record_dst), r0, r13
	ld.w	lo(_g_pcfx_plane_record_dst)[r13], r13
11:
	mov	r7, r10
	mov	r7, r11
	shr	5, r10
	shr	27, r11
	andi	0x07e0, r10, r10
	or	r11, r10
	add	r9, r10
	ld.b	0[r10], r10
	andi	0xff, r10, r10
	mov	r10, r11
	shl	8, r11
	or	r11, r10
	st.h	r10, 0[r13]
	add	2, r13
	out.h	r10, 0x604[r0]
	add	r8, r7
	add	-1, r6
	bne	11b
12:	jmp	[lp]
	.size	_pcfx_span_lit32_record, .-_pcfx_span_lit32_record

/* Hit/replay path: cached words are contiguous, so four external-RAM loads
 * share a page and no packed-coordinate extraction remains. */
	.global	_pcfx_span_cached16
	.type	_pcfx_span_cached16, @function
_pcfx_span_cached16:
	cmp	0, r6
	be	19f
	cmp	4, r6
	bl	18f
17:
	ld.h	0[r7], r10
	ld.h	2[r7], r11
	ld.h	4[r7], r12
	ld.h	6[r7], r13
	add	8, r7

	out.h	r10, 0x604[r0]
	out.h	r11, 0x604[r0]
	out.h	r12, 0x604[r0]
	out.h	r13, 0x604[r0]

	add	-4, r6
	cmp	4, r6
	bnl	17b
18:	cmp	0, r6
	be	19f
20:	ld.h	0[r7], r10
	add	2, r7
	out.h	r10, 0x604[r0]
	add	-1, r6
	bne	20b
19:	jmp	[lp]
	.size	_pcfx_span_cached16, .-_pcfx_span_cached16

/* Restore a vertical framebuffer interval from the row-major opaque background
 * cache.  The caller programs KING's write cursor for one-row increments and
 * passes the source stride in bytes (256 for the 128-word logical view). */
	.global	_pcfx_column_cached16
	.type	_pcfx_column_cached16, @function
_pcfx_column_cached16:
	cmp	0, r6
	be	24f
23:
	ld.h	0[r7], r10
	add	r8, r7
	out.h	r10, 0x604[r0]
	add	-1, r6
	bne	23b
24:
	jmp	[lp]
	.size	_pcfx_column_cached16, .-_pcfx_column_cached16

/* Snapshot a sequential KRAM run into external RAM.  king_kram_set_read() has
 * already programmed KRAMRA; select the shared data register once here instead
 * of repeating that out.h for every read as the general read_kram() helper must. */
	.global	_pcfx_kram_read_cached16
	.type	_pcfx_kram_read_cached16, @function
_pcfx_kram_read_cached16:
	mov	0x0e, r10
	out.h	r10, 0x600[r0]
	cmp	0, r6
	be	29f
	cmp	4, r6
	bl	28f
27:
	in.h	0x604[r0], r10
	in.h	0x604[r0], r11
	in.h	0x604[r0], r12
	in.h	0x604[r0], r13
	st.h	r10, 0[r7]
	st.h	r11, 2[r7]
	st.h	r12, 4[r7]
	st.h	r13, 6[r7]
	add	8, r7
	add	-4, r6
	cmp	4, r6
	bnl	27b
28:	cmp	0, r6
	be	29f
30:	in.h	0x604[r0], r10
	st.h	r10, 0[r7]
	add	2, r7
	add	-1, r6
	bne	30b
29:	jmp	[lp]
	.size	_pcfx_kram_read_cached16, .-_pcfx_kram_read_cached16
