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
 *      The status bar widget code.
 *
 *-----------------------------------------------------------------------------*/

#include "doomdef.h"
#include "doomstat.h"
#include "v_video.h"
#include "w_wad.h"
#include "st_stuff.h"
#include "st_lib.h"
#include "r_main.h"
#include "lprintf.h"
#include "global_data.h"
#include "pcfx_text.h"    /* status bar now uploaded as VDC BG tiles */

#include "gba_functions.h"

//
// STlib_init()
//
void STlib_init(void)
{
  // cph - no longer hold STMINUS pointer
}

//
// STlib_initNum()
//
// Initializes an st_number_t widget
//
// Passed the widget, its position, the patches for the digits, a pointer
// to the value displayed, a pointer to the on/off control, and the width
// Returns nothing
//
void STlib_initNum
(st_number_t* n,
  int x,
  int y,
  const patch_t **pl,
  int* num,
  boolean* on,
  int     width )
{
  n->x  = x;
  n->y  = y;
  n->oldnum = 0;
  n->width  = width;
  n->num  = num;
  n->on = on;
  n->p  = pl;
}

/*
 * STlib_drawNum()
 *
 * A fairly efficient way to draw a number based on differences from the
 * old number.
 *
 * Passed a st_number_t widget, a color range for output, and a flag
 * indicating whether refresh is needed.
 * Returns nothing
 *
 * jff 2/16/98 add color translation to digit output
 * cphipps 10/99 - const pointer to colour trans table, made function static
 */
static void STlib_drawNum
( st_number_t*  n,
  int cm,
  boolean refresh )
{

  int   numdigits = n->width;
  int   num = *n->num;

  int   w = SHORT(n->p[0]->width);
  int   x = n->x;

  int   neg;

  // CPhipps - compact some code, use num instead of *n->num
  if ((neg = (n->oldnum = num) < 0))
  {
    if (numdigits == 2 && num < -9)
      num = -9;
    else if (numdigits == 3 && num < -99)
      num = -99;

    num = -num;
  }

  // clear the area
  x = n->x - numdigits*w;

  // if non-number, do not draw it
  if (num == 1994)
    return;

  x = n->x;

  //jff 2/16/98 add color translation to digit output
  // in the special case of 0, you draw 0
  if (!num)
    // CPhipps - patch drawing updated, reformatted
    HUD_DrawPatch(x - w, n->y, n->p[0]);

  // draw the new number
  //jff 2/16/98 add color translation to digit output
  while (num && numdigits--)
  {
    // CPhipps - patch drawing updated, reformatted
    x -= w;
    HUD_DrawPatch(x, n->y, n->p[num % 10]);
    num /= 10;
  }
}

/*
 * STlib_updateNum()
 *
 * Draws a number conditionally based on the widget's enable
 *
 * Passed a number widget, the output color range, and a refresh flag
 * Returns nothing
 *
 * jff 2/16/98 add color translation to digit output
 * cphipps 10/99 - make that pointer const
 */
void STlib_updateNum
( st_number_t*    n,
  int cm,
  boolean   refresh )
{
  if (*n->on) STlib_drawNum(n, cm, refresh);
}

//
// STlib_initPercent()
//
// Initialize a st_percent_t number with percent sign widget
//
// Passed a st_percent_t widget, the position, the digit patches, a pointer
// to the number to display, a pointer to the enable flag, and patch
// for the percent sign.
// Returns nothing.
//
void STlib_initPercent
(st_percent_t* p,
  int x,
  int y,
  const patch_t** pl,
  int* num,
  boolean* on,
  const patch_t *percent )
{
  STlib_initNum(&p->n, x, y, pl, num, on, 3);
  p->p = percent;
}

/*
 * STlib_updatePercent()
 *
 * Draws a number/percent conditionally based on the widget's enable
 *
 * Passed a precent widget, the output color range, and a refresh flag
 * Returns nothing
 *
 * jff 2/16/98 add color translation to digit output
 * cphipps - const for pointer to the colour translation table
 */

void STlib_updatePercent(st_percent_t* per, int cm, int refresh)
{
    STlib_updateNum(&per->n, cm, refresh);
    //V_DrawPatchNoScale(per->n.x, per->n.y, per->p); - Percentage is in the GBA Doom II Hud graphic ~Kippykip
}

//
// STlib_initMultIcon()
//
// Initialize a st_multicon_t widget, used for a multigraphic display
// like the status bar's keys.
//
// Passed a st_multicon_t widget, the position, the graphic patches, a pointer
// to the numbers representing what to display, and pointer to the enable flag
// Returns nothing.
//
void STlib_initMultIcon
(st_multicon_t* i,
  int x,
  int y,
  const patch_t **il,
  int* inum,
  boolean* on )
{
  i->x  = x;
  i->y  = y;
  i->oldinum  = -1;
  i->inum = inum;
  i->on = on;
  i->p  = il;
}

//
// STlib_updateMultIcon()
//
// Draw a st_multicon_t widget, used for a multigraphic display
// like the status bar's keys. Displays each when the control
// numbers change or refresh is true
//
// Passed a st_multicon_t widget, and a refresh flag
// Returns nothing.
//
void STlib_updateMultIcon
( st_multicon_t*  mi,
  boolean   refresh )
{
    if(!mi->p)
        return;

    if (*mi->inum != -1)  // killough 2/16/98: redraw only if != -1
		HUD_DrawPatch(mi->x, mi->y, mi->p[*mi->inum]);

    mi->oldinum = *mi->inum;

}

