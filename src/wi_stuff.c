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
 *  Intermission screens.
 *
 *-----------------------------------------------------------------------------
 */

#include "doomstat.h"
#include "m_random.h"
#include "w_wad.h"
#include "g_game.h"
#include "r_main.h"
#include "v_video.h"
#include "wi_stuff.h"
#include "s_sound.h"
#include "sounds.h"
#include "lprintf.h"  // jff 08/03/98 - declaration of lprintf
#include "r_draw.h"
#include "pcfx_cdasset.h"  // CD-streamed intermission background (not resident)
#include "pcfx_text.h"     // VDC BG font-tile overlay: intermission text like the title/menu

/* platform/i_sound_pcfx.c. Declared here rather than pulling the sound backend's
 * internals into portable engine code, as i_video.c does for pcfx_cdda_pump. */
void pcfx_cdda_mute_for_dma(void);
#include <string.h>
#include <ctype.h>

#include "global_data.h"

//
// Data needed to add patches to full screen intermission pics.
// Patches are statistics messages, and animations.
// Loads of by-pixel layout and placement, offsets etc.
//

//
// Different vetween registered DOOM (1994) and
//  Ultimate DOOM - Final edition (retail, 1995?).
// This is supposedly ignored for commercial
//  release (aka DOOM II), which had 34 maps
//  in one episode. So there.
#define NUMEPISODES 4
#define NUMMAPS     9


// Not used
// in tics
//U #define PAUSELEN    (TICRATE*2)
//U #define SCORESTEP    100
//U #define ANIMPERIOD    32
// pixel distance from "(YOU)" to "PLAYER N"
//U #define STARDIST  10
//U #define WK 1


// GLOBAL LOCATIONS
#define WI_TITLEY      2
#define WI_SPACINGY   33

// SINGLE-PLAYER STUFF
#define SP_STATSX     50
#define SP_STATSY     50

#define SP_TIMEX      8
// proff/nicolas 09/20/98 -- changed for hi-res
#define SP_TIMEY      160
//#define SP_TIMEY      (SCREENHEIGHT-32)


// NET GAME STUFF
#define NG_STATSY     50
#define NG_STATSX     (32 + V_NamePatchWidth(star)/2 + 32*!dofrags)

#define NG_SPACINGX   64


// Used to display the frags matrix at endgame
// DEATHMATCH STUFF
#define DM_MATRIXX    42
#define DM_MATRIXY    68

#define DM_SPACINGX   40

#define DM_TOTALSX   269

#define DM_KILLERSX   10
#define DM_KILLERSY  100
#define DM_VICTIMSX    5
#define DM_VICTIMSY   50

typedef struct
{
  int   x;       // x/y coordinate pair structure
  int   y;
} point_t;

static const point_t lnodes[NUMEPISODES][NUMMAPS] =
{
  // Episode 0 World Map
  {
    { 185, 164 }, // location of level 0 (CJ)
    { 148, 143 }, // location of level 1 (CJ)
    { 69, 122 },  // location of level 2 (CJ)
    { 209, 102 }, // location of level 3 (CJ)
    { 116, 89 },  // location of level 4 (CJ)
    { 166, 55 },  // location of level 5 (CJ)
    { 71, 56 },   // location of level 6 (CJ)
    { 135, 29 },  // location of level 7 (CJ)
    { 71, 24 }    // location of level 8 (CJ)
  },

  // Episode 1 World Map should go here
  {
    { 254, 25 },  // location of level 0 (CJ)
    { 97, 50 },   // location of level 1 (CJ)
    { 188, 64 },  // location of level 2 (CJ)
    { 128, 78 },  // location of level 3 (CJ)
    { 214, 92 },  // location of level 4 (CJ)
    { 133, 130 }, // location of level 5 (CJ)
    { 208, 136 }, // location of level 6 (CJ)
    { 148, 140 }, // location of level 7 (CJ)
    { 235, 158 }  // location of level 8 (CJ)
  },

  // Episode 2 World Map should go here
  {
    { 156, 168 }, // location of level 0 (CJ)
    { 48, 154 },  // location of level 1 (CJ)
    { 174, 95 },  // location of level 2 (CJ)
    { 265, 75 },  // location of level 3 (CJ)
    { 130, 48 },  // location of level 4 (CJ)
    { 279, 23 },  // location of level 5 (CJ)
    { 198, 48 },  // location of level 6 (CJ)
    { 140, 25 },  // location of level 7 (CJ)
    { 281, 136 }  // location of level 8 (CJ)
  }
};




