/* pcfx.h — low-level NEC PC-FX register access for the DOOM port.
 *
 * KING (the HuC6272 graphics chip) is programmed through an index/data port pair;
 * KRAM (its 512KB video RAM, addressed as 16-bit words) is reached only through
 * KING's data port, which auto-increments the write address after every store.
 *
 * ACCESS METHOD: under pcfx-headless (mednafen) the KING index/data registers and
 * the KRAM auto-increment respond to 16-bit *memory-mapped* stores in the
 * 0x80000000 I/O window (0x80000600 = index, 0x80000604 = data). The V810 out.h
 * I/O-port instruction does NOT drive KRAM here, so we use memory-mapped st.h. A
 * memory-mapped st.h is still a single instruction (the base 0x80000000 is held in
 * a register across a loop), so this is exactly the cheap streaming write we want.
 *
 * PERFORMANCE MODEL: a KRAM *seek* (loading the write-address register) is the
 * expensive step. KING only accepts 16-bit writes; a 32-bit register like KRAMWA
 * is written as two 16-bit bus cycles (a single st.w splits into those on the
 * 16-bit bus). Every hot loop must seek ONCE (king_kram_set_cursor) then stream
 * many write_kram() words, letting the hardware auto-increment walk the run. */
#ifndef PCFX_H
#define PCFX_H

#include <stdint.h>
#include <pcfx/tetsu.h>   /* tetsu_get_raster for pcfx_tetsu_raster_stable */

/* ---- KING (HuC6272) index/data ports, memory-mapped at 0x80000600/0604 ---- */
#define PCFX_IO_BASE        0x80000000u
#define KING_PORT_INDEX     (*(volatile uint16_t*)(PCFX_IO_BASE + 0x0600u))
#define KING_PORT_DATA16    (*(volatile uint16_t*)(PCFX_IO_BASE + 0x0604u))
#define KING_PORT_DATA32    (*(volatile uint32_t*)(PCFX_IO_BASE + 0x0604u))

/* KING register indices. */
#define KING_REG_KRAMRA     0x0Cu   /* 32-bit read address + auto-increment  */
#define KING_REG_KRAMWA     0x0Du   /* 32-bit write address + auto-increment */
#define KING_REG_KRAMWD     0x0Eu   /* KRAM data (auto-increments)           */

/* KRAMWA 32-bit format: bits[17:0]=word addr, bits[27:18]=signed 10-bit inc. */
#define KING_KRAM_ADDR_MASK 0x0003FFFFu
#define KING_KRAM_INC_SHIFT 18
#define KING_KRAM_INC_MASK  0x3FFu
#define KING_KRAM_INC1      (1u << KING_KRAM_INC_SHIFT)

/* ---- VCE / Tetsu display status; bit 0x20 = vblank ---- */
#define FXVCE_STATUS        (*(volatile uint16_t*)(PCFX_IO_BASE + 0x0400u))
#define FXVCE_VBLANK_BIT    0x0020u

/* ---- Stable Tetsu raster read (HuC6261 hardware-bug workaround) ------------
 * The Tetsu raster counter (read from I/O port 0x300 by tetsu_get_raster)
 * has a documented hardware bug: a single read can latch a transitional/bogus
 * value, and mednafen only ever returns the raster from the first OR the last
 * cycle of the read.  The fix (pcfx_7up_notes.txt, "Reading the Tetsu raster
 * position and waiting for the next frame") is to read TWICE and require the two
 * reads to agree, retrying until they do.
 *
 * Every raster-based timing decision (vblank detection for the page flip and the
 * frame clock, and the RAINBOW re-arm) MUST read through this helper.  A single
 * bogus read makes vblank detection fire at the wrong scanline; under load that
 * mistimes the page flip and the hardware-sprite SAT/pattern uploads relative to
 * the VDC scanout, so the weapon sprites tear or glitch. */
static inline unsigned pcfx_tetsu_raster_stable(void)
{
    unsigned a = (unsigned)tetsu_get_raster();
    /* Two consecutive reads that agree = a coherent value.  They differ only if
     * one caught a mid-transition read (or the raster genuinely stepped a line
     * between them); either way retry.  Bounded so a pathological emulator can
     * never wedge the frame loop — 8 tries is far more than convergence needs. */
    for (unsigned tries = 0; tries < 8u; tries++) {
        unsigned b = (unsigned)tetsu_get_raster();
        if (a == b)
            return a;
        a = b;
    }
    return a;
}

/* ---- interrupt-atomic KING access -----------------------------------------
 * Silicon-verified (maka/tank3d hardware bring-up, fix2_irq_atomic_king +
 * libpcfx probes 027-029, on the same real hardware this port targets): a
 * firing interval-timer IRQ that lands INSIDE a KING 0x600/0x604
 * register-select/data sequence corrupts the access. pcfxemu performs KING
 * accesses atomically, so the corruption exists only on silicon — it is what
 * garbled the boot-panel text (seek + 3-word bursts, many IRQ hits) while the
 * solid load bar looked fine (corruption inside a run of identical words is
 * invisible). Any multi-write KING sequence that can run with the ~1 ms timer
 * IRQ live must sit between pcfx_irq_save() and pcfx_irq_restore().
 *
 * Full PSW save/restore (not the libpcfx irq_disable helper, which rebuilds
 * PSW from just the ID bit and drops the interrupt-level field). */
