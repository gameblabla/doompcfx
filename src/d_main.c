/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2004 by
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
 *  DOOM main program (D_DoomMain) and game loop (D_DoomLoop),
 *  plus functions to determine game mode (shareware, registered),
 *  parse command line parameters, configure game parameters (turbo),
 *  and call the startup functions.
 *
 *-----------------------------------------------------------------------------
 */



#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "doomdef.h"
#include "doomtype.h"
#include "doomstat.h"
#include "d_net.h"
#include "dstrings.h"
#include "sounds.h"
#include "z_zone.h"
#include "w_wad.h"
#include "s_sound.h"
#include "v_video.h"
#include "f_finale.h"
#include "f_wipe.h"
#include "m_misc.h"
#include "m_menu.h"
#include "i_main.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_video.h"
#include "g_game.h"
#include "hu_stuff.h"
#include "wi_stuff.h"
#include "st_stuff.h"
#include "am_map.h"
#include "p_setup.h"
#include "r_draw.h"
#include "r_main.h"
#include "d_main.h"
#include "lprintf.h"  // jff 08/03/98 - declaration of lprintf
#include "am_map.h"
#include "m_cheat.h"

#include "global_data.h"

#include "pcfx_time.h"   /* render profiler (SERIAL_LOG) */
#include "pcfx_weapon.h"  /* pcfx_weapon_hide(): park the VDC weapon off-level */
#include "pcfx_cdasset.h" /* pcfx_cd_background_hide(): free CD bg buffer in-level */
#include "pcfx_text.h"    /* pcfx_text_begin(): clear the VDC font-tile overlay per frame */
#include "pcfx_present.h" /* pcfx_present_tick(): service a deferred page flip */
#include "pcfx_boot.h"    /* boot loading bar (drawn while CD-streaming to title) */
#include "pcfx_save.h"    /* BIOS backup-memory filesystem (savegames + settings) */

void GetFirstMap(int *ep, int *map); // Ty 08/29/98 - add "-warp x" functionality
static void D_PageDrawer(void);
static void D_UpdateFPS(void);


// CPhipps - removed wadfiles[] stuff


//jff 1/22/98 parms for disabling music and sound
const boolean nosfxparm = false;
const boolean nomusicparm = false;

const skill_t startskill = sk_medium;
const int startepisode = 1;
const int startmap = 1;

const boolean nodrawers = false;

static const char* timedemo = NULL;//"demo1";

/*
 * D_PostEvent - Event handling
 *
 * Called by I/O functions when an event is received.
 * Try event handlers for each code area in turn.
 * cph - in the true spirit of the Boom source, let the 
 *  short ciruit operator madness begin!
 */

void D_PostEvent(event_t *ev)
{
    /* cph - suppress all input events at game start
   * FIXME: This is a lousy kludge */
    if (_g->gametic < 3)
        return;

    M_Responder(ev) ||
            (_g->gamestate == GS_LEVEL && (
                 C_Responder(ev) ||
                 ST_Responder(ev) ||
                 AM_Responder(ev)
                 )
             ) ||
            G_Responder(ev);

}

//
// D_Wipe
//
// CPhipps - moved the screen wipe code from D_Display to here
// The screens to wipe between are already stored, this just does the timing
// and screen updating

static void D_Wipe(void)
{
    boolean done;
    int wipestart = I_GetTime () - 1;

    wipe_initMelt();

    do
    {
        int nowtime, tics;
        do
        {
            nowtime = I_GetTime();
            tics = nowtime - wipestart;
        } while (!tics);

        wipestart = nowtime;
        done = wipe_ScreenWipe(tics);

        I_UpdateNoBlit();
        M_Drawer();                   // menu is drawn even on top of wipes

    } while (!done);
}

//
// D_Display
//  draw current display, possibly wiping it from the previous
//