//
// GENERAL DATA
//

//
// Locally used stuff.
//
#define FB 0


// States for single-player
#define SP_KILLS    0
#define SP_ITEMS    2
#define SP_SECRET   4
#define SP_FRAGS    6
#define SP_TIME     8
#define SP_PAR      ST_TIME

#define SP_PAUSE    1

// PC-FX: the intermission is authored in Doom's 320x200 space, but the native
// screen is 256x240 and the full-screen background (WIMAP0/INTERPIC) is stretched
// 320->256 / 200->240 by V_DrawPatch's VPT_STRETCH path. Scale every widget ANCHOR
// the same way so labels, stats and the "you are here" splats stay aligned with
// that stretched map. The patches themselves are still drawn at native size (small
// text must not be down-sampled), so only the placement coordinate is scaled.
#define WI_SX(x)  (((x) * 256) / 320)
#define WI_SY(y)  (((y) * 240) / 200)

// in seconds
#define SHOWNEXTLOCDELAY  4
//#define SHOWLASTLOCDELAY  SHOWNEXTLOCDELAY

//
//  GRAPHICS
//

// You Are Here graphic
static const char* const yah[2] = { "WIURH0", "WIURH1" };

// splat
static const char* const splat = "WISPLAT";

// %, : graphics
static const char percent[] = {"WIPCNT"};
static const char colon[] = {"WICOLON"};



// minus sign
static const char wiminus[] = {"WIMINUS"};

// "Finished!" graphics
static const char finished[] = {"WIF"};

// "Entering" graphic
static const char entering[] = {"WIENTER"};

// "secret"
static const char sp_secret[] = {"WISCRT2"};

// "Kills", "Scrt", "Items", "Frags"
static const char kills[] = {"WIOSTK"};
static const char items[] = {"WIOSTI"};

// Time sucks.
static const char time1[] = {"WITIME"};
static const char par[] = {"WIPAR"};
static const char sucks[] = {"WISUCKS"};

// "Total", your face, your dead face
static const char total[] = {"WIMSTT"};


//
// CODE
//

static void WI_endNetgameStats(void);
#define WI_endStats WI_endNetgameStats


// ====================================================================
// CPhipps - WI_endNetgameStats
// Purpose: Clean up coop game stats
// Args:    none
// Returns: void
//
static void WI_endNetgameStats(void)
{
    _g->cnt_kills = -1;
    _g->cnt_secret = -1;
    _g->cnt_items = -1;
}


/* ====================================================================
 * WI_levelNameLump
 * Purpore: Returns the name of the graphic lump containing the name of
 *          the given level.
 * Args:    Episode and level, and buffer (must by 9 chars) to write to
 * Returns: void
 */
void WI_levelNameLump(int epis, int map, char* buf)
{
  if (_g->gamemode == commercial)
  {
    sprintf(buf, "CWILV%2.2d", map);
  }
  else
  {
    sprintf(buf, "WILV%d%d", epis, map);
  }
}

// ====================================================================
// WI_slamBackground
// Purpose: Put the full-screen background up prior to patches
// Args:    none
// Returns: void
//
static void WI_slamBackground(void)
{
  char  name[9];  // limited to 8 characters

  if (_g->gamemode == commercial || (_g->gamemode == retail && _g->wbs->epsd == 3))
    strcpy(name, "INTERPIC");
  else
    sprintf(name, "WIMAP%d", _g->wbs->epsd);

  // background. Served from the resident KRAM cache (CD DMA at most once per
  // picture, KRAM->KRAM copy per repaint — platform/pcfx_cdasset.c); fall back
  // to the baked patch if it isn't a CD asset.
  if (!pcfx_cd_background(name))
    V_DrawNamePatch(0, 0, FB, name, CR_DEFAULT, VPT_STRETCH);
}


// ====================================================================
// WI_Responder
// Purpose: Draw animations on intermission background screen
// Args:    ev    -- event pointer, not actually used here.
// Returns: False -- dummy routine
//
// The ticker is used to detect keys
//  because of timing issues in netgames.
boolean WI_Responder(event_t* ev)
{
  return false;
}