//
// STlib_initBinIcon()
//
// Initialize a st_binicon_t widget, used for a multinumber display
// like the status bar's weapons, that are present or not.
//
// Passed a st_binicon_t widget, the position, the digit patches, a pointer
// to the flags representing what is displayed, and pointer to the enable flag
// Returns nothing.
//
void STlib_initBinIcon
( st_binicon_t* b,
  int x,
  int y,
  const patch_t* i,
  boolean* val,
  boolean* on )
{
  b->x  = x;
  b->y  = y;
  b->oldval = 0;
  b->val  = val;
  b->on = on;
  b->p  = i;
}

//
// STlib_updateBinIcon()
//
// DInitialize a st_binicon_t widget, used for a multinumber display
// like the status bar's weapons, that are present or not.
//
// Draw a st_binicon_t widget, used for a multinumber display
// like the status bar's weapons that are present or not. Displays each
// when the control flag changes or refresh is true
//
// Passed a st_binicon_t widget, and a refresh flag
// Returns nothing.
//
void STlib_updateBinIcon
( st_binicon_t*   bi,
  boolean   refresh )
{
    if (*bi->on && (bi->oldval != *bi->val || refresh))
    {
        if (*bi->val)
            HUD_DrawPatch(bi->x, bi->y, bi->p);

        bi->oldval = *bi->val;
    }
}

/* -------------------------------------------------------------------------
 * Full-resolution HUD compositor.
 *
 * The 3D view is drawn with fat pixels (1 logical column = 1 KRAM word = 2
 * screen px), but the status bar must be crisp: STBARFX is a full 256px-wide
 * image and its printed labels line up with native-size widget graphics. KING
 * only writes 16-bit KRAM words (2 screen px each), so we can't poke a single
 * screen pixel without clobbering its neighbour. Instead we composite the whole
 * bar — background + every widget — into a 256x32 RAM strip at full resolution,
 * then blit it to the bottom 32 KRAM rows once, packing two distinct pixels per
 * word (low byte = left pixel, high byte = right pixel).
 * ----------------------------------------------------------------------- */
#define HUD_W 256
#define HUD_H ST_HEIGHT
#define HUD_Y0 (SCREENHEIGHT - HUD_H)          /* top screen row of the bar */

static byte hud_strip[HUD_W * HUD_H] __attribute__((aligned(4)));

/* Composite one masked patch into the strip at full resolution. x is in 256px
 * bar space; y is the absolute screen row (converted to strip-local). Posts
 * that fall outside the strip are clipped; gaps keep the background. */
void HUD_DrawPatch(int x, int y, const patch_t *patch)
{
    y -= SHORT(patch->topoffset);
    x -= SHORT(patch->leftoffset);

    const int width = SHORT(patch->width);

    for (int col = 0; col < width; col++)
    {
        const int sx = x + col;
        if (sx < 0 || sx >= HUD_W)
            continue;

        const column_t *column =
            (const column_t *)((const byte *)patch + LONG(patch->columnofs[col]));

        while (column->topdelta != 0xff)
        {
            const byte *source = (const byte *)column + 3;
            int   sy    = y + column->topdelta;        /* absolute screen row */
            int   count = column->length;

            while (count-- > 0)
            {
                const int ly = sy - HUD_Y0;            /* strip-local row */
                if (ly >= 0 && ly < HUD_H)
                    hud_strip[ly * HUD_W + sx] = *source;
                source++;
                sy++;
            }

            column = (const column_t *)((const byte *)column + column->length + 4);
        }
    }
}

/* Blit the composited strip to the bottom HUD_H KRAM rows of BOTH framebuffer
 * pages (2 px per word).
 *
 * KING 8bpp packs two pixels per 16-bit KRAM word BIG-ENDIAN: the HIGH byte is
 * the left (even) screen column, the LOW byte is the right (odd) column (see the
 * emulator's DRAWBG8x1_256: target[0]=word>>8, target[1]=word&0xFF). So the even
 * pixel s[2x] goes in the high byte and the odd pixel s[2x+1] in the low byte;
 * packing them the other way swaps every adjacent pair and scrambles the bar.
 *
 * We write the HUD into BOTH pages (front and back), not just the current render
 * page. The framebuffer is double-buffered with a hardware page flip, and the
 * status bar is only redrawn when a widget changes; a single-page blit would land
 * on one page and leave the other showing the previous frame's bottom rows (the
 * title/menu remnant), so page-flipping made the bar flicker against stale content
 * until an unrelated widget change happened to refresh the other page. Painting
 * both pages here keeps the bar identical on whichever page is shown. */
void HUD_BlitStrip(void)
{
    /* PC-FX: the status bar now lives on the VDC background-tile layer (platform/
     * pcfx_text.c), composited over the KING scene by hardware at full 256px res and
     * sharing the scene palette (so it fades with damage/pickup flashes for free). Push
     * the freshly-composited 256x32 strip into the HUD tile CG; pcfx_text_hud_place()
     * (called every frame by ST_Drawer) stamps the BAT cells. Called only on a widget
     * change (ST_doRefresh), so this re-upload is not per-frame. */
    pcfx_text_hud_update(hud_strip);
}

void ST_refreshBackground(void)
{
    /* Reset the strip to the STBARFX background; widgets composite on top, then
     * ST_doRefresh() blits the whole strip. STBARFX is a raw 256x32 8bpp image
     * (== screen width) whose palette is PLAYPAL, so its bytes are copied 1:1. */
    if (!_g->st_statusbaron)
        return;

    memcpy(hud_strip, (const byte *)_g->stbarbg, HUD_W * HUD_H);
}