static void D_Display (void)
{

    boolean wipe;
    boolean intermission_fade;
    boolean level_fade;
    boolean level_entry;
    boolean viewactive = false;

    if (nodrawers)                    // for comparative timing / profiling
        return;

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    /* Reset per-frame world-phase timers; only R_RenderPlayerView refills them,
     * so non-level frames (menu/intermission) report 0 for bsp/pln/spr. */
    g_rp_bsp = g_rp_plane = g_rp_spr = g_rp_blit = 0;
    g_rp_view = g_rp_hud = 0;
#endif
#ifdef SERIAL_LOG
    g_rp_seg = g_rp_draw = g_rp_fetch = 0;
    g_rp_draw_samples = g_rp_draw_pixels = g_rp_fetch_samples = g_rp_wall_columns = 0;
    g_rp_bbox = g_rp_bbox_samples = g_rp_bbox_calls = 0;
    g_rp_addline = g_rp_addline_samples = g_rp_addline_calls = 0;
    g_rp_clip = g_rp_clip_samples = g_rp_clip_calls = 0;
    g_rp_store = g_rp_store_samples = g_rp_store_calls = 0;
    g_rp_plane_setup = g_rp_plane_draw = 0;
    g_rp_plane_samples = g_rp_plane_spans = 0;
    g_rp_bsp_nodes = g_rp_bsp_subsectors = g_rp_bsp_earlyouts = 0;
    g_rp_wall_pixels = g_rp_plane_pixels = 0;
    g_rp_masked_wall_pixels = g_rp_sprite_pixels = 0;
    g_rp_wall_page_cmaps = g_rp_wall_column_cmaps = 0;
    g_rp_wall_page_cmap_overflow = g_rp_wall_column_cmap_overflow = 0;
    g_rp_wall_lit_hits = g_rp_wall_lit_misses = 0;
    g_rp_wall_lit_bakes = g_rp_wall_lit_evictions = 0;
#endif

    if (!I_StartDisplay())
        return;

    // PC-FX: clear the VDC font-tile overlay for this frame; the drawers below
    // (HUD messages, menu, finale) re-place their glyphs, and pcfx_text_present()
    // flushes the tilemap to the VDCs in vblank (I_FinishUpdate).
    pcfx_text_begin();

    // save the current screen if about to wipe
    wipe = (_g->gamestate != _g->wipegamestate);
    intermission_fade = wipe && _g->oldgamestate == GS_LEVEL &&
                        _g->gamestate == GS_INTERMISSION;
    level_fade = wipe && _g->oldgamestate == GS_INTERMISSION &&
                 _g->gamestate == GS_LEVEL;
    level_entry = wipe && _g->gamestate == GS_LEVEL;

    if (wipe)
        wipe_StartScreen();

    /* PC-FX level exit: fade the still-displayed gameplay page (including the
     * VDC weapon and HUD) fully to black before replacing it. The new
     * intermission is presented while the shared palette remains black, then
     * faded back in below. */
    if (intermission_fade)
        I_FadePalette(false);

    // PC-FX: the RAINBOW sky is a hardware layer behind the framebuffer; show it
    // only while a level is on screen, else it bleeds through index-0 pixels on
    // the title/menu/intermission/finale. No-op when the state is unchanged.
    I_SetRainbowActive(_g->gamestate == GS_LEVEL && !level_entry);

    if (_g->gamestate != GS_LEVEL) { // Not a level
        // PC-FX: the first-person weapon is composited by the VDC sprite hardware,
        // which keeps showing the last gameplay frame's weapon over the
        // intermission/finale/menu unless we park it. Hide it while off-level.
        pcfx_weapon_hide();

        switch (_g->oldgamestate)
        {
            case -1:
            case GS_LEVEL:
                V_SetPalette(0); // cph - use default (basic) palette
            default:
                break;
        }

        switch (_g->gamestate)
        {
            case GS_INTERMISSION:
                WI_Drawer();
                break;
            case GS_FINALE:
                F_Drawer();
                break;
            case GS_DEMOSCREEN:
                D_PageDrawer();
                break;
            default:
                break;
        }
    }
    else if (_g->gametic != _g->basetic)
    { // In a level

        // Back in gameplay: free the transient CD background buffer (if any).
        pcfx_cd_background_hide();

        HU_Erase();

        // Work out if the player view is visible, and if there is a border
        viewactive = (!(_g->automapmode & am_active) || (_g->automapmode & am_overlay));

        // Now do the drawing
        if (viewactive)
        {
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
            uint64_t view_t0 = itu_ticks();
#endif
            R_RenderPlayerView (&_g->player);
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
            g_rp_view = (uint32_t)(itu_ticks() - view_t0);
#endif
        }
        else
        {
            /* A full automap skips R_RenderPlayerView, which is normally what
             * assembles the next VDC weapon SAT. Park the prior frame instead
             * so the hardware sprites cannot remain over the map. */
            pcfx_weapon_hide();
        }

        pcfx_present_tick();

        if (_g->automapmode & am_active)
            AM_Drawer();

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        uint64_t hud_t0 = itu_ticks();
#endif
        ST_Drawer(true, false);
        HU_Drawer();
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        g_rp_hud = (uint32_t)(itu_ticks() - hud_t0);
#endif
    }

    _g->oldgamestate = _g->wipegamestate = _g->gamestate;

#ifdef GEN_BOOTPACK_MANIFEST
    // Ground-truth boot capture: by the first title frame D_PageDrawer has cached
    // TITLEPIC, so the boot-resident set is complete — dump it (once) for the BOOT
    // pack builder. See w_wad.c W_BootManifestDump.
    if (_g->gamestate == GS_DEMOSCREEN)
        W_BootManifestDump();
#endif

    // menus go directly to the screen
    pcfx_present_tick();
    M_Drawer();          // menu is drawn even on top of everything

    D_BuildNewTiccmds();

    // normal update
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    uint64_t blit_t0 = itu_ticks();
#endif
    if (!wipe)
        I_FinishUpdate ();              // page flip or blit buffer
    else if (intermission_fade)
    {
        /* Latch the pre-rendered KING intermission page and flush its VDC text
         * while every shared palette entry is black. */
        wipe_EndScreen();
        I_FinishUpdate();
        I_FadePalette(true);
    }
    else if (level_fade)
    {
        /* G_DoWorldDone left the shared palette black after the load. Present
         * the first KING gameplay page plus its VDC gun/HUD while still black,
         * then reveal them together. The direct-YUV RAINBOW is enabled only at
         * the end so it cannot punch through index-0 sky pixels during the fade. */
        wipe_EndScreen();
        I_FinishUpdate();
        I_FadePalette(true);
        I_SetRainbowActive(true);
    }
    else if (level_entry)
    {
        /* Ordinary first entry/restart has no palette fade, but must still flip
         * the completed KING/VDC frame before enabling RAINBOW. Otherwise the
         * direct-YUV sky shows through the old index-0 loading page for a field. */
        wipe_EndScreen();
        I_FinishUpdate();
        I_SetRainbowActive(true);
    }
    else
    {
        // wipe update
        wipe_EndScreen();
        D_Wipe();
    }
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    g_rp_blit = (uint32_t)(itu_ticks() - blit_t0);
#endif

    I_EndDisplay();
}

