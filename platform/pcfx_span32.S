/* Two-load dense (32x64) flat span kernel for the NEC V810.
 *
 * C signature:
 *   void pcfx_span32(unsigned count, const draw_span_vars_t *dsvars);
 *
 * This is the drawer every floor/ceiling pixel goes through on maps whose heap
 * cannot afford the 128 KiB pre-lit flat cache (E1M6 among them: litflat_slots
 * is 0 there), so it is the single hottest routine in the game -- 21.6% of all
 * measured V810 cycles on the E1M6 automove benchmark.
 *
 * Why hand asm, when the pattern catalogue says GCC ties hand-scheduled drawers:
 * the win here is NOT instruction scheduling, it is the 2 KiB DRAM page.  Main
 * RAM charges +3 cycles whenever an access leaves the currently open 2 KiB page
 * (shell/emu/core.c RAMLPCHECK, one global last-page register).  The C drawer
 * loads flat[texel] and then colormap[that] per pixel; the dense flat page is
 * exactly one 2 KiB-aligned DRAM page and the 512-byte colormap is in another,
 * so every pixel changed the open page TWICE -- +6 cycles/pixel, about 2.2 ms
 * per frame at the measured ~10.4k span pixels/frame.
 *
 * Four pixels are therefore processed as: four texel addresses (ALU only), four
 * clustered ld.b (one page change), four colormap addresses, four clustered
 * ld.h (one page change), four out.h.  That is 2 page changes per 4 pixels
 * instead of 8, and the clustering also drops each load's own penalty from +2
 * (after ALU) to +1 (after another load).  Output is bit-identical to
 * R_DrawSpanPixel32 -- same texel index, same colormap word, same write order.
 *
 * Registers r6..r19 are caller-saved in the v810-gcc ABI, so this leaf needs no
 * stack frame.  The caller has already programmed KING's KRAM write cursor with
 * FB_span (auto-increments one word per write).
 *
 * The kernel is placed by platform/pcfx_hot.ld into a 1 KiB icache window that
 * R_MapPlane, its per-span caller, does not execute from: in ordinary .text the
 * two overlapped by ~290 bytes of executed code and evicted each other once per
 * span (15.2k icache misses/frame between them).
 */
	.section .pcfx_span32hot,"ax"
	.global	_pcfx_span32
	.type	_pcfx_span32, @function

/* texel address for one pixel: source + (((pos >> 5) & 0x07e0) | (pos >> 27)),
 * then step the packed coordinate.  \d receives the address, r15 is scratch. */
	.macro	pcfx_span32_addr d
	mov	r7, \d
	mov	r7, r15
	shr	5, \d
	shr	27, r15
	andi	0x07e0, \d, \d
	or	r15, \d
	add	r9, \d
	add	r8, r7
	.endm

/* colormap word address for a loaded texel byte: colormap + 2*(byte & 0xff).
 * ld.b sign-extends, so the mask is mandatory for indices >= 0x80. */
	.macro	pcfx_span32_cmap d
	andi	0xff, \d, \d
	add	\d, \d
	add	r10, \d
	.endm

_pcfx_span32:
	/* Cluster the four descriptor loads; r7 (dsvars) dies with the last. */
	mov	r7, r15
	ld.w	0[r15], r7		/* position */
	ld.w	4[r15], r8		/* step     */
	ld.w	8[r15], r9		/* source   */
	ld.w	12[r15], r10		/* colormap */

	cmp	0, r6
	be	9f
	cmp	4, r6
	bl	2f

1:	pcfx_span32_addr r11
	pcfx_span32_addr r12
	pcfx_span32_addr r13
	pcfx_span32_addr r14

	ld.b	0[r11], r11
	ld.b	0[r12], r12
	ld.b	0[r13], r13
	ld.b	0[r14], r14

	pcfx_span32_cmap r11
	pcfx_span32_cmap r12
	pcfx_span32_cmap r13
	pcfx_span32_cmap r14

	ld.h	0[r11], r11
	ld.h	0[r12], r12
	ld.h	0[r13], r13
	ld.h	0[r14], r14

	out.h	r11, 0x604[r0]
	out.h	r12, 0x604[r0]
	out.h	r13, 0x604[r0]
	/* The loop test sits between the third and fourth write: `out.h` neither
	 * writes nor reads flags, so it both dodges the +2 flag-use stall on the
	 * branch and breaks the consecutive-out pairing penalty on that write. */
	add	-4, r6
	cmp	4, r6
	out.h	r14, 0x604[r0]
	bnl	1b

2:	cmp	0, r6
	be	9f
3:	mov	r7, r11
	mov	r7, r15
	shr	5, r11
	shr	27, r15
	andi	0x07e0, r11, r11
	or	r15, r11
	add	r9, r11
	ld.b	0[r11], r11
	andi	0xff, r11, r11
	add	r11, r11
	add	r10, r11
	ld.h	0[r11], r11
	out.h	r11, 0x604[r0]
	add	r8, r7
	add	-1, r6
	bne	3b

9:	jmp	[lp]
	.size	_pcfx_span32, .-_pcfx_span32
	.text