// ====================================================================
// WI_drawLF
// Purpose: Draw the "Finished" level name before showing stats
// Args:    none
// Returns: void
//
// ====================================================================
// PC-FX: intermission text on the VDC BG-tile overlay (like the title menu &
// finale), NOT the framebuffer graphic patches (WILV/WIF/WINUM...). The map
// background + its animated spots/markers still paint the KING framebuffer every
// frame (WI_slamBackground / WI_drawAnimatedBack); this text composites on top by
// the HuC6270 and fades with the scene for free. hu_font has no lowercase, so
// everything is uppercased (as F_TextWrite / M_WriteText do). 32x30 cell grid.
// ====================================================================
static void WI_text(int col, int row, const char *s)
{
  for (; *s && col < PCFX_TEXT_COLS; s++, col++)
    pcfx_text_putc(col, row, toupper((unsigned char)*s));
}
static int WI_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void WI_textCenter(int row, const char *s)
{
  int col = (PCFX_TEXT_COLS - WI_slen(s)) / 2;
  WI_text(col < 0 ? 0 : col, row, s);
}
static void WI_textRight(int endcol, int row, const char *s)
{
  int col = endcol - WI_slen(s);
  WI_text(col < 0 ? 0 : col, row, s);
}
// The level's display name without the "ExMy: " prefix (e.g. "HANGAR").
static const char *WI_nameText(int epsd, int map)
{
  extern const char *const mapnames[];
  extern const char *const mapnames2[];
  const char *s = (_g->gamemode == commercial) ? mapnames2[map]
                                               : mapnames[epsd * 9 + map];
  const char *c = s;
  while (*c && *c != ':') c++;
  if (*c == ':') { c++; while (*c == ' ') c++; return c; }
  return s;
}
// Format a time (seconds) as M:SS, or "SUCKS" past 100 minutes (the classic gag).
static void WI_fmtTime(char *buf, int t)
{
  if (t < 0) { buf[0] = 0; return; }
  if (t >= 100 * 60) { strcpy(buf, "SUCKS"); return; }
  sprintf(buf, "%d:%02d", t / 60, t % 60);
}

void WI_drawLF(void)
{
  // level name, then "FINISHED"
  WI_textCenter(1, WI_nameText(_g->wbs->epsd, _g->wbs->last));
  WI_textCenter(3, "FINISHED");
}


// ====================================================================
// WI_drawEL
// Purpose: Draw introductory "Entering" and level name
// Args:    none
// Returns: void
//
void WI_drawEL(void)
{
  // "ENTERING", then the next level's name
  WI_textCenter(1, "ENTERING");
  WI_textCenter(3, WI_nameText(_g->wbs->epsd, _g->wbs->next));
}


/* ====================================================================
 * WI_drawOnLnode
 * Purpose: Draw patches at a location based on episode/map
 * Args:    n   -- index to map# within episode
 *          c[] -- array of names of patches to be drawn
 * Returns: void
 */
void
WI_drawOnLnode  // draw stuff at a location by episode/map#
( int   n,
  const char* const c[] )
{
  int   i;
  boolean fits = false;

  i = 0;
  do
  {
    int            left;
    int            top;
    int            right;
    int            bottom;
    const patch_t* patch = W_CacheLumpName(c[i]);

    left = lnodes[_g->wbs->epsd][n].x - SHORT(patch->leftoffset);
    top = lnodes[_g->wbs->epsd][n].y - SHORT(patch->topoffset);
    right = left + SHORT(patch->width);
    bottom = top + SHORT(patch->height);

    if (left >= 0
       && right < 320
       && top >= 0
       && bottom < 200)
    {
      fits = true;
    }
    else
    {
      i++;
    }
  } while (!fits && i!=2);

  if (fits && i<2)
  {
    // CPhipps - patch drawing updated
    V_DrawNamePatch(WI_SX(lnodes[_g->wbs->epsd][n].x), WI_SY(lnodes[_g->wbs->epsd][n].y),
       FB, c[i], CR_DEFAULT, VPT_STRETCH);
  }
  else
  {
    // DEBUG
    //jff 8/3/98 use logical output routine
    lprintf(LO_DEBUG,"Could not place patch on level %d", n+1);
  }
}


// ====================================================================
// WI_initAnimatedBack
// Purpose: Initialize pointers and styles for background animation
// Args:    none
// Returns: void
//
void WI_initAnimatedBack(void)
{

}


// ====================================================================
// WI_updateAnimatedBack
// Purpose: Figure out what animation we do on this iteration
// Args:    none
// Returns: void
//
void WI_updateAnimatedBack(void)
{

}