//
//  D_DoomLoop()
//
// Not a globally visible function,
//  just included for source reference,
//  called by D_DoomMain, never exits.
// Manages timing and IO,
//  calls all ?_Responder, ?_Ticker, and ?_Drawer,
//  calls I_GetTime, I_StartFrame, and I_StartTic
//

static void D_DoomLoop(void)
{
    for (;;)
    {
        // frame syncronous IO operations

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        uint64_t frame_t0 = itu_ticks();
#endif

        I_StartFrame();

        // process one or more tics
        if (_g->singletics)
        {
            I_StartTic ();
            G_BuildTiccmd (&_g->netcmd);

            if (_g->advancedemo)
                D_DoAdvanceDemo ();

            M_Ticker ();
            G_Ticker ();

            _g->gametic++;
            _g->maketic++;
        }
        else
            TryRunTics (); // will run at least one tic

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        g_rp_tic = (uint32_t)(itu_ticks() - frame_t0);   // game logic (tics)
#endif

        // killough 3/16/98: change consoleplayer to displayplayer
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        uint64_t sound_t0 = itu_ticks();
#endif
        if (_g->player.mo) // cph 2002/08/10
            S_UpdateSounds(_g->player.mo);// move positional sounds
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        g_rp_sound = (uint32_t)(itu_ticks() - sound_t0);
#endif

#ifdef DEV_EXIT_AFTER
        {
            static int dev_exit_frames = 0;
            /* DEV_EXIT_REPEAT: exit EVERY DEV_EXIT_AFTER in-level frames instead of
             * once, so one run chains real level TRANSITIONS (E1M1->2->3...). The
             * transition path is the only one that exercises a dirty/fragmented heap
             * (a direct warp always loads into a clean one), so chaining is how a
             * transition-only load bug gets caught. */
#if defined(DEV_EXIT_REPEAT)
            if (_g->gamestate == GS_LEVEL && ++dev_exit_frames % (DEV_EXIT_AFTER) == 0)
#else
            if (_g->gamestate == GS_LEVEL && ++dev_exit_frames == (DEV_EXIT_AFTER))
#endif
            {
#if defined(DEV_EXIT_FINALE)
                F_StartFinale();   // jump straight to the episode-end finale
#else
                G_ExitLevel();
#endif
            }
        }
#endif

        // Update display, next frame, with current state.
        D_Display();

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        g_rp_display = (uint32_t)(itu_ticks() - sound_t0 - g_rp_sound);
        g_rp_frame = (uint32_t)(itu_ticks() - frame_t0); // whole loop iteration
        rprof_endframe();
#endif

        if(_g->fps_show)
        {
            D_UpdateFPS();
        }
    }
}

