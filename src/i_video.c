/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2006 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  DOOM graphics stuff for SDL
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <math.h>

#include "doomstat.h"
#include "doomdef.h"
#include "doomtype.h"
#include "v_video.h"
#include "r_draw.h"
#include "d_main.h"
#include "d_event.h"
#include "i_video.h"
#include "i_sound.h"
#include "z_zone.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "st_stuff.h"
#include "lprintf.h"

#include "i_system_e32.h"

#include "global_data.h"

//
// I_StartTic
//

void I_StartTic (void)
{
    I_ProcessKeyEvents();
}

//
// I_StartFrame
//
#ifdef __v810__
/* platform/i_sound_pcfx.c. Declared here rather than pulling a platform header
 * into portable engine code. */
void pcfx_cdda_pump(void);
#endif

void I_StartFrame (void)
{
#ifdef __v810__
    /* Service CD-DA music. A CD data read stops the drive's audio engine, so
     * the manager needs a regular tick to notice and restart playback; this is
     * the one place in D_DoomLoop that runs every frame in every game state
     * (title, menu, level, intermission). */
    pcfx_cdda_pump();
#endif
}


boolean I_StartDisplay(void)
{
    unsigned short* backbuffer = I_GetBackBuffer();

    _g->screens[0].data = backbuffer;

    // Same with base row offset.
    drawvars.byte_topleft = backbuffer;

    return true;
}

void I_EndDisplay(void)
{

}


//
// I_InitInputs
//

static void I_InitInputs(void)
{

}
/////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////
// Palette stuff.
//
/* GAMMA BOOST (menu, 0..5) is a DISPLAY brightness: a mid-tone parabola that
 * pins 0->0 and 255->255, folded into the platform's one-time conversion of all
 * 14 PLAYPAL sub-palettes (I_SetPalletteIndexed_e32). Handing the platform the
 * whole PLAYPAL lets it pre-convert every damage/pickup/radiation tint, so a
 * flash step never runs the expensive RGB->YUV search mid-game (it was the
 * 0.1%-low frame spike). */
static void I_UploadNewPalette(int pal)
{
  // This is used to replace the current 256 colour cmap with a new one
  // Used by 256 colour PseudoColor modes

    if(!_g->pallete_lump)
    {
        _g->pallete_lump = W_CacheLumpName("PLAYPAL");
    }

    _g->current_pallete = &_g->pallete_lump[pal*256*3];
    I_SetPalletteIndexed_e32(pal, (int)_g->gamma, _g->pallete_lump);
}

//////////////////////////////////////////////////////////////////////////////
// Graphics API

void I_ShutdownGraphics(void)
{
}

//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
}

//
// I_FinishUpdate
//
#define NO_PALETTE_CHANGE 1000

void I_FinishUpdate (void)
{
    if (_g->newpal != NO_PALETTE_CHANGE)
	{
        I_UploadNewPalette(_g->newpal);
        _g->newpal = NO_PALETTE_CHANGE;
	}

    I_FinishUpdate_e32((const byte*)_g->screens[0].data, _g->current_pallete, SCREENWIDTH, SCREENHEIGHT);
}

//
// I_SetPalette
//
void I_SetPalette (int pal)
{
    /* Upload NOW rather than deferring to I_FinishUpdate. On level entry D_Display
     * takes the wipe branch and SKIPS I_FinishUpdate (the PC-FX melt is an instant
     * cut), so a deferred palette — notably the status bar's V_SetPalette(0) at
     * ST_Start — would not reach the VCE until the next palette event (a damage/
     * pickup tint). Until then the HUD kept the previous, brighter palette, which
     * is why the numbers looked too bright until you got hit. Uploading here makes
     * every palette change take effect immediately. (Graphics are initialised well
     * before any I_SetPalette caller runs.) */
    _g->newpal = NO_PALETTE_CHANGE;   /* consumed here; don't double-upload later */
    I_UploadNewPalette(pal);
}

void I_FadePalette(boolean fade_in)
{
    I_FadePalette_e32(fade_in ? 1 : 0);
}

void I_PrepareLevelLoad(void)
{
    I_PrepareLevelLoad_e32();
}

void I_PreallocStatics(void)
{
    I_PreallocStatics_e32();
}

//
// I_SetRainbowActive - gate the RAINBOW sky layer on gameplay (see i_video.h).
//
void I_SetRainbowActive(boolean on)
{
    I_SetRainbowActive_e32(on ? 1 : 0);
}


void I_PreInitGraphics(void)
{
	I_InitScreen_e32();
}

// CPhipps -
// I_SetRes
// Sets the screen resolution
void I_SetRes(void)
{
    //backbuffer
    _g->screens[0].width = SCREENWIDTH;
    _g->screens[0].height = SCREENHEIGHT;

    lprintf(LO_INFO,"I_SetRes: Using resolution %dx%d", SCREENWIDTH, SCREENHEIGHT);
}

void I_InitGraphics(void)
{
    static int    firsttime=1;

    if (firsttime)
    {
        firsttime = 0;

        lprintf(LO_INFO, "I_InitGraphics: %dx%d", SCREENWIDTH, SCREENHEIGHT);

        /* Set the video mode */
        I_UpdateVideoMode();

        /* Initialize the input system */
        I_InitInputs();

        I_CreateBackBuffer_e32();
    }
}

void I_UpdateVideoMode(void)
{
    lprintf(LO_INFO, "I_SetRes: %dx%d", SCREENWIDTH, SCREENHEIGHT);
    I_SetRes();

    lprintf(LO_INFO, "R_InitBuffer:");
    R_InitBuffer();
}