// ====================================================================
// WI_drawAnimatedBack
// Purpose: Actually do the animation (whew!)
// Args:    none
// Returns: void
//
void WI_drawAnimatedBack(void)
{

}

// PC-FX two-buffer intermission (see platform/pcfx_cdasset.h pcfx_im_*): the KING
// framebuffer only ever holds background + splats + the "you are here" marker (all
// the text rides the VDC overlay), and the only thing that changes frame-to-frame
// is the YAH blink. So we pre-render BOTH framebuffers once per screen -- buffer 0
// without YAH, buffer 1 with it -- and each frame just page-select which one is
// shown. No RAM staging buffer (the old ~61 KB s_buf that fragmented the zone and
// froze the game), no per-frame repaint. s_wi_fb_dirty forces a re-render when the
// screen (splat set) changes.
static int s_wi_fb_dirty = 1;

static void WI_renderBuffers(int with_locs)
{
    for (int buf = 0; buf < 2; buf++)
    {
        pcfx_im_draw_to(buf);
        WI_slamBackground();                 // raw bg -> this framebuffer (CD->KRAM DMA)
        if (with_locs && _g->gamemode != commercial && _g->wbs->epsd <= 2)
        {
            int i, last = (_g->wbs->last == 8) ? _g->wbs->next - 1 : _g->wbs->last;
            for (i = 0; i <= last; i++)       // splats on completed levels
                WI_drawOnLnode(i, &splat);
            if (_g->wbs->didsecret)
                WI_drawOnLnode(8, &splat);
            if (buf == 1)                     // "you are here" only in buffer 1
                WI_drawOnLnode(_g->wbs->next, yah);
        }
    }

}


// ====================================================================
// WI_drawNum
// Purpose: Draws a number.  If digits > 0, then use that many digits
//          minimum, otherwise only use as many as necessary
// Args:    x, y   -- location
//          n      -- the number to be drawn
//          digits -- number of digits minimum or zero
// Returns: new x position after drawing (note we are going to the left)
// CPhipps - static
static int WI_drawNum (int x, int y, int n, int digits)
{
  int   fontwidth = SHORT(_g->num[0]->width);
  int   neg;
  int   temp;

  if (digits < 0)
  {
    if (!n)
    {
      // make variable-length zeros 1 digit long
      digits = 1;
    }
    else
    {
      // figure out # of digits in #
      digits = 0;
      temp = n;

      while (temp)
      {
        temp /= 10;
        digits++;
      }
    }
  }

  neg = n < 0;
  if (neg)
    n = -n;

  // if non-number, do not draw it
  if (n == 1994)
    return 0;

  // draw the new number
  while (digits--)
  {
    x -= fontwidth;
    // CPhipps - patch drawing updated
    V_DrawPatch(x, y, FB, _g->num[ n % 10 ]);
    n /= 10;
  }

  // draw a minus sign if necessary
  if (neg)
    // CPhipps - patch drawing updated
    V_DrawNamePatch(x-=8, y, FB, wiminus, CR_DEFAULT, VPT_STRETCH);

  return x;
}


// ====================================================================
// WI_drawPercent
// Purpose: Draws a percentage, really just a call to WI_drawNum
//          after putting a percent sign out there
// Args:    x, y   -- location
//          p      -- the percentage value to be drawn, no negatives
// Returns: void
// CPhipps - static
static void WI_drawPercent(int x, int y, int p)
{
  if (p < 0)
    return;

  // CPhipps - patch drawing updated
  V_DrawNamePatch(x, y, FB, percent, CR_DEFAULT, VPT_STRETCH);
  WI_drawNum(x, y, p, -1);
}


// ====================================================================
// WI_drawTime
// Purpose: Draws the level completion time or par time, or "Sucks"
//          if 1 hour or more
// Args:    x, y   -- location
//          t      -- the time value to be drawn
// Returns: void
//
// CPhipps - static
//         - largely rewritten to display hours and use slightly better algorithm

static void WI_drawTime(int x, int y, int t)
{
  int   n;

  if (t<0)
    return;

  if (t < 100*60*60)
    for(;;) {
      n = t % 60;
      t /= 60;
      x = WI_drawNum(x, y, n, (t || n>9) ? 2 : 1) - V_NamePatchWidth(colon);

      // draw
      if (t)
  // CPhipps - patch drawing updated
        V_DrawNamePatch(x, y, FB, colon, CR_DEFAULT, VPT_STRETCH);
      else break;
    }
  else // "sucks" (maybe should be "addicted", even I've never had a 100 hour game ;)
    V_DrawNamePatch(x - V_NamePatchWidth(sucks),
        y, FB, sucks, CR_DEFAULT, VPT_STRETCH);
}


