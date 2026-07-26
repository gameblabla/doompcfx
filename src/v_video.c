/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
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
 *  Gamma correction LUT stuff.
 *  Color range translation support
 *  Functions to draw patches (by post) directly to screen.
 *  Functions to blit a block to the screen.
 *
 *-----------------------------------------------------------------------------
 */

#include "doomdef.h"
#include "r_main.h"
#include "r_draw.h"
#include "m_bbox.h"
#include "w_wad.h"   /* needed for color translation lump lookup */
#include "v_video.h"
#include "i_video.h"
#include "lprintf.h"

#include "global_data.h"
#include "gba_functions.h"

/*
 * V_DrawBackground tiles a 64x64 patch over the entire screen, providing the
 * background for the Help and Setup screens, and plot text betwen levels.
 * cphipps - used to have M_DrawBackground, but that was used the framebuffer
 * directly, so this is my code from the equivalent function in f_finale.c
 */
void V_DrawBackground(const char* flatname)
{
    /* erase the entire screen to a tiled 64x64 flat, drawn as fat pixels to KRAM */
    const byte *src;
    int         lump;

    src = W_CacheLumpNum(lump = _g->firstflat + R_FlatNumForName(flatname));

    for(unsigned int y = 0; y < SCREENHEIGHT; y++)
    {
        const byte* s = &src[(y & 63) * 64];
        FB_span(0, y);
        for(unsigned int x = 0; x < SCREENWIDTH; x++)
            FB_puti(s[x & 63]);
    }
}



/*
 * This function draws at GBA resoulution (ie. not pixel doubled)
 * so the st bar and menus don't look like garbage.
 */

void V_DrawPatch(int x, int y, int scrn, const patch_t* patch)
{
    /* UI patches (menu items, the DOOM logo, fonts, thermometer, intermission
     * numbers) are small — downsampling 320px into the 128-word framebuffer drops
     * most of their columns and garbles them. Draw those at native 1:1 full
     * resolution. Their 320x200-space coords are used directly (NOT scaled down):
     * the menu layout lives in the left/middle of 320, so at native size on the
     * 256px screen it lands centred — e.g. M_DOOM (94, w61, loff -3) spans 97..158,
     * centre 127.5 ~ screen centre 128. Scaling the position instead pushed
     * everything left. A patch exactly the screen width (256px, e.g. the PC-FX
     * TITLEPIC) also draws native 1:1; only wider art (a 320px intermission map)
     * is stretched to fill. */
    if (SHORT(patch->width) <= (SCREENWIDTH * 2))
    {
        V_DrawPatchFull(x, y, patch);
        return;
    }

    y -= SHORT(patch->topoffset);
    x -= SHORT(patch->leftoffset);

    const int patch_width  = SHORT(patch->width);
    const int patch_height = SHORT(patch->height);

    (void)scrn;
    int   col = 0;

    /* Scale 320x200 patch space onto the 120x160 logical (fat-pixel) framebuffer;
     * each logical pixel is one KRAM word written through the KING data port —
     * no system-RAM buffer, no copy. */
    const int   DX  = (SCREENWIDTH<<FRACBITS) / 320;
    const int   DXI = (320<<FRACBITS) / SCREENWIDTH;
    const int   DY  = ((SCREENHEIGHT<<FRACBITS)+(FRACUNIT-1)) / 200;
    const int   DYI = (200<<FRACBITS) / SCREENHEIGHT;

    const int left = ( x * DX ) >> FRACBITS;
    const int right =  ((x + patch_width) *  DX) >> FRACBITS;
    const int bottom = ((y + patch_height) * DY) >> FRACBITS;

    for (int dc_x=left; dc_x<right; dc_x++, col+=DXI)
    {
        int colindex = (col>>FRACBITS);

        if(dc_x < 0)
            continue;

        if (dc_x >= SCREENWIDTH)
            break;

        const column_t* column = (const column_t *)((const byte*)patch + LONG(patch->columnofs[colindex]));

        // step through the posts in a column
        while (column->topdelta != 0xff)
        {
            const byte* source = (const byte*)column + 3;
            const int topdelta = column->topdelta;

            int dc_yl = (((y + topdelta) * DY) >> FRACBITS);
            int dc_yh = (((y + topdelta + column->length) * DY) >> FRACBITS);

            if ((dc_yl >= SCREENHEIGHT) || (dc_yl > bottom))
                break;

            int count = (dc_yh - dc_yl);

            const fixed_t fracstep = DYI;
            fixed_t frac = 0;

            // clip the top of the scaled post, then draw the rest as a KRAM column
            while (count > 0 && dc_yl < 0) { frac += fracstep; dc_yl++; count--; }
            if (count > 0 && dc_yl < SCREENHEIGHT)
            {
                if (dc_yl + count > SCREENHEIGHT) count = SCREENHEIGHT - dc_yl;
                FB_col((unsigned)dc_x, (unsigned)dc_yl);
                while (count--)
                {
                    FB_puti(source[frac >> FRACBITS]);
                    frac += fracstep;
                }
            }

            column = (const column_t *)((const byte *)column + column->length + 4 );
        }
    }
}


