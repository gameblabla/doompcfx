/* pcfx_weapon.h -- first-person weapon as HuC6270 VDC hardware sprites.
 *
 * The player's weapon (psprite) is no longer drawn into the KING KRAM
 * framebuffer by the software renderer; it is composited by the two HuC6270
 * VDCs in 256-colour combined-sprite mode and clipped above the HUD by the
 * VDC's vertical display window.  See pcfx_weapon.c for the hardware contract.
 */
#ifndef PCFX_WEAPON_H
#define PCFX_WEAPON_H

/* One-time hardware setup: init both VDCs, load the weapon sprite palette into
 * the VCE, enable 256-colour combined sprites, and set the HUD-clip window.
 * Call once after the KING/tetsu video init. */
void pcfx_weapon_init(void);

/* Per rendered frame: bracket the psprite placement with begin()/end(). */
void pcfx_weapon_begin(void);

/* Place generated frame `fidx` (from pcfx_weapon_lookup) with its top-left at
 * physical screen pixel (x_left, y_top).  slot selects a VRAM pattern region:
 * 0 = weapon, 1 = muzzle flash. */
void pcfx_weapon_add(int slot, int fidx, int x_left, int y_top);

/* Finish assembling the frame in RAM (marks it ready to flush).  Does NOT touch
 * the VDCs — the hardware update is deferred to pcfx_weapon_present(). */
void pcfx_weapon_end(void);

/* Flush the assembled frame to the VDCs: upload any changed pattern data and DMA
 * the SAT.  MUST be called during vblank (from the presenter, right after the
 * page flip) so the pattern upload never races the VDC's sprite scanout — doing
 * it mid-frame tears the weapon on the frame it changes.  No-op if no new frame
 * was assembled since the last flush. */
void pcfx_weapon_present(void);

/* Park every weapon sprite offscreen and flush, so no weapon shows.  Call when
 * leaving gameplay (intermission / finale / menu) — otherwise the last frame's
 * SAT stays latched in the VDCs and the weapon keeps compositing over those
 * non-level screens.  Safe to call every frame (cheap: SAT-only). */
void pcfx_weapon_hide(void);
void pcfx_weapon_prealloc(void);  /* boot-time zone alloc of the decode scratch */

/* Generated frame index for (DOOM spritenum, 0-based frame), or -1 if this
 * sprite/frame has no VDC weapon art (caller should fall back to software). */
int pcfx_weapon_lookup(int spritenum, int frame);

#endif
