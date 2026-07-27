/* Keep cache-shaped PC-FX renderer code out of the ordinary .text.* stream.
 * This supplements the toolchain's default v810.x script: ordinary function
 * addresses/layout remain stable, then the explicitly shaped hot section is
 * appended at a 1 KiB instruction-cache boundary.
 *
 * Only the low 10 bits of each __pcfx_*_offset (its position mod 1024, i.e.
 * its cache INDEX) are load-bearing: they were measured against a specific
 * cold caller's own cache footprint elsewhere in .text (see the per-slot
 * comments below). The high bits used to be an absolute jump target, which
 * meant every slot after a routine that changed size had to be re-measured
 * and hand-edited even though its cache index was still correct. Each slot
 * below instead advances "." by the minimal forward distance that lands on
 * the same cache index, so the section auto-shrinks/grows with the actual
 * compiled size of everything before it and dead alignment padding cannot
 * accumulate. Re-run `make pcfx-hot-layout` after any change to confirm the
 * cache indices still land where intended -- that part is not automatic. */
SECTIONS
{
  /* Defaults are also supplied by Make so they appear in `make -pn`, but
   * PROVIDE keeps direct v810-ld users working.  A command-line --defsym wins.
   * Only bits 0-9 of each value matter (see above); they're written with
   * their historical absolute magnitude purely for continuity with prior
   * `make pcfx-hot-layout` output. */
  PROVIDE(__pcfx_renderseg_offset = 0x200);
  PROVIDE(__pcfx_span_lit_offset = 0xa40);
  PROVIDE(__pcfx_column_lit_offset = 0xb04);
  PROVIDE(__pcfx_walldispatch_offset = 0xda0);
  PROVIDE(__pcfx_span32_offset = 0x1454);

  .pcfx_renderseg ALIGN(1024) :
  {
    __pcfx_renderhot_start = .;
    . = . + (((__pcfx_renderseg_offset % 1024) - ((. - __pcfx_renderhot_start) % 1024) + 1024) % 1024);
    __pcfx_renderseg_slot = .;
    *(.pcfx_renderseg)
    /* Generic patch-column composition is not used by generated dense pages. */
    KEEP(*(.pcfx_wallcold))
    /* With inline prologues R_MapPlane occupies cache indices 0x308..0x3ff
     * and 0x000..0x233; its span call site is at 0x1ae.  Put the 0xc2-byte
     * moving span leaf wholly in the unused 0x240..0x301 interval, so
     * 170-ish calls/frame do not evict their caller. */
    . = . + (((__pcfx_span_lit_offset % 1024) - ((. - __pcfx_renderhot_start) % 1024) + 1024) % 1024);
    __pcfx_span_lit_slot = .;
    *(.pcfx_spanhot)
    /* R_DrawColumnPCFXLit occupies indices 0x280..0x2f4 and resumes at 0x2ec.
     * Put its 0xcc-byte leaf immediately after the span, so neither the wrapper
     * nor its return site is evicted.  0x2fc measured better than the old 0x304
     * once the dispatcher shrank. */
    . = . + (((__pcfx_column_lit_offset % 1024) - ((. - __pcfx_renderhot_start) % 1024) + 1024) % 1024);
    __pcfx_column_lit_slot = .;
    *(.pcfx_columnhot)
    /* This flat-cache bake path is cold during steady-state rendering. */
    *(.pcfx_bakecold)
    /* Put the compact release dispatcher over the seg loop's least-executed
     * cache band.  This deliberately trades return-block reloads for avoiding
     * the much hotter loop head and backedge blocks.
     * The seg loop's generated loop is 830 bytes (indices 0x3c4..0x3ff and
     * 0x000..0x301), so only 0x302..0x3c3 is free for the dispatcher and the
     * lit-column leaf together and SOME collision is unavoidable.  Keeping the
     * dispatcher small is therefore worth more than moving it: out-lining the
     * lit-wall cache lookup took it from 0x166 to 0x0f4 bytes and was the
     * single largest win of the 2026-07-27 pass (see R_GetLitWallColumn). */
    . = . + (((__pcfx_walldispatch_offset % 1024) - ((. - __pcfx_renderhot_start) % 1024) + 1024) % 1024);
    __pcfx_walldispatch_slot = .;
    *(.pcfx_walldispatch)
    __pcfx_hot_core_end = .;
    /* Two-load span leaf (pcfx_span32.S): the hot drawer on every map whose
     * heap cannot hold the pre-lit flat cache (E1M6 included).  R_MapPlane
     * calls it once per span.
     *
     * Re-measured 2026-07-27: R_MapPlane is now 0x360 bytes at cache index
     * 0x250, i.e. it covers 0x250..0x3ff AND 0x000..0x1af, leaving one free
     * window of 0x1b0..0x24f (160 bytes).  The old 0x054 placement -- chosen
     * when R_MapPlane was smaller and 0x054..0x13e really was free -- had come
     * to sit ENTIRELY inside the caller, so all 0xe8 bytes of the leaf collided
     * and the two evicted each other once per span.
     * The leaf is 0xe8 bytes and the window is 0xa0, so 0x48 bytes must clash
     * wherever it goes; index 0x168 spends that on R_MapPlane's tail (its last
     * 0x48 bytes) rather than on its entry block, which measured fastest.
     * Re-derive both from objdump whenever either changes size. */
    . = . + (((__pcfx_span32_offset % 1024) - ((. - __pcfx_renderhot_start) % 1024) + 1024) % 1024);
    __pcfx_span32_slot = .;
    *(.pcfx_span32hot)
    __pcfx_renderhot_end = .;
  }
}
INSERT AFTER .text;

ASSERT((__pcfx_renderseg_offset & 3) == 0,
       "PC-FX hot layout: render-seg offset must be 4-byte aligned")
ASSERT((__pcfx_span_lit_offset & 3) == 0,
       "PC-FX hot layout: lit-span offset must be 4-byte aligned")
ASSERT((__pcfx_column_lit_offset & 3) == 0,
       "PC-FX hot layout: lit-column offset must be 4-byte aligned")
ASSERT((__pcfx_walldispatch_offset & 3) == 0,
       "PC-FX hot layout: wall-dispatch offset must be 4-byte aligned")
ASSERT((__pcfx_span32_offset & 3) == 0,
       "PC-FX hot layout: two-load span offset must be 4-byte aligned")

/* Weapon pattern decode/SAT state is deliberately near the top of RAM.  Catch
 * resident-image growth at link time instead of silently wrapping VDC state past
 * 0x200000.  6 KiB is the minimum proven stack gap for the current bare-metal
 * call graph; release currently retains more than that. */
ASSERT(__end <= (__stack - 0x1800),
       "PC-FX RAM overflow: less than 6 KiB remains below the stack")