static void D_UpdateFPS()
{
    _g->fps_frames++;

    unsigned int timenow = I_GetTime();
    if(timenow >= (_g->fps_timebefore + TICRATE))
    {
        unsigned int tics_elapsed = timenow - _g->fps_timebefore;
        fixed_t f_realfps = FixedDiv((_g->fps_frames*(TICRATE*10)) << FRACBITS, tics_elapsed <<FRACBITS);

        _g->fps_framerate = (f_realfps >> FRACBITS);

        _g->fps_frames = 0;
        _g->fps_timebefore = timenow;
    }
    else if(timenow < _g->fps_timebefore)
    {
        //timer overflow.
        _g->fps_timebefore = timenow;
        _g->fps_frames = 0;
    }
}

//
//  DEMO LOOP
//


//
// D_PageTicker
// Handles timing for warped projection
//
void D_PageTicker(void)
{
    if (--_g->pagetic < 0)
        D_AdvanceDemo();
}

//
// D_PageDrawer
//
static void D_PageDrawer(void)
{
    // proff/nicolas 09/14/98 -- now stretchs bitmaps to fullscreen!
    // CPhipps - updated for new patch drawing
    // proff - added M_DrawCredits
    if (_g->pagelump)
    {
        // PC-FX: the title picture is stored uncompressed on the CD and DMA'd
        // straight into the framebuffer (no LZ4 decode, no per-word CPU blit),
        // cached per page — so it is read from CD at most once per page and then
        // never touched again. The menu that opens over it draws its cursor and
        // text on the VDC overlay (see M_Drawer / pcfx_text.c), NOT the KING
        // framebuffer, so there is nothing to repaint and no per-frame RAM blit /
        // CD read (an earlier design re-blitted the whole page from a TITLE0 LZ4
        // asset every menu frame, whose first read froze the loop ~2 s on the
        // button press that opened the menu). pcfx_cd_background_dma returns 1 when
        // it handled the raw asset (TITLEPIC); otherwise fall back to a patch draw.
        // The lump name field is 8 bytes and not NUL-terminated for an 8-char name,
        // so copy it into a terminated buffer for the lookup.
        char nm[9]; const char *src = W_GetNameForNum(_g->pagelump);
        int c = 0; for (; c < 8 && src && src[c]; c++) nm[c] = src[c];
        nm[c] = 0;

        if (!pcfx_cd_background_dma(nm))
            V_DrawNumPatch(0, 0, 0, _g->pagelump, CR_DEFAULT, VPT_STRETCH);

        // Title overlay on the VDC text layer (pcfx_text_begin cleared the
        // tilemap at the top of D_Display, so stamp it every frame): a
        // blinking start prompt plus the credit lines. Centered on the 32-cell
        // grid and kept >=32px inside every edge for TV overscan. Hidden while
        // the menu is open — the overlay belongs to M_Drawer then.
        if (!_g->menuactive && !strncmp(nm, "TITLEPIC", 8))
        {
            if (I_GetTime() & 16)              /* ~0.46 s on / off blink */
                pcfx_text_puts((PCFX_TEXT_COLS - 18) / 2, 21,
                               "PRESS RUN TO START");
            pcfx_text_puts((PCFX_TEXT_COLS - 15) / 2, 24, "(C) ID SOFTWARE");
            pcfx_text_puts((PCFX_TEXT_COLS - 23) / 2, 25,
                           "PORT BY GAMEBLABLA 2026");
        }
    }
}

