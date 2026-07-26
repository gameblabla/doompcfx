/*
 * PC-FX save backend — BIOS backup-memory filesystem (libpcfx filesys.h).
 *
 * Replaces the GBA SRAM shim: game/settings data live as ordinary files on
 * either internal BackupRAM (/SRAM) or an external FX-BMP card (/CARD).
 * All calls return 0/positive on success or a negative FILESYS_ERR_* code;
 * PCFX_SaveStrerror turns that into a short human-readable string.
 */

#ifndef PCFX_SAVE_H
#define PCFX_SAVE_H

/* One-time init of the BIOS filesystem dispatcher. Returns 0 on success,
 * negative error code otherwise. Safe to call more than once. */
int PCFX_SaveInit(void);

/* Select the device used by subsequent load/store calls.
 * external=0 -> internal BackupRAM, external=1 -> FX-BMP card. */
void PCFX_SaveSetDevice(int external);
int  PCFX_SaveGetDevice(void);

/* Probe whether a device is present/usable (0 internal, 1 external).
 * Returns 1 if available, 0 if not. */
int PCFX_SaveDeviceAvailable(int external);

/* Pre-title device pick (platform/pcfx_devsel.c): when an FX-BMP card answers,
 * show a minimal boot-time chooser (internal vs card) and set the device;
 * otherwise silently select internal BackupRAM. Call after PCFX_SaveInit and
 * before the first load/store (i.e. before G_LoadSettings). */
void PCFX_SaveDeviceSelect(void);

/* Load up to `cap` bytes of file `name` on the current device into `dst`.
 * Returns byte count read (>=0) or a negative error code. */
int PCFX_SaveLoad(const char *name, void *dst, unsigned cap);

/* Write `len` bytes of `src` as file `name` on the current device,
 * creating/replacing it and committing on close. Returns 0 or negative. */
int PCFX_SaveStore(const char *name, const void *src, unsigned len);

/* Short error description for a negative return code. */
const char *PCFX_SaveStrerror(int rc);

#endif /* PCFX_SAVE_H */