// ====================================================================
// WI_End
// Purpose: Unloads data structures (inverse of WI_Start)
// Args:    none
// Returns: void
//
void WI_End(void)
{
    WI_endStats();
    pcfx_im_end();         // hand back to the normal double-buffer page flip
}


// ====================================================================
// WI_initNoState
// Purpose: Clear state, ready for end of level activity
// Args:    none
// Returns: void
//
void WI_initNoState(void)
{
  _g->state = NoState;
  s_wi_fb_dirty = 1;
  _g->acceleratestage = 0;
  _g->cnt = 10;
}


// ====================================================================
// WI_drawTimeStats
// Purpose: Put the times on the screen
// Args:    time, total time, par time, in seconds
// Returns: void
//
// cph - pulled from WI_drawStats below

static void WI_drawTimeStats(int cnt_time, int cnt_total_time, int cnt_par)
{
  V_DrawNamePatch(WI_SX(SP_TIMEX), WI_SY(SP_TIMEY), FB, time1, CR_DEFAULT, VPT_STRETCH);
  WI_drawTime(WI_SX(320/2 - SP_TIMEX), WI_SY(SP_TIMEY), cnt_time);

  V_DrawNamePatch(WI_SX(SP_TIMEX), WI_SY((SP_TIMEY+200)/2), FB, total, CR_DEFAULT, VPT_STRETCH);
  WI_drawTime(WI_SX(320/2 - SP_TIMEX), WI_SY((SP_TIMEY+200)/2), cnt_total_time);

  // Ty 04/11/98: redid logic: should skip only if with pwad but
  // without deh patch
  // killough 2/22/98: skip drawing par times on pwads
  // Ty 03/17/98: unless pars changed with deh patch

    if (_g->wbs->epsd < 3)
    {
      V_DrawNamePatch(WI_SX(320/2 + SP_TIMEX), WI_SY(SP_TIMEY), FB, par, CR_DEFAULT, VPT_STRETCH);
      WI_drawTime(WI_SX(320 - SP_TIMEX), WI_SY(SP_TIMEY), cnt_par);
    }

}

// ====================================================================
// WI_updateNoState
// Purpose: Cycle until end of level activity is done
// Args:    none
// Returns: void
//
void WI_updateNoState(void)
{

  WI_updateAnimatedBack();

  if (!--_g->cnt)
    G_WorldDone();
}

// ====================================================================
// WI_initShowNextLoc
// Purpose: Prepare to show the next level's location
// Args:    none
// Returns: void
//
void WI_initShowNextLoc(void)
{
  if ((_g->gamemode != commercial) && (_g->gamemap == 8)) {
    G_WorldDone();
    return;
  }

  _g->state = ShowNextLoc;
  s_wi_fb_dirty = 1;
  _g->acceleratestage = 0;

  // e6y: That was pretty easy - only a HEX editor and luck
  // There is no more desync on ddt-tas.zip\e4tux231.lmp
  // --------- tasdoom.idb ---------
  // .text:00031194 loc_31194:      ; CODE XREF: WI_updateStats+3A9j
  // .text:00031194                 mov     ds:state, 1
  // .text:0003119E                 mov     ds:acceleratestage, 0
  // .text:000311A8                 mov     ds:cnt, 3Ch
  // nowhere no hide
    _g->cnt = SHOWNEXTLOCDELAY * TICRATE;

  WI_initAnimatedBack();
}


// ====================================================================
// WI_updateShowNextLoc
// Purpose: Prepare to show the next level's location
// Args:    none
// Returns: void
//
void WI_updateShowNextLoc(void)
{
  WI_updateAnimatedBack();

  if (!--_g->cnt || _g->acceleratestage)
    WI_initNoState();
  else
    _g->snl_pointeron = (_g->cnt & 31) < 20;
}


