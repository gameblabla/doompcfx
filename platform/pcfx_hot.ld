/* Keep cache-shaped PC-FX renderer code out of the ordinary .text.* stream.
 * This supplements the toolchain's default v810.x script: ordinary function
 * addresses/layout remain stable, then the explicitly shaped hot section is
 * appended at a 1 KiB instruction-cache boundary. */
SECTIONS
{
  /* Defaults are also supplied by Make so they appear in `make -pn`, but
   * PROVIDE keeps direct v810-ld users working.  A command-line --defsym wins. */
  PROVIDE(__pcfx_renderseg_offset = 0x200);
  PROVIDE(__pcfx_span_lit_offset = 0xa40);
  PROVIDE(__pcfx_column_lit_offset = 0xb04);
  PROVIDE(__pcfx_walldispatch_offset = 0xda0);
  PROVIDE(__pcfx_hot_core_end_offset = 0x10e0);
  PROVIDE(__pcfx_span32_offset = 0x1454);

  .pcfx_renderseg ALIGN(1024) :
  {
    __pcfx_renderhot_start = .;
    . = __pcfx_renderhot_start + __pcfx_renderseg_offset;
    __pcfx_renderseg_slot = .;
    *(.pcfx_renderseg)
    /* Generic patch-column composition is not used by generated dense pages. */
    KEEP(*(.pcfx_wallcold))
    /* With inline prologues R_MapPlane occupies cache indices 0x308..0x3ff
     * and 0x000..0x233; its span call site is at 0x1ae.  Put the 0xc2-byte
     * moving span leaf wholly in the unused 0x240..0x301 interval, so
     * 170-ish calls/frame do not evict their caller. */
    . = __pcfx_renderhot_start + __pcfx_span_lit_offset;
    __pcfx_span_lit_slot = .;
    *(.pcfx_spanhot)
    /* R_DrawColumnPCFXLit occupies indices 0x280..0x2f4 and resumes at 0x2ec.
     * Put its 0xcc-byte leaf at 0x304, immediately after the span, so neither
     * the wrapper nor its return site is evicted. */
    . = __pcfx_renderhot_start + __pcfx_column_lit_offset;
    __pcfx_column_lit_slot = .;
    *(.pcfx_columnhot)
    /* This flat-cache bake path is cold during steady-state rendering. */
    *(.pcfx_bakecold)
    /* Put the compact release dispatcher over the seg loop's least-executed
     * cache band.  This deliberately trades return-block reloads for avoiding
     * the much hotter loop head and backedge blocks. */
    . = __pcfx_renderhot_start + __pcfx_walldispatch_offset;
    __pcfx_walldispatch_slot = .;
    *(.pcfx_walldispatch)
    /* Preserve the measured end of the shaped section as the dispatcher
     * changes size, preventing unrelated rodata/data placement noise. */
    . = __pcfx_renderhot_start + __pcfx_hot_core_end_offset;
    __pcfx_hot_core_end = .;
    /* Two-load span leaf (pcfx_span32.S): the hot drawer on every map whose
     * heap cannot hold the pre-lit flat cache (E1M6 included).  R_MapPlane
     * calls it once per span, and with the lit path not taken it executes cache
     * indices 0x370..0x054 (wrapped), 0x13e..0x1d8, 0x230..0x26e and
     * 0x2a2..0x2d0.  Put the 0xe8-byte leaf at index 0x054, filling
     * R_MapPlane's largest unused window (0x054..0x13e), so caller and callee
     * are both fully resident across the per-span call.
     * Re-derive both from objdump whenever either changes size. */
    . = __pcfx_renderhot_start + __pcfx_span32_offset;
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