//
// D_AdvanceDemo
// Called after each demo or intro demosequence finishes
//
void D_AdvanceDemo (void)
{
    _g->advancedemo = true;
}

/* killough 11/98: functions to perform demo sequences
 * cphipps 10/99: constness fixes
 */

static void D_SetPageName(const char *name)
{
    _g->pagelump = W_GetNumForName(name);
}

static void D_DrawTitle1(const char *name)
{
    /* Loop the title music at the CD-DA drive level (D9 mode 4), not
     * S_StartMusic's play-once. Vanilla never needed the loop because the
     * attract demos changed the cue every ~30 s; this port's title sequence
     * cycles TITLEPIC forever (demos are trimmed from the baked WAD), the
     * track is finite Red Book audio, and S_ChangeMusic early-outs on the
     * wrap (same song), so a play-once cue died after one pass and the
     * title sat silent. */
    S_ChangeMusic(mus_intro, true);
    _g->pagetic = (TICRATE*30);
    D_SetPageName(name);
}

static void D_DrawTitle2(const char *name)
{
    /* One looping title theme on this port: mus_dm2ttl has no CD track
     * (doom_song_3do maps it to 0 = silence), so the old cue change here
     * KILLED the music mid-track for a 30 s page. Keeping the same looping
     * cue makes S_ChangeMusic early-out and the theme run uninterrupted. */
    S_ChangeMusic(mus_intro, true);
    D_SetPageName(name);
}

/* killough 11/98: tabulate demo sequences
 */

static struct
{
    void (*func)(const char *);
    const char *name;
}

const demostates[][4] =
{
    {
        {D_DrawTitle1, "TITLEPIC"},
        {D_DrawTitle1, "TITLEPIC"},
        {D_DrawTitle2, "TITLEPIC"},
        {D_DrawTitle1, "TITLEPIC"},
    },

    /* Attract demos disabled: the DEMO* lumps are trimmed from the baked WAD
     * (tools/bake_wad.py --drop-demos) to fit the 4 MB flash, so the title loop
     * cycles TITLEPIC instead of playing a (missing) demo. */
    {
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
    },
    {
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
    },

    {
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
    },

    {
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
    },

    {
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
        {D_SetPageName, "TITLEPIC"},
    },

    {
        {NULL, NULL},
        {NULL, NULL},
        {NULL, NULL},
        {NULL, NULL},
    }


};

/*
 * This cycles through the demo sequences.
 * killough 11/98: made table-driven
 */

void D_DoAdvanceDemo(void)
{
    _g->player.playerstate = PST_LIVE;  /* not reborn */
    _g->advancedemo = _g->usergame = false;
    _g->gameaction = ga_nothing;

    _g->pagetic = TICRATE * 11;         /* killough 11/98: default behavior */
    _g->gamestate = GS_DEMOSCREEN;


    if (!demostates[++_g->demosequence][_g->gamemode].func)
        _g->demosequence = 0;

    demostates[_g->demosequence][_g->gamemode].func(demostates[_g->demosequence][_g->gamemode].name);
}

//
// D_StartTitle
//
void D_StartTitle (void)
{
    _g->gameaction = ga_nothing;
    _g->demosequence = -1;
    D_AdvanceDemo();
}

//
// CheckIWAD
//
// Verify a file is indeed tagged as an IWAD
// Scan its lumps for levelnames and return gamemode as indicated
// Detect missing wolf levels in DOOM II
//
// The filename to check is passed in iwadname, the gamemode detected is
// returned in gmode, hassec returns the presence of secret levels
//
// jff 4/19/98 Add routine to test IWAD for validity and determine
// the gamemode from it. Also note if DOOM II, whether secret levels exist
// CPhipps - const char* for iwadname, made static