// ====================================================================
// WI_drawShowNextLoc
// Purpose: Show the next level's location on animated backgrounds
// Args:    none
// Returns: void
//
void WI_drawShowNextLoc(void)
{
    // Pre-render both framebuffers (bg + splats; buffer 1 also gets the YAH marker)
    // once per screen; the blink below is just a page-select. See WI_renderBuffers.
    if (s_wi_fb_dirty)
    {
        WI_renderBuffers(1);
        s_wi_fb_dirty = 0;
    }

    // draw flashing ptr: show buffer 1 (with "you are here") while the pointer is on
    pcfx_im_show((_g->gamemode != commercial && _g->wbs->epsd <= 2 && _g->snl_pointeron)
                 ? 1 : 0);

    // draws which level you are entering.. (VDC overlay text, every frame)
    if (_g->gamemode != commercial && _g->wbs->epsd > 2)
    {
        WI_drawEL();  // "Entering..." if not E1 or E2
        return;
    }
    if ( (_g->gamemode != commercial)
         || _g->wbs->next != 30)  // check for MAP30 end game
        WI_drawEL();
}

// ====================================================================
// WI_drawNoState
// Purpose: Draw the pointer and next location
// Args:    none
// Returns: void
//
void WI_drawNoState(void)
{
  _g->snl_pointeron = true;
  WI_drawShowNextLoc();
}

// ====================================================================
// WI_initStats
// Purpose: Get ready for single player stats
// Args:    none
// Returns: void
// Comment: Seems like we could do all these stats in a more generic
//          set of routines that weren't duplicated for dm, coop, sp
//



void WI_initStats(void)
{
  _g->state = StatCount;
  s_wi_fb_dirty = 1;
  _g->acceleratestage = 0;
  _g->sp_state = 1;

  _g->cnt_kills = -1;
  _g->cnt_secret = -1;
  _g->cnt_items = -1;

  _g->cnt_time = _g->cnt_par = _g->cnt_total_time = -1;
  _g->cnt_pause = TICRATE;

  WI_initAnimatedBack();
}

// ====================================================================
// WI_updateStats
// Purpose: Calculate solo stats
// Args:    none
// Returns: void
//
void WI_updateStats(void)
{
  WI_updateAnimatedBack();

  if (_g->acceleratestage && _g->sp_state != 10)
  {
    _g->acceleratestage = 0;
    _g->cnt_kills = (_g->plrs[0].skills * 100) / _g->wbs->maxkills;
    _g->cnt_items = (_g->plrs[0].sitems * 100) / _g->wbs->maxitems;

    // killough 2/22/98: Make secrets = 100% if maxsecret = 0:
    _g->cnt_secret = (_g->wbs->maxsecret ?
      (_g->plrs[0].ssecret * 100) / _g->wbs->maxsecret : 100);

    _g->cnt_total_time = _g->wbs->totaltimes / TICRATE;
    _g->cnt_time = _g->plrs[0].stime / TICRATE;
    _g->cnt_par = _g->wbs->partime / TICRATE;
    S_StartSound(0, sfx_barexp);
    _g->sp_state = 10;
  }

  if (_g->sp_state == 2)
  {
    _g->cnt_kills += 2;

    if (!(_g->bcnt&3))
      S_StartSound(0, sfx_pistol);

    if (_g->cnt_kills >= (_g->plrs[0].skills * 100) / _g->wbs->maxkills)
    {
      _g->cnt_kills = (_g->plrs[0].skills * 100) / _g->wbs->maxkills;
      S_StartSound(0, sfx_barexp);
      _g->sp_state++;
    }
  }
  else if (_g->sp_state == 4)
  {
    _g->cnt_items += 2;

    if (!(_g->bcnt&3))
      S_StartSound(0, sfx_pistol);

    if (_g->cnt_items >= (_g->plrs[0].sitems * 100) / _g->wbs->maxitems)
    {
      _g->cnt_items = (_g->plrs[0].sitems * 100) / _g->wbs->maxitems;
      S_StartSound(0, sfx_barexp);
      _g->sp_state++;
    }
  }
  else if (_g->sp_state == 6)
  {
    _g->cnt_secret += 2;

    if (!(_g->bcnt&3))
      S_StartSound(0, sfx_pistol);

    // killough 2/22/98: Make secrets = 100% if maxsecret = 0:
    if (_g->cnt_secret >= (_g->wbs->maxsecret ? (_g->plrs[0].ssecret * 100) / _g->wbs->maxsecret : 100))
    {
      _g->cnt_secret = (_g->wbs->maxsecret ?
        (_g->plrs[0].ssecret * 100) / _g->wbs->maxsecret : 100);
      S_StartSound(0, sfx_barexp);
      _g->sp_state++;
    }
  }
  else if (_g->sp_state == 8)
  {
    if (!(_g->bcnt&3))
      S_StartSound(0, sfx_pistol);

    _g->cnt_time += 3;

    if (_g->cnt_time >= _g->plrs[0].stime / TICRATE)
      _g->cnt_time = _g->plrs[0].stime / TICRATE;

    _g->cnt_total_time += 3;

    if (_g->cnt_total_time >= _g->wbs->totaltimes / TICRATE)
      _g->cnt_total_time = _g->wbs->totaltimes / TICRATE;

    _g->cnt_par += 3;

    if (_g->cnt_par >= _g->wbs->partime / TICRATE)
    {
      _g->cnt_par = _g->wbs->partime / TICRATE;

      if ((_g->cnt_time >= _g->plrs[0].stime / TICRATE) && (_g->cnt_total_time >= _g->wbs->totaltimes / TICRATE))
      {
        S_StartSound(0, sfx_barexp);
        _g->sp_state++;
      }
    }
  }
  else if (_g->sp_state == 10)
  {
    if (_g->acceleratestage)
    {
      S_StartSound(0, sfx_sgcock);

      if (_g->gamemode == commercial)
        WI_initNoState();
      else
        WI_initShowNextLoc();
    }
  }
  else if (_g->sp_state & 1)
  {
    if (!--_g->cnt_pause)
    {
      _g->sp_state++;
      _g->cnt_pause = TICRATE;
    }
  }
}

