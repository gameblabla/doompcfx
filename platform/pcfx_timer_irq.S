/* Minimal interval-timer interrupt handler for the NEC V810.
 *
 * This fires PCFX_TIMER_PERIOD-ly (1 kHz) for the whole run, so it is the one
 * routine whose cost is paid ~51 times inside every rendered frame regardless
 * of what the frame draws.
 *
 * It used to be a C function carrying __attribute__((interrupt)), which makes
 * GCC spill every register an ordinary call could clobber -- r6..r19, r30 and
 * lp, seventeen st.w on entry and seventeen ld.w on exit -- purely because the
 * body made two out-of-line calls (timer_ack_irq, tick_ms). Measured on the
 * E1M6 automove benchmark that came to 40 instructions and ~306 cycles per
 * interrupt, of which the profiler attributed nearly 29 icache misses: at 1 kHz
 * the handler is evicted between every firing, so it re-missed its own 160
 * bytes each time AND evicted 160 bytes of whichever hot drawer it interrupted.
 *
 * The work itself is an I/O read-modify-write and one counter increment, which
 * needs exactly two scratch registers. Doing it here, in the vector, keeps the
 * handler at 14 instructions / 56 bytes and touches nothing else.
 *
 * Registered by pcfx_time_init() via irq_set_raw_handler(0x9, ...), i.e. it IS
 * the vector, so it returns with reti (which restores PC/PSW from EIPC/EIPSW).
 */

	.global	_pcfx_timer_irq
	.type	_pcfx_timer_irq, @function
	.text
_pcfx_timer_irq:
	add	-8, sp
	st.w	r10, 0[sp]
	st.w	r11, 4[sp]

	/* timer_ack_irq(): clear the expiry flag (bit 2) in the control port. */
	in.h	0xF00[r0], r10
	andi	~4, r10, r10
	out.h	r10, 0xF00[r0]

	/* tick_ms(): g_ms_irq++.  It is volatile, so load and store it for real. */
	movhi	hi(_g_ms_irq), r0, r11
	movea	lo(_g_ms_irq), r11, r11
	ld.w	0[r11], r10
	add	1, r10
	st.w	r10, 0[r11]

	ld.w	0[sp], r10
	ld.w	4[sp], r11
	add	8, sp
	reti
	.size	_pcfx_timer_irq, .-_pcfx_timer_irq
