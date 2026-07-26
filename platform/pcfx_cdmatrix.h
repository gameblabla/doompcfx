/* CD-DMA matrix self-test (EXTRA=-DDEV_CD_MATRIX): one burn, every CD read
 * path x arm shape x chunking x KRAM region, verified against a build-time
 * CRC of the exact on-disc extent and shown as a photographable grid.
 * Runs from I_InitScreen_e32 once video+text are up; never returns. */
#ifndef PCFX_CDMATRIX_H
#define PCFX_CDMATRIX_H

void pcfx_cdmatrix_run(void);

#endif