/* V_DrawPatchFull — draw a masked patch at NATIVE 1:1 full resolution.
 *
 * V_DrawPatch (VPT_STRETCH) squeezes 320px into the 128-word framebuffer, so it
 * samples only ~40% of the source columns and doubles them (fat pixels) — which
 * shreds small text/UI patches into an unreadable, garbled mess. This draws every
 * source pixel to its own screen column instead. Since KING packs two 8bpp pixels
 * per 16-bit KRAM word (BIG-ENDIAN: high byte = left/even column, low byte =
 * right/odd column), a single-column write must preserve the neighbour column, so
 * we read-modify-write: read a post's words down the column, patch our byte, and
 * write them back. x/y are in native SCREEN space (256x240). Masked posts leave
 * the background untouched.
 */
void V_DrawPatchFull(int x, int y, const patch_t* patch)
{
    y -= SHORT(patch->topoffset);
    x -= SHORT(patch->leftoffset);

    const int width = SHORT(patch->width);

    for (int col = 0; col < width; col++)
    {
        const int sx = x + col;
        if (sx < 0 || sx >= (SCREENWIDTH * 2))          /* 256 screen px */
            continue;

        const unsigned wx = (unsigned)sx >> 1;          /* KRAM word column */
        const int      hi = !(sx & 1);                  /* even col -> high byte */

        const column_t* post =
            (const column_t*)((const byte*)patch + LONG(patch->columnofs[col]));

        while (post->topdelta != 0xff)
        {
            const byte* src = (const byte*)post + 3;
            int sy = y + post->topdelta;
            int n  = post->length;

            while (n > 0 && sy < 0) { src++; sy++; n--; }   /* clip top */
            if (n > 0 && sy < SCREENHEIGHT)
            {
                if (sy + n > SCREENHEIGHT) n = SCREENHEIGHT - sy;

                const uint32_t addr =
                    g_pcfx_fb_base + (unsigned)sy * KFB_ROW_WORDS + wx;
                uint16_t buf[MAX_SCREENHEIGHT];

                king_kram_set_read(addr, (int)KFB_ROW_WORDS);
                for (int i = 0; i < n; i++) buf[i] = read_kram();

                if (hi)
                    for (int i = 0; i < n; i++)
                        buf[i] = (uint16_t)((buf[i] & 0x00FF) | ((uint16_t)src[i] << 8));
                else
                    for (int i = 0; i < n; i++)
                        buf[i] = (uint16_t)((buf[i] & 0xFF00) | src[i]);

                king_kram_set_cursor(addr, (int)KFB_ROW_WORDS);
                for (int i = 0; i < n; i++) write_kram(buf[i]);
            }

            post = (const column_t*)((const byte*)post + post->length + 4);
        }
    }
}

// CPhipps - some simple, useful wrappers for that function, for drawing patches from wads

// CPhipps - GNU C only suppresses generating a copy of a function if it is
// static inline; other compilers have different behaviour.
// This inline is _only_ for the function below

void V_DrawNumPatch(int x, int y, int scrn, int lump,
         int cm, enum patch_translation_e flags)
{
    V_DrawPatch(x, y, scrn, W_CacheLumpNum(lump));
}

//
// V_SetPalette
//
// CPhipps - New function to set the palette to palette number pal.
// Handles loading of PLAYPAL and calls I_SetPalette

void V_SetPalette(int pal)
{
	I_SetPalette(pal);
}

//Colour corrected PLAYPAL lumps ~ Kippykip
void V_SetPalLump(int index)
{
    /* There is only one PLAYPAL in the IWAD; the per-gamma PLAYPAL1..5 lumps do
     * not exist here. GAMMA BOOST is now applied to the display palette at upload
     * time (see I_UploadNewPalette), so always cache the base PLAYPAL and let the
     * caller's V_SetPalette(0) re-upload it through the current gamma. */
    (void)index;
    _g->pallete_lump = W_CacheLumpName("PLAYPAL");
}

//
// V_FillRect
//
// CPhipps - New function to fill a rectangle with a given colour
void V_FillRect(int x, int y, int width, int height, byte colour)
{
    const unsigned short w = (unsigned short)(colour | (colour << 8));

    for (int row = 0; row < height; row++)
    {
        int yy = y + row;
        if (yy < 0 || yy >= SCREENHEIGHT)
            continue;

        int x0 = x, cnt = width;
        if (x0 < 0) { cnt += x0; x0 = 0; }
        if (x0 + cnt > SCREENWIDTH) cnt = SCREENWIDTH - x0;
        if (cnt <= 0)
            continue;

        FB_span((unsigned)x0, (unsigned)yy);
        while (cnt--) FB_put(w);
    }
}



static void V_PlotPixel(int x, int y, int color)
{
    if ((unsigned)x >= (unsigned)SCREENWIDTH || (unsigned)y >= (unsigned)SCREENHEIGHT)
        return;

    FB_at(g_pcfx_fb_base + FB_YOFFSET(y) + (unsigned)x, 1);
    FB_puti((unsigned)color);
}

//
// WRAP_V_DrawLine()
//
// Draw a line in the frame buffer.
// Classic Bresenham w/ whatever optimizations needed for speed
//
// Passed the frame coordinates of line, and the color to be drawn
// Returns nothing
//
void V_DrawLine(fline_t* fl, int color)
{
    int x0 = fl->a.x;
    int x1 = fl->b.x;

    int y0 = fl->a.y;
    int y1 = fl->b.y;

    int dx =  D_abs(x1-x0);
    int sx = x0<x1 ? 1 : -1;

    int dy = -D_abs(y1-y0);
    int sy = y0<y1 ? 1 : -1;

    int err = dx + dy;

    while(true)
    {
        V_PlotPixel(x0, y0, color);

        if (x0==x1 && y0==y1)
            break;

        int e2 = 2*err;

        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}