// ====================================================================
// WI_drawStats
// Purpose: Put the solo stats on the screen
// Args:    none
// Returns: void
//
// proff/nicolas 09/20/98 -- changed for hi-res
// CPhipps - patch drawing updated
void WI_drawStats(void)
{
  char buf[16];

  // Stats screen has no splats/YAH on the framebuffer -- just the map. Render it
  // once into both framebuffers and show buffer 0; the stats below are VDC text.
  if (s_wi_fb_dirty)
  {
    WI_renderBuffers(0);
    s_wi_fb_dirty = 0;
  }
  pcfx_im_show(0);

  WI_drawLF();     // level name + "FINISHED" (VDC text)

  // Kills / Items / Secret: label left, percentage right-aligned. Percentages count
  // up (WI_updateStats), re-placed on the VDC overlay every frame. Shown once the
  // count starts (>= 0); hidden while still -1.
  WI_text(6, 8, "KILLS");
  if (_g->cnt_kills  >= 0) { sprintf(buf, "%d%%", _g->cnt_kills);  WI_textRight(26, 8,  buf); }
  WI_text(6, 10, "ITEMS");
  if (_g->cnt_items  >= 0) { sprintf(buf, "%d%%", _g->cnt_items);  WI_textRight(26, 10, buf); }
  WI_text(6, 12, "SECRET");
  if (_g->cnt_secret >= 0) { sprintf(buf, "%d%%", _g->cnt_secret); WI_textRight(26, 12, buf); }

  // Time + Par on one row (the classic Doom-1 single-player layout).
  if (_g->cnt_time >= 0) { WI_text(4, 22, "TIME"); WI_fmtTime(buf, _g->cnt_time); WI_text(10, 22, buf); }
  if (_g->cnt_par  >= 0) { WI_text(19, 22, "PAR"); WI_fmtTime(buf, _g->cnt_par);  WI_text(24, 22, buf); }
}

// ====================================================================
// WI_accelerate
// Purpose: Skip to the end of the current intermission stage, and hand the CD
//          drive over to the transfers that skipping is about to trigger.
//
//          The backgrounds repaint from the resident KRAM cache now (no CD),
//          but the level load right after still streams the whole map pack. A
//          CD-DA track playing across it makes the drive abort the audio and
//          seek mid-transfer -- the two contend for one drive. Muting here quiets it
//          for the rest of the intermission; the next level's S_ChangeMusic
//          re-arms the player (eris_cdda_music_play clears the mute), so nothing
//          has to remember to unmute. Not resuming is deliberate: the "you are
//          here" screen is ~4 s and a resume only buys a seek out to the audio
//          track that the level load immediately seeks back from.
// Args:    none
// Returns: void
//
static void WI_accelerate(void)
{
  if (!_g->acceleratestage)
    pcfx_cdda_mute_for_dma();
  _g->acceleratestage = 1;
}