// PC-FX: the IWAD is streamed from CD, so scan the resident directory (loaded by
// W_Init, which must run first) instead of a RAM image of the WAD.
static void CheckIWAD2(GameMode_t *gmode, boolean *hassec)
{
    int ud=0,rg=0,sw=0,cm=0,sc=0;

    int length = W_NumLumps();
    while (length--)
    {
        const char* name = W_GetNameForNum(length);   // 8 bytes, not NUL-terminated
        if (name[0] == 'E' && name[2] == 'M' && name[4] == 0)
        {
          if (name[1] == '4')
            ++ud;
          else if (name[1] == '3')
            ++rg;
          else if (name[1] == '2')
            ++rg;
          else if (name[1] == '1')
            ++sw;
        }
        else if (name[0] == 'M' && name[1] == 'A' && name[2] == 'P' && name[5] == 0)
        {
          ++cm;
          if (name[3] == '3')
          {
              if (name[4] == '1' || name[4] == '2')
                ++sc;
          }
        }
        //Final Doom IWAD check hacks ~Kippykip
        //TNT - MURAL1
        else if (name[0] == 'M' && name[1] == 'U' && name[2] == 'R'  && name[3] == 'A' && name[4] == 'L' && name[5] == '1' && name[6] == 0)
        {
            *gmode = commercial;
            _g->gamemission = pack_tnt;
            _g->gamemode = commercial;
            return;
        }
        //Plutonia - WFALL1
        else if (name[0] == 'W' && name[1] == 'F' && name[2] == 'A'  && name[3] == 'L' && name[4] == 'L' && name[5] == '1' && name[6] == 0)
        {
            *gmode = commercial;
            _g->gamemission = pack_plut;
            _g->gamemode = commercial;
            return;
        }
    }

    // Determine game mode from levels present
    // Must be a full set for whichever mode is present
    // Lack of wolf-3d levels also detected here

    *gmode = indetermined;
    *hassec = false;
    if (cm>=30)
    {
        *gmode = commercial;
        *hassec = sc>=2;
    }
    else if (ud>=9)
        *gmode = retail;
    else if (rg>=18)
        *gmode = registered;
    else if (sw>=9)
        *gmode = shareware;
    // PC-FX: IWAD is trimmed to fit flash (maps dropped) so full-set counts miss; fall back to the tier present.
    else if (ud)
        *gmode = retail;
    else if (rg)
        *gmode = registered;
    else if (sw)
        *gmode = shareware;
}

//
// IdentifyVersion
//
// Set the location of the defaults file and the savegame root
// Locate and validate an IWAD file
// Determine gamemode from the IWAD
//
// supports IWADs with custom names. Also allows the -iwad parameter to
// specify which iwad is being searched for if several exist in one dir.
// The -iwad parm may specify:
//
// 1) a specific pathname, which must exist (.wad optional)
// 2) or a directory, which must contain a standard IWAD,
// 3) or a filename, which must be found in one of the standard places:
//   a) current dir,
//   b) exe dir
//   c) $DOOMWADDIR
//   d) or $HOME
//
// jff 4/19/98 rewritten to use a more advanced search algorithm


static void IdentifyVersion()
{
    CheckIWAD2(&_g->gamemode, &_g->haswolflevels);

    /* jff 8/23/98 set gamemission global appropriately in all cases
     * cphipps 12/1999 - no version output here, leave that to the caller
     */
    switch(_g->gamemode)
    {
        case retail:
        case registered:
        case shareware:
            _g->gamemission = doom;
            break;
        case commercial:
            _g->gamemission = doom2;
            break;

        default:
            _g->gamemission = none;
            break;
    }

    if (_g->gamemode == indetermined)
    {
        //jff 9/3/98 use logical output routine
        lprintf(LO_WARN,"Unknown Game Version, may not work\n");
    }
}

//
// D_DoomMainSetup
//
// CPhipps - the old contents of D_DoomMain, but moved out of the main
//  line of execution so its stack space can be freed

/* TEMP boot tracer: the emulator logs every VDP register write, so stamping the
 * backdrop register at each milestone reveals the last stage reached. */