static inline uint32_t pcfx_irq_save(void)
{
    uint32_t psw;
    __asm__ volatile ("stsr psw, %0" : "=r"(psw));
    __asm__ volatile ("ldsr %0, psw" : : "r"(psw | 0x1000u) : "memory");
    return psw;
}
static inline void pcfx_irq_restore(uint32_t psw)
{
    __asm__ volatile ("ldsr %0, psw" : : "r"(psw) : "memory");
}

/* Select a KING register for a subsequent data read/write (out.h to 0x600). */
static inline __attribute__((always_inline)) void king_out_idx(uint16_t reg)
{ __asm__ volatile ("out.h %0, 0x600[r0]" : : "r"(reg)); }
static inline void king_reg_select(uint8_t reg) { king_out_idx(reg); }

/* Stream one 16-bit word to the current KRAM cursor (auto-increments). One out.h
 * — the primitive every drawing inner loop uses. */
static inline __attribute__((always_inline)) void write_kram(uint16_t v)
{ __asm__ volatile ("out.h %0, 0x604[r0]" : : "r"(v)); }

/* Set the KRAM write cursor to `word_addr` with signed word auto-increment `inc`,
 * then select the data register (0xE) so write_kram() streams from there. This
 * mirrors liberis eris_king_set_kram_write: select KRAMWA (0xD), load the 32-bit
 * (addr | incr<<18) with one out.w, then select KRAMWD (0xE). inc=1 for horizontal
 * spans, inc=row-stride for vertical columns. The seek is the ONLY expensive step;
 * do it once per column/span, never per pixel. */
static inline __attribute__((always_inline)) void king_kram_set_cursor(uint32_t word_addr, int inc)
{
    uint32_t v = (word_addr & KING_KRAM_ADDR_MASK)
               | (((uint32_t)inc & KING_KRAM_INC_MASK) << KING_KRAM_INC_SHIFT);
    king_out_idx(KING_REG_KRAMWA);
    __asm__ volatile ("out.w %0, 0x604[r0]" : : "r"(v));
    king_out_idx(KING_REG_KRAMWD);
}

/* Point the KRAM write cursor at `word_addr` with +1-word auto-increment. */
static inline void king_kram_write_addr(uint32_t word_addr)
{ king_kram_set_cursor(word_addr, 1); }

/* KRAM write cursor with an explicit hardware page (bit 31). Page 0 holds the
 * framebuffer; page 1 holds the RAINBOW sky + ADPCM sample bank. */
static inline void king_kram_set_cursor_page(uint32_t word_addr, int inc, int page)
{
    uint32_t v = (word_addr & KING_KRAM_ADDR_MASK)
               | (((uint32_t)inc & KING_KRAM_INC_MASK) << KING_KRAM_INC_SHIFT)
               | (page ? 0x80000000u : 0u);
    king_out_idx(KING_REG_KRAMWA);
    __asm__ volatile ("out.w %0, 0x604[r0]" : : "r"(v));
    king_out_idx(KING_REG_KRAMWD);
}

/* Point the KRAM READ cursor at `word_addr` with signed word auto-increment `inc`
 * (KRAMRA, reg 0x0C) — the read counterpart of king_kram_set_cursor. read_kram()
 * then streams words from it. Read and write cursors are independent, so a
 * read-modify-write can seek the read cursor, read a run, seek the write cursor,
 * and write the run back. */
static inline void king_kram_set_read(uint32_t word_addr, int inc)
{
    uint32_t v = (word_addr & KING_KRAM_ADDR_MASK)
               | (((uint32_t)inc & KING_KRAM_INC_MASK) << KING_KRAM_INC_SHIFT);
    king_out_idx(KING_REG_KRAMRA);
    __asm__ volatile ("out.w %0, 0x604[r0]" : : "r"(v));
}

/* Read one 16-bit word from the current KRAM read cursor (auto-increments). The
 * data port (0x0E) is shared with writes; an `in` reads KRAMRA, an `out` writes
 * KRAMWA (mirrors liberis eris_king_kram_read). */
static inline uint16_t read_kram(void)
{
    uint16_t r;
    king_out_idx(KING_REG_KRAMWD);
    __asm__ volatile ("in.h 0x604[r0], %0" : "=r"(r));
    return r;
}

/* General KING register writes: select `reg` (port 0x600), then data (0x604). */
static inline void king_reg16(uint16_t reg, uint16_t val)
{ king_out_idx(reg); __asm__ volatile ("out.h %0, 0x604[r0]" : : "r"(val)); }
static inline void king_reg32(uint16_t reg, uint32_t val)
{ king_out_idx(reg); __asm__ volatile ("out.w %0, 0x604[r0]" : : "r"(val)); }

/* Bulk helpers (cursor must already be set). */
static inline void king_kram_stream(const uint16_t *src, int n)
{ for (int i = 0; i < n; i++) write_kram(src[i]); }

static inline void king_kram_fill(uint16_t value, int n)
{ for (int i = 0; i < n; i++) write_kram(value); }

#endif /* PCFX_H */
