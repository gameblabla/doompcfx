/*
 * PC-FX save backend — BIOS backup-memory filesystem (libpcfx filesys.h).
 *
 * The BIOS owns the BackupRAM FAT, formatting and file commits, so the same
 * code path works on internal BackupRAM (/SRAM), an external FX-BMP card
 * (/CARD) and PC-FXGA. See pcfx_save.h for the interface contract.
 */

#include <string.h>

#include <pcfx/filesys.h>

#include "z_zone.h"
#include "pcfx_save.h"

/* BIOS scratch space for the filesystem dispatcher. Must stay allocated for
 * as long as filesystem calls may be made. filesys.h documents a >= 1024 byte
 * minimum, but the real PC-FX BIOS rejects anything under ~6 KiB with
 * "out of BIOS heap" (-30); measured on hardware-accurate emulation:
 * filesys_init succeeds at 6144+ and fails at 4096-. 8 KiB gives margin for the
 * write path. Allocated from the DOOM zone (PU_STATIC, never purged) rather than
 * a static array so it stays clear of the ~6 KiB stack budget below __stack. */
#define FS_HEAP_BYTES 8192
static void *fs_heap;

static int fs_ready;        /* filesys_init succeeded */
static int fs_init_rc;      /* last init return code */
static int fs_external;     /* 0 = /SRAM (internal), 1 = /CARD (FX-BMP) */

static const char *device_root(int external)
{
    return external ? FILESYS_PATH_EXTERNAL : FILESYS_PATH_INTERNAL;
}

/* "<root>/<name>" -> dst. Paths here are short ("/SRAM/DOOMFX.SAV"), the
 * buffer is sized well past any name this port uses. */
static void build_path(char *dst, unsigned cap, const char *name)
{
    const char *root = device_root(fs_external);
    unsigned n = 0;

    while (*root && n + 1 < cap)
        dst[n++] = *root++;
    if (n + 1 < cap)
        dst[n++] = '/';
    while (*name && n + 1 < cap)
        dst[n++] = *name++;
    dst[n] = 0;
}

int PCFX_SaveInit(void)
{
    if (!fs_heap)
        fs_heap = Z_Malloc(FS_HEAP_BYTES, PU_STATIC, NULL);

    /* Re-run the BIOS dispatcher init on every entry instead of caching a
     * one-shot "ready" flag. A DOOM level load streams megabytes from CD via
     * eris_cd_read_dma (custom KING/SCSI path, bypassing the BIOS), which leaves the
     * BIOS fileio state disturbed enough that the next BackupRAM open/read fails
     * (observed: BIOS error -22 on the second load after a level reload). A fresh
     * filesys_init before each access re-establishes that state so repeated
     * loads/saves keep working across level loads. */
    fs_init_rc = filesys_init(fs_heap, FS_HEAP_BYTES);
    fs_ready = (fs_init_rc >= 0);

    return fs_ready ? 0 : fs_init_rc;
}

void PCFX_SaveSetDevice(int external)
{
    fs_external = external ? 1 : 0;
}

int PCFX_SaveGetDevice(void)
{
    return fs_external;
}

int PCFX_SaveDeviceAvailable(int external)
{
    char probe[40];
    int fd;

    if (PCFX_SaveInit() < 0)
        return 0;

    /* Opening a (most likely absent) file distinguishes "device answers"
     * (ERR_NOT_FOUND, or a valid fd) from "no device / unformatted"
     * (ERR_DEVICE_UNAVAILABLE / ERR_PATH_UNAVAILABLE). */
    strcpy(probe, device_root(external));
    strcat(probe, "/DOOMFX.PRB");

    fd = filesys_open(probe, FILESYS_OPEN_READ);
    if (fd >= 0)
    {
        filesys_close(fd);
        return 1;
    }

    return fd == FILESYS_ERR_NOT_FOUND;
}

int PCFX_SaveLoad(const char *name, void *dst, unsigned cap)
{
    char path[40];
    unsigned got = 0;
    int rc;

    rc = PCFX_SaveInit();
    if (rc < 0)
        return rc;

    build_path(path, sizeof path, name);

    rc = filesys_load_file(path, dst, cap, &got);
    if (rc < 0)
        return rc;

    return (int)got;
}

int PCFX_SaveStore(const char *name, const void *src, unsigned len)
{
    char path[40];
    int rc;

    rc = PCFX_SaveInit();
    if (rc < 0)
        return rc;

    build_path(path, sizeof path, name);

    return filesys_save_file(path, src, len);
}

const char *PCFX_SaveStrerror(int rc)
{
    return filesys_strerror(rc);
}
