// PsionDoomApp.h
//
// Copyright 17/02/2019 
//

#ifndef HEADER_ISYSTEME32
#define HEADER_ISYSTEME32


#ifdef __cplusplus
extern "C" {
#endif


void I_InitScreen_e32();

void I_CreateBackBuffer_e32();

int I_GetVideoWidth_e32();

int I_GetVideoHeight_e32();

void I_FinishUpdate_e32(const byte* srcBuffer, const byte* pallete, const unsigned int width, const unsigned int height);

void I_SetPalletteIndexed_e32(int pal, int gamma, const byte* playpal);

/* Hardware palette fade. fade_in=0 steps the cached shared KING/VDC palette to
 * black; fade_in=1 restores it. Each direction is vblank paced. */
void I_FadePalette_e32(int fade_in);

/* Replace the current display with a black level-load page, hide VDC sprites,
 * and present centered VDC "LOADING..." text. */
void I_PrepareLevelLoad_e32(void);
void I_PreallocStatics_e32(void);

/* Show/hide the HuC6271 RAINBOW sky. Only shown during gameplay (GS_LEVEL); off
 * (KRAM cleared) on the title, menus, intermission and finale. No-op if unchanged. */
void I_SetRainbowActive_e32(int on);

void I_ProcessKeyEvents();

/* How many tics the caller is about to build back to back, so relative input
 * (the mouse, whose counter is drained by the first read of the burst) can be
 * shared out evenly instead of landing entirely on the first tic. */
void I_SetTicBurst(int ntics);

int I_GetTime_e32(void);

void I_Error (const char *error, ...);

void I_Quit_e32();

unsigned short* I_GetBackBuffer();

unsigned short* I_GetFrontBuffer();

#ifdef __cplusplus
}
#endif


#endif