static void D_DoomMainSetup(void)
{
    // Loading bar: started in main() (before I_Init/I_InitSound's first CD
    // read), not here — see i_main.c. Ticked once per CD read by pcfx_wad_read;
    // ended just before the title starts.

    // W_Init must precede IdentifyVersion now: the IWAD is streamed from CD, so
    // the directory (which IdentifyVersion scans) is only resident after W_Init.
    lprintf(LO_INFO,"W_Init: Init WADfiles.");
    W_Init();

    IdentifyVersion();

    // jff 1/24/98 end of set to both working and command line value

    // CPhipps - localise title variable
    // print title for every printed line
    // cph - code cleaned and made smaller
    const char* doomverstr;

    switch ( _g->gamemode )
    {
        case retail:
            doomverstr = "The Ultimate DOOM";
            break;
        case shareware:
            doomverstr = "DOOM Shareware";
            break;
        case registered:
            doomverstr = "DOOM Registered";
            break;
        case commercial:  // Ty 08/27/98 - fixed gamemode vs gamemission
            switch (_g->gamemission)
            {
            case pack_plut:
                doomverstr = "DOOM 2: Plutonia Experiment";
                break;
            case pack_tnt:
                doomverstr = "DOOM 2: TNT - Evilution";
                break;
            default:
                doomverstr = "DOOM 2: Hell on Earth";
                break;
            }
            break;
        default:
            doomverstr = "Public DOOM";
            break;
    }

    /* cphipps - the main display. This shows the build date, copyright, and game type */

    lprintf(LO_ALWAYS,"PrBoom (built %s)", version_date);
    lprintf(LO_ALWAYS, "Playing: %s", doomverstr);
    lprintf(LO_ALWAYS, "PrBoom is released under the");
    lprintf(LO_ALWAYS, "GNU GPL v2.0.");

    lprintf(LO_ALWAYS, "You are welcome to");
    lprintf(LO_ALWAYS, "redistribute it under");
    lprintf(LO_ALWAYS, "certain conditions.");

    lprintf(LO_ALWAYS, "It comes with ABSOLUTELY\nNO WARRANTY.\nSee the file COPYING for\ndetails.");

    lprintf(LO_ALWAYS, "\nPhew. Thats the nasty legal\nstuff out of the way.\nLets play Doom!\n");



    // init subsystems

    G_ReloadDefaults();    // killough 3/4/98: set defaults just loaded.
    // jff 3/24/98 this sets startskill if it was -1

    // CPhipps - move up netgame init
    //jff 9/3/98 use logical output routine
    lprintf(LO_INFO,"D_InitNetGame.");
    D_InitNetGame();

    //jff 9/3/98 use logical output routine
    lprintf(LO_INFO,"M_Init: Init misc info.");
    M_Init();

    //jff 9/3/98 use logical output routine
    lprintf(LO_INFO,"R_Init: DOOM refresh daemon.");
    R_Init();

    //jff 9/3/98 use logical output routine
    lprintf(LO_INFO,"P_Init: Init Playloop state.");
    P_Init();

    //jff 9/3/98 use logical output routine
    lprintf(LO_INFO,"S_Init: Setting up sound.");
    S_Init(_g->snd_SfxVolume /* *8 */, _g->snd_MusicVolume /* *8*/ );

    //jff 9/3/98 use logical output routine
    lprintf(LO_INFO,"HU_Init: Setting up HUD.");
    HU_Init();

    //jff 9/3/98 use logical output routine
    lprintf(LO_INFO,"ST_Init: Init status bar.");
    ST_Init();

    // Bring the BIOS backup-memory filesystem up before anything touches
    // saves or settings, so a missing/unformatted device is reported once
    // here instead of failing silently deeper in the load paths.
    pcfx_boot_progress_set_label("SAVE SYSTEM");
    lprintf(LO_INFO,"PCFX_SaveInit: Init backup-memory filesystem.");
    {
        int save_rc = PCFX_SaveInit();
        if (save_rc < 0)
            lprintf(LO_WARN, "PCFX_SaveInit: %s (saves disabled)",
                    PCFX_SaveStrerror(save_rc));
    }

    // When an FX-BMP card is plugged in, ask which device to use before the
    // first backup-memory read below; with no card this is a silent no-op.
    // DEV_WARP boot builds skip menus/title by contract (see AGENTS.md), and
    // pcfx-headless answers the FX-BMP probe as present, so without this guard
    // every headless benchmark blocks here forever instead of reaching gameplay.
#ifndef DEV_WARP
    PCFX_SaveDeviceSelect();
#endif

    pcfx_boot_progress_set_label("SETTINGS");
    lprintf(LO_INFO,"G_LoadSettings: Loading settings.");
    G_LoadSettings();

    _g->idmusnum = -1; //jff 3/17/98 insure idmus number is blank

    _g->fps_show = false;

    _g->highDetail = false;

    pcfx_boot_progress_set_label("VIDEO");
    I_InitGraphics();

    // Place every whole-run allocation NOW, while the zone below the first level's
    // arena is still one contiguous block. Anything PU_STATIC that is instead created
    // lazily on the first rendered frame (palette table, HUD shadow, the UI lumps the
    // title/HUD/intermission cache on first use) lands wherever the rover has reached —
    // deep inside the big free region, past the arena — and, being unpurgeable, outlives
    // every level free as a WALL through the free space. W_PrecacheReserve then measures
    // Z_LargestFreeBlock() across those walls and sees a fraction of the real capacity,
    // so the map pack fails its one-chunk fit test and the level load falls back to the
    // scattered per-lump seek-storm (tens of CD commands / seconds of black screen).
    // These cost the same RAM either way — they are never freed — so this only fixes
    // WHERE they land: beside the other boot statics, leaving one big block for maps.
    pcfx_boot_progress_set_label("STATIC BUFFERS");
    I_PreallocStatics();
    pcfx_boot_progress_set_label("STATIC ASSETS");
    W_PreloadStatics();

    // Static UI is loaded; the title draws next. Snap the bar full and stop
    // ticking (gameplay lump streaming must not scribble the live frame).
    pcfx_boot_progress_end();

    // Boot CD cost: with the BOOT asset pack (W_LoadBootPack) the resident set streams
    // in a few abutting reads; without it, ~140 scattered lumps each pay a pcfxemu SEEK.
    // The command count is the seek proxy; the seek estimate mirrors pcfxemu's model.
    { extern unsigned g_cd_read_cmds, g_cd_read_sectors, g_cd_seek_ms;
      lprintf(LO_INFO, "Boot CD: %u cmds, %u sectors, ~%u ms seek + ~%u ms xfer",
              g_cd_read_cmds, g_cd_read_sectors, g_cd_seek_ms,
              (g_cd_read_sectors * 20u) / 3u); }

    if (timedemo)
    {
        _g->singletics = true;
        _g->timingdemo = true;            // show stats after quit
        G_DeferedPlayDemo(timedemo);
        _g->singledemo = true;            // quit after one demo
    }
    else
    {
#ifdef DEV_WARP
#ifndef DEV_WARP_MAP
#define DEV_WARP_MAP 1
#endif
        G_DeferedInitNew(sk_medium, 1, DEV_WARP_MAP);   // dev: warp straight into E1M<DEV_WARP_MAP>
#else
        D_StartTitle();                 // start up intro loop
#endif
    }
}