// ====================================================================
// WI_checkForAccelerate
// Purpose: See if the player has hit either the attack or use key
//          or mouse button.  If so we set acceleratestage to 1 and
//          all those display routines above jump right to the end.
// Args:    none
// Returns: void
//
void WI_checkForAccelerate(void)
{
  player_t  *player = &_g->player;

    if (_g->playeringame)
    {
      if (player->cmd.buttons & BT_ATTACK)
      {
        if (!player->attackdown)
          WI_accelerate();
        player->attackdown = true;
      }
      else
        player->attackdown = false;

      if (player->cmd.buttons & BT_USE)
      {
        if (!player->usedown)
          WI_accelerate();
        player->usedown = true;
      }
      else
        player->usedown = false;
    }
}

// ====================================================================
// WI_Ticker
// Purpose: Do various updates every gametic, for stats, animation,
//          checking that intermission music is running, etc.
// Args:    none
// Returns: void
//
void WI_Ticker(void)
{
  // counter for general background animation
  _g->bcnt++;

  if (_g->bcnt == 1)
  {
    // intermission music
    if ( _g->gamemode == commercial )
      S_ChangeMusic(mus_dm2int, true);
    else
      S_ChangeMusic(mus_inter, true);
  }

  WI_checkForAccelerate();

#ifdef DEV_EXIT_AFTER
  // DEV: auto-advance the intermission (headless can't reliably time the button
  // press) so a warp+exit run reaches the NEXT level for transition testing.
  // Goes through WI_accelerate so this stands in for a real keypress exactly --
  // including handing the CD drive to the transfers that follow.
  if (_g->bcnt > 20)
    WI_accelerate();
#endif

  switch (_g->state)
  {
    case StatCount:
         WI_updateStats();
         break;

    case ShowNextLoc:
         WI_updateShowNextLoc();
         break;

    case NoState:
         WI_updateNoState();
         break;
  }
}

/* ====================================================================
 * WI_loadData
 * Purpose: Initialize intermission data such as background graphics,
 *          patches, map names, etc.
 * Args:    none
 * Returns: void
 *
 * CPhipps - modified for new wad lump handling.
 *         - no longer preload most graphics, other funcs can use
 *           them by name
 */

void WI_loadData(void)
{
  // PC-FX: nothing to load. The stats are drawn as VDC-overlay font text (WI_drawStats),
  // not the WINUM digit graphics — so we no longer W_CacheLumpName the WINUM* lumps here.
  // That also avoids a needless (and, before the w_stage fix, hang-prone) CD read on the
  // level exit into the intermission, since those lumps aren't in the level's arena.
}


// ====================================================================
// WI_Drawer
// Purpose: Call the appropriate stats drawing routine depending on
//          what kind of game is being played (DM, coop, solo)
// Args:    none
// Returns: void
//
void WI_Drawer (void)
{
  switch (_g->state)
  {
    case StatCount:
           WI_drawStats();
         break;

    case ShowNextLoc:
         WI_drawShowNextLoc();
         break;

    case NoState:
         WI_drawNoState();
         break;
  }
}


// ====================================================================
// WI_initVariables
// Purpose: Initialize the intermission information structure
//          Note: wbstartstruct_t is defined in d_player.h
// Args:    wbstartstruct -- pointer to the structure with the data
// Returns: void
//
void WI_initVariables(wbstartstruct_t* wbstartstruct)
{

  _g->wbs = wbstartstruct;

  _g->acceleratestage = 0;
  _g->cnt = _g->bcnt = 0;
  _g->plrs = _g->wbs->plyr;

  if (!_g->wbs->maxkills)
    _g->wbs->maxkills = 1;  // probably only useful in MAP30

  if (!_g->wbs->maxitems)
    _g->wbs->maxitems = 1;

  if ( _g->gamemode != retail )
    if (_g->wbs->epsd > 2)
      _g->wbs->epsd -= 3;
}

// ====================================================================
// WI_Start
// Purpose: Call the various init routines
//          Note: wbstartstruct_t is defined in d_player.h
// Args:    wbstartstruct -- pointer to the structure with the
//          intermission data
// Returns: void
//
void WI_Start(wbstartstruct_t* wbstartstruct)
{
  WI_initVariables(wbstartstruct);
  WI_loadData();

  pcfx_im_begin();       // static two-buffer display: no per-frame page flip
  s_wi_fb_dirty = 1;

    WI_initStats();
}