//
// D_DoomMain
//

void D_DoomMain(void)
{
    D_DoomMainSetup(); // CPhipps - setup out of main execution stack

    D_DoomLoop ();  // never returns
}

//
// GetFirstMap
//
// Ty 08/29/98 - determine first available map from the loaded wads and run it
//

void GetFirstMap(int *ep, int *map)
{
    int i,j; // used to generate map name
    boolean done = false;  // Ty 09/13/98 - to exit inner loops
    char test[6];  // MAPxx or ExMx plus terminator for testing
    char name[6];  // MAPxx or ExMx plus terminator for display
    boolean newlevel = false;  // Ty 10/04/98 - to test for new level
    int ix;  // index for lookup

    strcpy(name,""); // initialize
    if (*map == 0) // unknown so go search for first changed one
    {
        *ep = 1;
        *map = 1; // default E1M1 or MAP01
        if (_g->gamemode == commercial)
        {
            for (i=1;!done && i<33;i++)  // Ty 09/13/98 - add use of !done
            {
                sprintf(test,"MAP%02d",i);
                ix = W_CheckNumForName(test);
                if (ix != -1)  // Ty 10/04/98 avoid -1 subscript
                {
                        if (!*name)  // found one, not pwad.  First default.
                            strcpy(name,test);
                }
            }
        }
        else // one of the others
        {
            strcpy(name,"E1M1");  // Ty 10/04/98 - default for display
            for (i=1;!done && i<5;i++)  // Ty 09/13/98 - add use of !done
            {
                for (j=1;!done && j<10;j++)  // Ty 09/13/98 - add use of !done
                {
                    sprintf(test,"E%dM%d",i,j);
                    ix = W_CheckNumForName(test);
                    if (ix != -1)  // Ty 10/04/98 avoid -1 subscript
                    {

                            if (!*name)  // found one, not pwad.  First default.
                                strcpy(name,test);
                    }
                }
            }
        }
        //jff 9/3/98 use logical output routine
        lprintf(LO_CONFIRM,"Auto-warping to first %slevel: %s\n",
                newlevel ? "new " : "", name);  // Ty 10/04/98 - new level test
    }
}
