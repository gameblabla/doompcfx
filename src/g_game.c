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
 * DESCRIPTION:  none
 *  The original Doom description was none, basically because this file
 *  has everything. This ties up the game logic, linking the menu and
 *  input code to the underlying game by creating & respawning players,
 *  building game tics, calling the underlying thing logic.
 *
 *-----------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "doomstat.h"
#include "d_net.h"
#include "f_finale.h"
#include "m_misc.h"
#include "m_menu.h"
#include "m_random.h"
#include "p_setup.h"
#include "p_tick.h"
#include "p_map.h"
#include "d_main.h"
#include "wi_stuff.h"
#include "hu_stuff.h"
#include "st_stuff.h"
#include "am_map.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_draw.h"
#include "p_map.h"
#include "s_sound.h"
#include "dstrings.h"
#include "sounds.h"
#include "r_data.h"
#include "r_sky.h"
#include "p_inter.h"
#include "g_game.h"
#include "lprintf.h"
#include "i_main.h"
#include "i_system.h"
#include "i_video.h"

#include "global_data.h"

#include "gba_functions.h"
#include "pcfx_boot.h"   /* pcfx_load_progress_*: level-load bar (see G_DoLoadLevel) */
#include "pcfx_save.h"   /* BIOS backup-memory filesystem (savegames + settings) */

//
// controls (have defaults)
//

const int     key_right = KEYD_RIGHT;
const int     key_left = KEYD_LEFT;
const int     key_up = KEYD_UP;
const int     key_down = KEYD_DOWN;
const int     key_menu_right = KEYD_RIGHT;                                      // phares 3/7/98
const int     key_menu_left = KEYD_LEFT;                                       //     |
const int     key_menu_up = KEYD_UP;                                         //     V
const int     key_menu_down = KEYD_DOWN;
const int     key_menu_escape = KEYD_START;                                     //     |
const int     key_menu_enter = KEYD_A;                                      // phares 3/7/98
/* PC-FX six-button layout, modelled after the 32X control scheme:
 * X/Y strafe, B fires, C cycles weapons, A uses, Z opens the automap,
 * SELECT runs, and RUN/START pauses or backs out of menus. */
const int     key_strafeleft = KEYD_X;
const int     key_straferight = KEYD_Y;
const int     key_fire = KEYD_B;
const int     key_use = KEYD_A;
const int     key_speed = KEYD_SELECT;
const int     key_escape = KEYD_START;                           // phares 4/13/98
const int     key_enter = KEYD_A;
const int     key_map_right = KEYD_RIGHT;
const int     key_map_left = KEYD_LEFT;
const int     key_map_up = KEYD_UP;
const int     key_map_down = KEYD_DOWN;
const int     key_map = KEYD_Z;
const int     key_map_follow = KEYD_X;
const int     key_map_zoomin = KEYD_Y;
const int     key_map_zoomout = KEYD_C;
                                          // phares

#define MAXPLMOVE   (forwardmove[1])
#define SLOWTURNTICS  6

static const fixed_t forwardmove[2] = {0x19, 0x32};
static const fixed_t sidemove[2]    = {0x18, 0x28};
static const fixed_t angleturn[3]   = {640, 1280, 320};  // + slow turn

static void G_DoSaveGame (boolean menu);
static const byte* G_ReadDemoHeader(const byte* demo_p, size_t size, boolean failonerror);


typedef struct gba_save_data_t
{
    int save_present;
    skill_t gameskill;
    int gameepisode;
    int gamemap;
    int totalleveltimes;
	int alwaysRun;
	int gamma;

    int weaponowned[NUMWEAPONS];
    int ammo[NUMAMMO];
    int maxammo[NUMAMMO];
} gba_save_data_t;


typedef struct gba_save_settings_t
{
    unsigned int cookie;
    unsigned int alwaysRun;
    unsigned int gamma;
    unsigned int showMessages;
    unsigned int musicVolume;
    unsigned int soundVolume;
    unsigned int mouseSensitivity;

} gba_save_settings_t;

// Bumped when a field is added: an old, shorter DOOMFX.CFG is already rejected
// on size, but the cookie keeps the two layouts distinguishable.
const unsigned int settings_cookie = 0xbaddead2;

// (settings_sram_offset is gone: slots and settings are separate BIOS files
// now, not offsets into one raw SRAM image.)
//
// Backup-memory file names (device root /SRAM or /CARD is prepended by the
// pcfx_save backend, see PCFX_SaveSetDevice). One file holds all 8 slots,
// settings live in their own small file so toggling an option never rewrites
// the (much larger) savegame file.
static const char savegame_file[] = "DOOMFX.SAV";
static const char settings_file[] = "DOOMFX.CFG";

// Fill `buf` with the savegame slots from backup memory. Missing file (first
// boot), short file or device error all yield 8 empty slots; the negative
// error code (if any) is returned for callers that want to report it.
static int G_ReadSaveBuffer(byte* buf, unsigned int size)
{
    int rc = PCFX_SaveLoad(savegame_file, buf, size);

    if (rc != (int)size)
        memset(buf, 0, size);

    return rc < 0 ? rc : 0;
}

//
// G_BuildTiccmd
// Builds a ticcmd from all of the available inputs
// or reads it from the demo buffer.
// If recording a demo, write it out
//
static inline signed char fudgef(signed char b)
{
    static int c;
    if (!b || _g->longtics) return b;
    if (++c & 0x1f) return b;
    b |= 1; if (b>2) b-=2;
    return b;
}

static inline signed short fudgea(signed short b)
{
    if (!b || !_g->longtics) return b;
    b |= 1; if (b>2) b-=2;
    return b;
}


void G_BuildTiccmd(ticcmd_t* cmd)
{
    int speed;
    int tspeed;
    int forward;
    int side;
    int newweapon;                                          // phares
    /* cphipps - remove needless I_BaseTiccmd call, just set the ticcmd to zero */
    memset(cmd,0,sizeof*cmd);

#ifdef DEV_BENCH_AUTOMOVE
    /* Performance benchmark input is generated from the tic number rather than
     * emulator video fields.  TryRunTics rebuilds it for each executed tic so
     * catch-up batches cannot replay a phase-boundary command.  Faster and
     * slower builds therefore execute the same command stream.  The cycle
     * exercises pure turning, diagonal and straight forward motion, both turn
     * directions, and the matching reverse phases. */
    {
        unsigned phase = ((unsigned)_g->gametic / 175u) & 7u;
        if (phase == 1u || phase == 2u || phase == 3u)
            cmd->forwardmove = (signed char)forwardmove[1];
        else if (phase >= 5u)
            cmd->forwardmove = -(signed char)forwardmove[1];

        if (phase == 0u || phase == 1u || phase == 5u)
            cmd->angleturn = -(signed short)angleturn[1];
        else if (phase == 3u || phase == 4u || phase == 7u)
            cmd->angleturn = (signed short)angleturn[1];
#ifdef DEV_BENCH_FIRE
        /* "Actually playing" variant: spray deterministic bursts while moving
         * so monsters wake, fight back, and damage/pickup palette flashes and
         * muzzle/monster sprite load land in the measured window. */
        if (((unsigned)_g->gametic & 63u) < 24u)
            cmd->buttons |= BT_ATTACK;
        /* Keep the run alive: top health back up each tic so damage (and its
         * palette flash) still lands but the player never dies — a death-cam
         * idles the rest of the measured window with cheap frames. */
        if (_g->player.mo && _g->player.playerstate == PST_LIVE &&
            _g->player.health < 100)
        {
            _g->player.health = 100;
            _g->player.mo->health = 100;
        }
#endif
        return;
    }
#endif

    // Run button (SELECT) or Use/A negate the always-run setting, so holding
    // either walks when always-run is on and runs when it's off.  A still
    // triggers BT_USE below regardless of this, so use is never lost.
    speed = ((_g->gamekeydown[key_speed] || _g->gamekeydown[key_use]) ^ _g->alwaysRun);

    forward = side = 0;

    /* With a mouse plugged into port 2, the mouse handles turning, so the
     * D-pad left/right strafe instead of turning, freeing up the X/Y
     * buttons (formerly strafe) to cycle weapons. */
    if (!_g->mousepresent)
    {
        // use two stage accelerative turning
        // on the keyboard and joystick
        if (_g->gamekeydown[key_right] || _g->gamekeydown[key_left])
            _g->turnheld ++;
        else
            _g->turnheld = 0;

        if (_g->turnheld < SLOWTURNTICS)
            tspeed = 2;             // slow turn
        else
            tspeed = speed;

        // let movement keys cancel each other out

        if (_g->gamekeydown[key_right])
            cmd->angleturn -= angleturn[tspeed];
        if (_g->gamekeydown[key_left])
            cmd->angleturn += angleturn[tspeed];
    }

    if (_g->gamekeydown[key_up])
        forward += forwardmove[speed];
    if (_g->gamekeydown[key_down])
        forward -= forwardmove[speed];

    if (_g->mousepresent)
    {
        if (_g->gamekeydown[key_right])
            side += sidemove[speed];
        if (_g->gamekeydown[key_left])
            side -= sidemove[speed];
    }
    else
    {
        if (_g->gamekeydown[key_straferight])
            side += sidemove[speed];

        if (_g->gamekeydown[key_strafeleft])
            side -= sidemove[speed];
    }

    if (_g->gamekeydown[key_fire])
        cmd->buttons |= BT_ATTACK;

    if (_g->gamekeydown[key_use])
    {
        cmd->buttons |= BT_USE;
    }

    /* PC-FX mouse: left button fires, right button uses.  There's no
     * forward/backward from the mouse — only the X axis turns (Y is
     * unused for movement). */
    if (_g->mousebuttons & 1)
        cmd->buttons |= BT_ATTACK;
    if (_g->mousebuttons & 2)
        cmd->buttons |= BT_USE;

    /* Turn scale.  angleturn is applied as (angleturn << 16) on a 32-bit BAM
     * angle, so one unit is 360/65536 deg; MOUSE_TURN_SCALE units per mouse
     * count means a 360 needs 65536/scale counts.  The real PC-FX mouse is
     * about 200 counts per inch, so the old fixed x16 needed ~20 inches of
     * desk for one full turn -- fine under an emulator (which feeds scaled
     * host-mouse deltas, typically 800+ dpi) but unusably slow on hardware.
     * The default here, sens 6, gives ~112 units/count == ~3 inches per 360.
     * Sensitivity 0 reproduces the old x16 for anyone who wants it. */
    {
        int turn = _g->mousex * MOUSE_TURN_SCALE(_g->mouseSensitivity);

        /* angleturn is a signed short: a fast swipe at high sensitivity can
         * overflow it, which would wrap the turn round to the other direction.
         * Clamp instead, leaving room for the keyboard/pad turn added above. */
        if (turn >  16384) turn =  16384;
        if (turn < -16384) turn = -16384;

        cmd->angleturn -= turn;
    }
    _g->mousex = 0;
    _g->mousey = 0;

    // Toggle between the top 2 favorite weapons.                   // phares
    // If not currently aiming one of these, switch to              // phares
    // the favorite. Only switch if you possess the weapon.         // phares

    // killough 3/22/98:
    //
    // Perform automatic weapons switch here rather than in p_pspr.c,
    // except in demo_compatibility mode.
    //
    // killough 3/26/98, 4/2/98: fix autoswitch when no weapons are left

    if (_g->cycleweaponcooldown)
        _g->cycleweaponcooldown--;

    /* With a mouse present, the former strafe buttons (X/Y) cycle weapons
     * down/up instead — the D-pad took over strafing, so X/Y were free. */
    if (_g->mousepresent && _g->gamekeydown[key_strafeleft] && !_g->cycledowndown && !_g->cycleweaponcooldown)
    {
        newweapon = P_WeaponCycleDown(&_g->player);
        _g->cycleweaponcooldown = 5;
    }
    else if (!_g->cycleweaponcooldown &&
             ((_g->gamekeydown[KEYD_C] && !_g->cycleweapondown) ||
              (_g->mousepresent && _g->gamekeydown[key_straferight] && !_g->cycleupdown)))
    {
        newweapon = P_WeaponCycleUp(&_g->player);
        // Guard against controller-read bounce turning one physical press
        // into two registered edges: hold off the next cycle briefly.
        _g->cycleweaponcooldown = 5;
    }
    else if ((_g->player.attackdown && !P_CheckAmmo(&_g->player)))
        newweapon = P_SwitchWeapon(&_g->player);           // phares
    else
    {                                 // phares 02/26/98: Added gamemode checks
        newweapon = wp_nochange;

        // killough 3/22/98: For network and demo consistency with the
        // new weapons preferences, we must do the weapons switches here
        // instead of in p_user.c. But for old demos we must do it in
        // p_user.c according to the old rules. Therefore demo_compatibility
        // determines where the weapons switch is made.

        // killough 2/8/98:
        // Allow user to switch to fist even if they have chainsaw.
        // Switch to fist or chainsaw based on preferences.
        // Switch to shotgun or SSG based on preferences.

        {
            const player_t *player = &_g->player;

            // only select chainsaw from '1' if it's owned, it's
            // not already in use, and the player prefers it or
            // the fist is already in use, or the player does not
            // have the berserker strength.

            if (newweapon==wp_fist && player->weaponowned[wp_chainsaw] &&
                    player->readyweapon!=wp_chainsaw &&
                    (player->readyweapon==wp_fist ||
                     !player->powers[pw_strength] ||
                     P_WeaponPreferred(wp_chainsaw, wp_fist)))
                newweapon = wp_chainsaw;

            // Select SSG from '3' only if it's owned and the player
            // does not have a shotgun, or if the shotgun is already
            // in use, or if the SSG is not already in use and the
            // player prefers it.

            if (newweapon == wp_shotgun && _g->gamemode == commercial &&
                    player->weaponowned[wp_supershotgun] &&
                    (!player->weaponowned[wp_shotgun] ||
                     player->readyweapon == wp_shotgun ||
                     (player->readyweapon != wp_supershotgun &&
                      P_WeaponPreferred(wp_supershotgun, wp_shotgun))))
                newweapon = wp_supershotgun;
        }
        // killough 2/8/98, 3/22/98 -- end of weapon selection changes
    }
    _g->cycleweapondown = _g->gamekeydown[KEYD_C];
    _g->cycleupdown = _g->gamekeydown[key_straferight];
    _g->cycledowndown = _g->gamekeydown[key_strafeleft];

    if (newweapon != wp_nochange)
    {
        cmd->buttons |= BT_CHANGE;
        cmd->buttons |= newweapon<<BT_WEAPONSHIFT;
    }

    if (forward > MAXPLMOVE)
        forward = MAXPLMOVE;
    else if (forward < -MAXPLMOVE)
        forward = -MAXPLMOVE;
    if (side > MAXPLMOVE)
        side = MAXPLMOVE;
    else if (side < -MAXPLMOVE)
        side = -MAXPLMOVE;

    cmd->forwardmove += fudgef((signed char)forward);
    cmd->sidemove += side;
    cmd->angleturn = fudgea(cmd->angleturn);
}

#include "z_bmalloc.h"
//
// G_DoLoadLevel
//

static void G_DoLoadLevel (void)
{
    /* Level setup is a blocking CD operation. Replace the old display before
     * even the sky/texture lookups begin so every load shows a centered busy
     * message rather than a frozen intermission/title/gameplay frame. */
    if (!nodrawers)
        I_PrepareLevelLoad();

    // Set the sky map.
    // First thing, we have a dummy sky texture name,
    //  a flat. The data is in the WAD only because
    //  we look for an actual index, instead of simply
    //  setting one.

    _g->skyflatnum = R_FlatNumForName ( SKYFLATNAME );

    // DOOM determines the sky texture to be used
    // depending on the current episode, and the game version.
    if (_g->gamemode == commercial)
    {
        _g->skytexture = R_LoadTextureByName("SKY3");
        if (_g->gamemap < 12)
            _g->skytexture = R_LoadTextureByName ("SKY1");
        else
            if (_g->gamemap < 21)
                _g->skytexture = R_LoadTextureByName ("SKY2");
    }
    else //jff 3/27/98 and lets not forget about DOOM and Ultimate DOOM huh?
        switch (_g->gameepisode)
        {
            case 1:
                _g->skytexture = R_LoadTextureByName ("SKY1");
                break;
            case 2:
                _g->skytexture = R_LoadTextureByName ("SKY2");
                break;
            case 3:
                _g->skytexture = R_LoadTextureByName ("SKY3");
                break;
            case 4: // Special Edition sky
                _g->skytexture = R_LoadTextureByName ("SKY4");
                break;
        }//jff 3/27/98 end sky setting fix

    /* cph 2006/07/31 - took out unused levelstarttic variable */

    if (_g->wipegamestate == GS_LEVEL)
        _g->wipegamestate = -1;             // force a wipe

    _g->gamestate = GS_LEVEL;


    if (_g->playeringame && _g->player.playerstate == PST_DEAD)
        _g->player.playerstate = PST_REBORN;


    // initialize the msecnode_t freelist.                     phares 3/25/98
    // any nodes in the freelist are gone by now, cleared
    // by Z_FreeTags() when the previous level ended or player
    // died.

    DECLARE_BLOCK_MEMORY_ALLOC_ZONE(secnodezone);
    NULL_BLOCK_MEMORY_ALLOC_ZONE(secnodezone);


    // The map + its whole graphics pool stream from CD here (seconds on an unpacked
    // map) while the previous frame (title / intermission) stays frozen on screen.
    // Draw a progress bar over it — ticked per CD read from pcfx_cd_account — so the
    // load reads as working, not hung. Snapped full and disabled before the level's
    // first frame renders (which repaints the whole framebuffer).
    pcfx_load_progress_begin();
    P_SetupLevel (_g->gameepisode, _g->gamemap, 0, _g->gameskill);
    pcfx_load_progress_end();

    _g->gameaction = ga_nothing;
    Z_CheckHeap ();

    // clear cmd building stuff
    memset (_g->gamekeydown, 0, sizeof(_g->gamekeydown));

    // killough 5/13/98: in case netdemo has consoleplayer other than green
    ST_Start();
    HU_Start();
}


#ifdef DEV_WARP
// Warp to the next map that's actually baked into this ROM, wrapping around.
// Only E1M1..E1M6 are baked (see bake_wad.py --keep-maps); missing E1M7..9 are
// skipped so D can't drop us into a non-existent map. Derives the set at runtime
// via W_CheckNumForName, so it auto-adapts if the baked map list changes.
static void G_CycleToNextBakedMap(void)
{
    char name[8] = "E0M0";
    int ep = _g->gameepisode, m = _g->gamemap;
    name[1] = (char)('0' + ep);
    for (int i = 0; i < 9; i++)                 // at most 9 maps/episode
    {
        m = (m % 9) + 1;                         // advance 1..9 with wrap
        name[3] = (char)('0' + m);
        if (W_CheckNumForName(name) >= 0)
            break;
    }
    G_DeferedInitNew(_g->gameskill, ep, m);
}
#endif

//
// G_Responder
// Get info needed to make ticcmd_ts for the players.
//

boolean G_Responder (event_t* ev)
{
    // any other key pops up menu if in demos
    //
    // killough 8/2/98: enable automap in -timedemo demos
    //
    // killough 9/29/98: make any key pop up menu regardless of
    // which kind of demo, and allow other events during playback

    if (_g->gameaction == ga_nothing && (_g->demoplayback || _g->gamestate == GS_DEMOSCREEN))
    {
        // killough 10/98:
        // Don't pop up menu, if paused in middle
        // of demo playback, or if automap active.
        // Don't suck up keys, which may be cheats
        if(_g->gamestate == GS_DEMOSCREEN)
        {
            if(!(_g->automapmode & am_active))
            {
                if(ev->type == ev_keydown)
                {
                    M_StartControlPanel();
                    return true;
                }
            }
        }

        return false;
    }

    if (_g->gamestate == GS_FINALE && F_Responder(ev))
        return true;  // finale ate the event

#ifdef DEV_WARP
    // PC-FX D button: warp to the next baked map (only while in a level).
    if (ev->type == ev_keydown && ev->data1 == KEYD_CYCLEMAP)
    {
        if (_g->gamestate == GS_LEVEL)
            G_CycleToNextBakedMap();
        return true;
    }
#endif

    switch (ev->type)
    {
        case ev_keydown:

            if (ev->data1 <NUMKEYS)
                _g->gamekeydown[ev->data1] = true;
            return true;    // eat key down events

        case ev_keyup:
            if (ev->data1 <NUMKEYS)
                _g->gamekeydown[ev->data1] = false;
            return false;   // always let key up events filter down

        case ev_mouse:
            _g->mousepresent = true;   // sticky: once seen, mouse control layout stays active
            _g->mousebuttons = ev->data1;
            _g->mousex += ev->data2;
            _g->mousey += ev->data3;
            return true;

        default:
            break;
    }
    return false;
}

//
// G_Ticker
// Make ticcmd_ts for the players.
//

void G_Ticker (void)
{
    P_MapStart();

    if(_g->playeringame && _g->player.playerstate == PST_REBORN)
        G_DoReborn (0);

    P_MapEnd();

    // do things to change the game state
    while (_g->gameaction != ga_nothing)
    {
        switch (_g->gameaction)
        {
        case ga_loadlevel:
            _g->player.playerstate = PST_REBORN;
            G_DoLoadLevel ();
            break;
        case ga_newgame:
            G_DoNewGame ();
            break;
        case ga_loadgame:
            G_DoLoadGame ();
            break;
        case ga_savegame:
            G_DoSaveGame (false);
            break;
        case ga_playdemo:
            G_DoPlayDemo ();
            break;
        case ga_completed:
            G_DoCompleted ();
            break;
        case ga_victory:
            F_StartFinale ();
            break;
        case ga_worlddone:
            G_DoWorldDone ();
            break;
        case ga_nothing:
            break;
        }
    }

    if (!_g->demoplayback && _g->menuactive)
        _g->basetic++;  // For revenant tracers and RNG -- we must maintain sync
    else
    {
        if (_g->playeringame)
        {
            ticcmd_t *cmd = &_g->player.cmd;

            memcpy(cmd, &_g->netcmd, sizeof *cmd);

            if (_g->demoplayback)
                G_ReadDemoTiccmd (cmd);
        }
    }

    // cph - if the gamestate changed, we may need to clean up the old gamestate
    if (_g->gamestate != _g->prevgamestate)
    {
        switch (_g->prevgamestate)
        {
        case GS_LEVEL:
            // This causes crashes at level end - Neil Stevens
            // The crash is because the sounds aren't stopped before freeing them
            // the following is a possible fix
            // This fix does avoid the crash wowever, with this fix in, the exit
            // switch sound is cut off
            // S_Stop();
            // Z_FreeTags(PU_LEVEL, PU_PURGELEVEL-1);
            break;
        case GS_INTERMISSION:
            WI_End();
        default:
            break;
        }
        _g->prevgamestate = _g->gamestate;
    }

    // do main actions
    switch (_g->gamestate)
    {
    case GS_LEVEL:
        P_Ticker ();
        ST_Ticker ();
        AM_Ticker ();
        HU_Ticker ();
        break;

    case GS_INTERMISSION:
        WI_Ticker ();
        break;

    case GS_FINALE:
        F_Ticker ();
        break;

    case GS_DEMOSCREEN:
        D_PageTicker ();
        break;
    }
}

//
// PLAYER STRUCTURE FUNCTIONS
// also see P_SpawnPlayer in P_Things
//

//
// G_PlayerFinishLevel
// Can when a player completes a level.
//

static void G_PlayerFinishLevel(int player)
{
    player_t *p = &_g->player;
    memset(p->powers, 0, sizeof p->powers);
    memset(p->cards, 0, sizeof p->cards);
    p->mo = NULL;           // cph - this is allocated PU_LEVEL so it's gone
    p->extralight = 0;      // cancel gun flashes
    p->fixedcolormap = 0;   // cancel ir gogles
    p->damagecount = 0;     // no palette changes
    p->bonuscount = 0;
}

//
// G_PlayerReborn
// Called after a player dies
// almost everything is cleared and initialized
//

void G_PlayerReborn (int player)
{
    player_t *p;
    int i;
    int killcount;
    int itemcount;
    int secretcount;

    killcount = _g->player.killcount;
    itemcount = _g->player.itemcount;
    secretcount = _g->player.secretcount;

    p = &_g->player;

    int cheats = p->cheats;
    memset (p, 0, sizeof(*p));
    p->cheats = cheats;

    _g->player.killcount = killcount;
    _g->player.itemcount = itemcount;
    _g->player.secretcount = secretcount;

    p->usedown = p->attackdown = true;  // don't do anything immediately
    p->playerstate = PST_LIVE;
    p->health = initial_health;  // Ty 03/12/98 - use dehacked values
    p->readyweapon = p->pendingweapon = wp_pistol;
    p->weaponowned[wp_fist] = true;
    p->weaponowned[wp_pistol] = true;
    p->ammo[am_clip] = initial_bullets; // Ty 03/12/98 - use dehacked values

#ifdef DEV_SHOTGUN_TEST
    /* Temporary headless regression hook: start with a loaded shotgun so repeated
     * hardware-sprite frame transitions can be captured immediately after DEV_WARP. */
    p->weaponowned[wp_shotgun] = true;
    p->readyweapon = p->pendingweapon = wp_shotgun;
    p->ammo[am_shell] = 100;
#endif

    for (i=0 ; i<NUMAMMO ; i++)
        p->maxammo[i] = maxammo[i];
}

//
// G_DoReborn
//

void G_DoReborn (int playernum)
{
    _g->gameaction = ga_loadlevel;      // reload the level from scratch
}

// DOOM Par Times
const int pars[4][10] = {
    {0},
    {0,30,75,120,90,165,180,180,30,165},
    {0,90,90,90,120,90,360,240,30,170},
    {0,90,45,90,150,90,90,165,30,135}
};

// DOOM II Par Times
const int cpars[32] = {
    30,90,120,120,90,150,120,120,270,90,  //  1-10
    210,150,150,150,210,150,420,150,210,150,  // 11-20
    240,150,180,150,150,300,330,420,300,180,  // 21-30
    120,30          // 31-32
};


void G_ExitLevel (void)
{
    _g->secretexit = false;
    _g->gameaction = ga_completed;
}

// Here's for the german edition.
// IF NO WOLF3D LEVELS, NO SECRET EXIT!

void G_SecretExitLevel (void)
{
    if (_g->gamemode!=commercial || _g->haswolflevels)
        _g->secretexit = true;
    else
        _g->secretexit = false;
    _g->gameaction = ga_completed;
}

//
// G_DoCompleted
//

void G_DoCompleted (void)
{
    _g->gameaction = ga_nothing;

    if (_g->playeringame)
        G_PlayerFinishLevel(0);        // take away cards and stuff

    if (_g->automapmode & am_active)
        AM_Stop();

    if (_g->gamemode != commercial && _g->gamemap == 9) // kilough 2/7/98
        _g->player.didsecret = true;

    _g->wminfo.didsecret = _g->player.didsecret;
    _g->wminfo.epsd = _g->gameepisode -1;
    _g->wminfo.last = _g->gamemap -1;

    // wminfo.next is 0 biased, unlike gamemap
    if (_g->gamemode == commercial)
    {
        if (_g->secretexit)
            switch(_g->gamemap)
            {
                case 15:
                    _g->wminfo.next = 30; break;
                case 31:
                    _g->wminfo.next = 31; break;
            }
        else
            switch(_g->gamemap)
            {
                case 31:
                case 32:
                    _g->wminfo.next = 15; break;
                default:
                    _g->wminfo.next = _g->gamemap;
            }
    }
    else
    {
        if (_g->secretexit)
            _g->wminfo.next = 8;  // go to secret level
        else
            if (_g->gamemap == 9)
            {
                // returning from secret level
                switch (_g->gameepisode)
                {
                    case 1:
                        _g->wminfo.next = 3;
                        break;
                    case 2:
                        _g->wminfo.next = 5;
                        break;
                    case 3:
                        _g->wminfo.next = 6;
                        break;
                    case 4:
                        _g->wminfo.next = 2;
                        break;
                }
            }
            else
                _g->wminfo.next = _g->gamemap;          // go to next level
    }

    _g->wminfo.maxkills = _g->totalkills;
    _g->wminfo.maxitems = _g->totalitems;
    _g->wminfo.maxsecret = _g->totalsecret;

    if ( _g->gamemode == commercial )
        _g->wminfo.partime = TICRATE*cpars[_g->gamemap-1];
    else
        _g->wminfo.partime = TICRATE*pars[_g->gameepisode][_g->gamemap];

    _g->wminfo.pnum = 0;


    _g->wminfo.plyr[0].in = _g->playeringame;
    _g->wminfo.plyr[0].skills = _g->player.killcount;
    _g->wminfo.plyr[0].sitems = _g->player.itemcount;
    _g->wminfo.plyr[0].ssecret = _g->player.secretcount;
    _g->wminfo.plyr[0].stime = _g->leveltime;

    /* cph - modified so that only whole seconds are added to the totalleveltimes
   *  value; so our total is compatible with the "naive" total of just adding
   *  the times in seconds shown for each level. Also means our total time
   *  will agree with Compet-n.
   */
    _g->wminfo.totaltimes = (_g->totalleveltimes += (_g->leveltime - _g->leveltime%35));

    _g->gamestate = GS_INTERMISSION;
    _g->automapmode &= ~am_active;

    // lmpwatch.pl engine-side demo testing support
    // print "FINISHED: <mapname>" when the player exits the current map
    if (nodrawers && (_g->demoplayback || _g->timingdemo))
    {
        if (_g->gamemode == commercial)
            lprintf(LO_INFO, "FINISHED: MAP%02d\n", _g->gamemap);
        else
            lprintf(LO_INFO, "FINISHED: E%dM%d\n", _g->gameepisode, _g->gamemap);
    }

    WI_Start (&_g->wminfo);
}

//
// G_WorldDone
//

void G_WorldDone (void)
{
    _g->gameaction = ga_worlddone;

    if (_g->secretexit)
        _g->player.didsecret = true;

    if (_g->gamemode == commercial)
    {
        switch (_g->gamemap)
        {
        case 15:
        case 31:
            if (!_g->secretexit)
                break;
        case 6:
        case 11:
        case 20:
        case 30:
            F_StartFinale ();
            break;
        }
    }
    else if (_g->gamemap == 8)
        _g->gameaction = ga_victory; // cph - after ExM8 summary screen, show victory stuff
}

void G_DoWorldDone (void)
{
    /* G_DoLoadLevel blocks before D_Display can observe this state change, so
     * fade the completed intermission here while it is still on screen. */
    if (!nodrawers)
        I_FadePalette(false);

    _g->idmusnum = -1;             //jff 3/17/98 allow new level's music to be loaded
    _g->gamestate = GS_LEVEL;
    _g->gamemap = _g->wminfo.next+1;
    G_DoLoadLevel();
    _g->gameaction = ga_nothing;
}

// killough 2/28/98: A ridiculously large number
// of players, the most you'll ever need in a demo
// or savegame. This is used to prevent problems, in
// case more players in a game are supported later.

#define MIN_MAXPLAYERS 32

//
// killough 5/15/98: add forced loadgames, which allow user to override checks
//

void G_ForcedLoadGame(void)
{
    // CPhipps - net loadgames are always forced, so we only reach here
    //  in single player
    _g->gameaction = ga_loadgame;
}


//
// Update the strings displayed in the load-save menu.
//
void G_UpdateSaveGameStrings()
{
    unsigned int savebuffersize = sizeof(gba_save_data_t) * 8;


    byte* loadbuffer = Z_Malloc(savebuffersize, PU_STATIC, NULL);

    G_ReadSaveBuffer(loadbuffer, savebuffersize);

    gba_save_data_t* saveslots = (gba_save_data_t*)loadbuffer;

    for(int i = 0; i < 8; i++)
    {
        if(saveslots[i].save_present != 1)
        {
            strcpy(_g->savegamestrings[i], "EMPTY");
        }
        else
        {
            if(_g->gamemode == commercial)
            {
                strcpy(_g->savegamestrings[i], "MAP ");

                itoa(saveslots[i].gamemap, &_g->savegamestrings[i][4], 10);
            }
            else
            {
                strcpy(_g->savegamestrings[i], "ExMy");

                _g->savegamestrings[i][1] = '0' + saveslots[i].gameepisode;
                _g->savegamestrings[i][3] = '0' + saveslots[i].gamemap;
            }
        }
    }

    Z_Free(loadbuffer);
}

// killough 3/16/98: add slot info
// killough 5/15/98: add command-line
void G_LoadGame(int slot, boolean command)
{
    _g->savegameslot = slot;
    _g->demoplayback = false;

    // Defer the actual load to the G_Ticker gameaction pass (ga_loadgame) instead
    // of loading a whole level synchronously from the menu/input context. The old
    // immediate call rebuilt the level from inside M_Responder; a second menu load
    // reached this point while the input edge state from the first (blocking) load
    // was still unwound, and was silently dropped. Deferral makes every load run
    // from the same safe point, so repeated loads work.
    _g->gameaction = ga_loadgame;
}

void G_DoLoadGame()
{
    unsigned int savebuffersize = sizeof(gba_save_data_t) * 8;


    byte* loadbuffer = Z_Malloc(savebuffersize, PU_STATIC, NULL);

    int rc = G_ReadSaveBuffer(loadbuffer, savebuffersize);

    gba_save_data_t* saveslots = (gba_save_data_t*)loadbuffer;

    gba_save_data_t* savedata = &saveslots[_g->savegameslot];

    if(savedata->save_present != 1)
    {
        if (rc < 0)
            lprintf(LO_WARN, "G_DoLoadGame: %s", PCFX_SaveStrerror(rc));

        // Clear the deferred action so G_Ticker's `while (gameaction != ga_nothing)`
        // loop does not spin forever on an empty/unreadable slot.
        _g->gameaction = ga_nothing;
        Z_Free(loadbuffer);
        return;
    }

    _g->gameskill = savedata->gameskill;
    _g->gameepisode = savedata->gameepisode;
    _g->gamemap = savedata->gamemap;
	_g->alwaysRun = savedata->alwaysRun;
	_g->gamma = savedata->gamma;
	V_SetPalLump(_g->gamma);
	
    G_InitNew (_g->gameskill, _g->gameepisode, _g->gamemap);

    _g->totalleveltimes = savedata->totalleveltimes;
    memcpy(_g->player.weaponowned, savedata->weaponowned, sizeof(savedata->weaponowned));
    memcpy(_g->player.ammo, savedata->ammo, sizeof(savedata->ammo));
    memcpy(_g->player.maxammo, savedata->maxammo, sizeof(savedata->maxammo));
	
    //If stored maxammo is more than no backpack ammo, player had a backpack.
    if(_g->player.maxammo[am_clip] > maxammo[am_clip])
		_g->player.backpack = true;

    Z_Free(loadbuffer);
}

//
// G_SaveGame
// Called by the menu task.
// Description is a 24 byte text string
//

void G_SaveGame(int slot, const char *description)
{
    _g->savegameslot = slot;
    G_DoSaveGame(true);
}

static void G_DoSaveGame(boolean menu)
{
    unsigned int savebuffersize = sizeof(gba_save_data_t) * 8;

    byte* savebuffer = Z_Malloc(savebuffersize, PU_STATIC, NULL);

    // Read-modify-write: keep the other 7 slots intact.
    G_ReadSaveBuffer(savebuffer, savebuffersize);

    gba_save_data_t* saveslots = (gba_save_data_t*)savebuffer;

    gba_save_data_t* savedata = &saveslots[_g->savegameslot];

    savedata->save_present = 1;

    savedata->gameskill = _g->gameskill;
    savedata->gameepisode = _g->gameepisode;
    savedata->gamemap = _g->gamemap;
    savedata->totalleveltimes = _g->totalleveltimes;
	savedata->alwaysRun = _g->alwaysRun;
	savedata->gamma = _g->gamma;

    memcpy(savedata->weaponowned, _g->player.weaponowned, sizeof(savedata->weaponowned));
    memcpy(savedata->ammo, _g->player.ammo, sizeof(savedata->ammo));
    memcpy(savedata->maxammo, _g->player.maxammo, sizeof(savedata->maxammo));

    int rc = PCFX_SaveStore(savegame_file, savebuffer, savebuffersize);

    Z_Free(savebuffer);

    if (rc < 0)
    {
        // Keep the message alive after this frame: player.message is only a
        // pointer, so format into a static buffer.
        static char errmsg[48];

        snprintf(errmsg, sizeof errmsg, "SAVE FAILED: %s", PCFX_SaveStrerror(rc));
        _g->player.message = errmsg;

        lprintf(LO_WARN, "G_DoSaveGame: %s", PCFX_SaveStrerror(rc));
    }
    else
        _g->player.message = GGSAVED;

    G_UpdateSaveGameStrings();
}

void G_SaveSettings()
{
    gba_save_settings_t settings;

    settings.cookie = settings_cookie;

    settings.gamma = _g->gamma;
    settings.alwaysRun = _g->alwaysRun;

    settings.showMessages = _g->showMessages;

    settings.musicVolume = _g->snd_MusicVolume;
    settings.soundVolume = _g->snd_SfxVolume;
    settings.mouseSensitivity = _g->mouseSensitivity;

    int rc = PCFX_SaveStore(settings_file, (byte*)&settings, sizeof(settings));

    if (rc < 0)
        lprintf(LO_WARN, "G_SaveSettings: %s", PCFX_SaveStrerror(rc));
}

void G_LoadSettings()
{
    gba_save_settings_t settings;

    if (PCFX_SaveLoad(settings_file, (byte*)&settings, sizeof(settings)) != (int)sizeof(settings))
        return;   // missing/short/unreadable -> keep compiled-in defaults

    if(settings.cookie == settings_cookie)
    {
        _g->gamma = (settings.gamma > 0) ? 1 : 0;
        _g->alwaysRun = (settings.alwaysRun > 0) ? 1 : 0;

        _g->showMessages = (settings.showMessages > 0) ? 1 : 0;

        _g->snd_SfxVolume = (settings.soundVolume > 15) ? 15 : settings.soundVolume;
        _g->snd_MusicVolume = (settings.musicVolume > 15) ? 15 : settings.musicVolume;

        _g->mouseSensitivity = (settings.mouseSensitivity > MOUSE_SENS_MAX)
                                 ? MOUSE_SENS_MAX : settings.mouseSensitivity;

        V_SetPalLump(_g->gamma);
        V_SetPalette(0);

        S_SetSfxVolume(_g->snd_SfxVolume);
        S_SetMusicVolume(_g->snd_MusicVolume);
    }
}

void G_DeferedInitNew(skill_t skill, int episode, int map)
{
    _g->d_skill = skill;
    _g->d_episode = episode;
    _g->d_map = map;
    _g->gameaction = ga_newgame;
}

// killough 3/1/98: function to reload all the default parameter
// settings before a new game begins

void G_ReloadDefaults(void)
{
    // killough 3/1/98: Initialize options based on config file
    // (allows functions above to load different values for demos
    // and savegames without messing up defaults).

    _g->demoplayback = false;
    _g->singledemo = false;            // killough 9/29/98: don't stop after 1 demo
}

void G_DoNewGame (void)
{
    G_ReloadDefaults();            // killough 3/1/98
    G_InitNew (_g->d_skill, _g->d_episode, _g->d_map);
    _g->gameaction = ga_nothing;

    //jff 4/26/98 wake up the status bar in case were coming out of a DM demo
    ST_Start();
}

//
// G_InitNew
// Can be called by the startup code or the menu task,
// consoleplayer, displayplayer, playeringame[] should be set.
//

void G_InitNew(skill_t skill, int episode, int map)
{
    if (skill > sk_nightmare)
        skill = sk_nightmare;

    if (episode < 1)
        episode = 1;

    if (_g->gamemode == retail)
    {
        if (episode > 4)
            episode = 4;
    }
    else
        if (_g->gamemode == shareware)
        {
            if (episode > 1)
                episode = 1; // only start episode 1 on shareware
        }
        else
            if (episode > 3)
                episode = 3;

    if (map < 1)
        map = 1;
    if (map > 9 && _g->gamemode != commercial)
        map = 9;

    M_ClearRandom();

    _g->respawnmonsters = skill == sk_nightmare;

    _g->player.playerstate = PST_REBORN;

    _g->usergame = true;                // will be set false if a demo
    _g->automapmode &= ~am_active;
    _g->gameepisode = episode;
    _g->gamemap = map;
    _g->gameskill = skill;

    _g->totalleveltimes = 0; // cph

    G_DoLoadLevel ();
}

//
// DEMO RECORDING
//

#define DEMOMARKER    0x80

void G_ReadDemoTiccmd (ticcmd_t* cmd)
{
    unsigned char at; // e6y: tasdoom stuff

    if (*_g->demo_p == DEMOMARKER)
        G_CheckDemoStatus();      // end of demo data stream
    else if (_g->demoplayback && _g->demo_p + (_g->longtics?5:4) > _g->demobuffer + _g->demolength)
    {
        lprintf(LO_WARN, "G_ReadDemoTiccmd: missing DEMOMARKER\n");
        G_CheckDemoStatus();
    }
    else
    {
        cmd->forwardmove = ((signed char)*_g->demo_p++);
        cmd->sidemove = ((signed char)*_g->demo_p++);
        if (!_g->longtics) {
            cmd->angleturn = ((unsigned char)(at = *_g->demo_p++))<<8;
        } else {
            unsigned int lowbyte = (unsigned char)*_g->demo_p++;
            cmd->angleturn = (((signed int)(*_g->demo_p++))<<8) + lowbyte;
        }
        cmd->buttons = (unsigned char)*_g->demo_p++;
    }
}


/* Same, but read instead of write
 * cph - const byte*'s
 */

const byte *G_ReadOptions(const byte *demo_p)
{
    const byte *target = demo_p + GAME_OPTION_SIZE;

    return target;
}

//
// G_PlayDemo
//

static const char *defdemoname;

void G_DeferedPlayDemo (const char* name)
{
    defdemoname = name;
    _g->gameaction = ga_playdemo;
}

static int demolumpnum = -1;

//e6y: Check for overrun
static boolean CheckForOverrun(const byte *start_p, const byte *current_p, size_t maxsize, size_t size, boolean failonerror)
{
    size_t pos = current_p - start_p;
    if (pos + size > maxsize)
    {
        if (failonerror)
            I_Error("G_ReadDemoHeader: wrong demo header\n");
        else
            return true;
    }
    return false;
}

static const byte* G_ReadDemoHeader(const byte *demo_p, size_t size, boolean failonerror)
{
    skill_t skill;
    int episode, map;

    // e6y
    // The local variable should be used instead of demobuffer,
    // because demobuffer can be uninitialized
    const byte *header_p = demo_p;

    _g->basetic = _g->gametic;  // killough 9/29/98

    // killough 2/22/98, 2/28/98: autodetect old demos and act accordingly.
    // Old demos turn on demo_compatibility => compatibility; new demos load
    // compatibility flag, and other flags as well, as a part of the demo.

    //e6y: check for overrun
    if (CheckForOverrun(header_p, demo_p, size, 1, failonerror))
        return NULL;

    _g->demover = *demo_p++;
    _g->longtics = 0;

    // e6y
    // Handling of unrecognized demo formats
    // Versions up to 1.2 use a 7-byte header - first byte is a skill level.
    // Versions after 1.2 use a 13-byte header - first byte is a demoversion.
    // BOOM's demoversion starts from 200
    if (!((_g->demover >=   0  && _g->demover <=   4) ||
          (_g->demover >= 104  && _g->demover <= 111) ||
          (_g->demover >= 200  && _g->demover <= 214)))
    {
        I_Error("G_ReadDemoHeader: Unknown demo format %d.", _g->demover);
    }

    if (_g->demover < 200)     // Autodetect old demos
    {
        if (_g->demover >= 111) _g->longtics = 1;

        // killough 3/2/98: force these variables to be 0 in demo_compatibility

        // killough 3/6/98: rearrange to fix savegame bugs (moved fastparm,
        // respawnparm, nomonsters flags to G_LoadOptions()/G_SaveOptions())

        if ((skill=_g->demover) >= 100)         // For demos from versions >= 1.4
        {
            //e6y: check for overrun
            if (CheckForOverrun(header_p, demo_p, size, 8, failonerror))
                return NULL;

            skill = *demo_p++;
            episode = *demo_p++;
            map = *demo_p++;
            demo_p++;
            demo_p++;
            demo_p++;
            demo_p++;
            demo_p++;
        }
        else
        {
            //e6y: check for overrun
            if (CheckForOverrun(header_p, demo_p, size, 2, failonerror))
                return NULL;

            episode = *demo_p++;
            map = *demo_p++;
        }

    }
    else    // new versions of demos
    {
        demo_p += 6;               // skip signature;
        switch (_g->demover) {
        case 200: /* BOOM */
        case 201:
            //e6y: check for overrun
            if (CheckForOverrun(header_p, demo_p, size, 1, failonerror))
                return NULL;
            break;
        case 202:
            //e6y: check for overrun
            if (CheckForOverrun(header_p, demo_p, size, 1, failonerror))
                return NULL;

            break;
        case 203:
            /* LxDoom or MBF - determine from signature
     * cph - load compatibility level */
            switch (*(header_p + 2)) {
            case 'B': /* LxDoom */
                /* cph - DEMOSYNC - LxDoom demos recorded in compatibility modes support dropped */
                break;
            case 'M':
                demo_p++;
                break;
            }
            break;
        case 210:
            demo_p++;
            break;
        case 211:
            demo_p++;
            break;
        case 212:
            demo_p++;
            break;
        case 213:
            demo_p++;
            break;
        case 214:
            _g->longtics = 1;
            demo_p++;
            break;
        }
        //e6y: check for overrun
        if (CheckForOverrun(header_p, demo_p, size, 5, failonerror))
            return NULL;

        skill = *demo_p++;
        episode = *demo_p++;
        map = *demo_p++;
        demo_p++;
        demo_p++;

        //e6y: check for overrun
        if (CheckForOverrun(header_p, demo_p, size, GAME_OPTION_SIZE, failonerror))
            return NULL;

        demo_p = G_ReadOptions(demo_p);  // killough 3/1/98: Read game options

        if (_g->demover == 200)              // killough 6/3/98: partially fix v2.00 demos
            demo_p += 256-GAME_OPTION_SIZE;
    }

    //e6y: check for overrun
    if (CheckForOverrun(header_p, demo_p, size, MAXPLAYERS, failonerror))
        return NULL;

    _g->playeringame = *demo_p++;
    demo_p += MIN_MAXPLAYERS - MAXPLAYERS;


    if (_g->gameaction != ga_loadgame) { /* killough 12/98: support -loadgame */
        G_InitNew(skill, episode, map);
    }

    _g->player.cheats = 0;

    return demo_p;
}

void G_DoPlayDemo(void)
{
    char basename[9];

    ExtractFileBase(defdemoname,basename);           // killough
    basename[8] = 0;

    /* cph - store lump number for unlocking later */
    demolumpnum = W_GetNumForName(basename);
    _g->demobuffer = W_CacheLumpNum(demolumpnum);
    _g->demolength = W_LumpLength(demolumpnum);

    _g->demo_p = G_ReadDemoHeader(_g->demobuffer, _g->demolength, true);

    _g->gameaction = ga_nothing;
    _g->usergame = false;

    _g->demoplayback = true;

    _g->starttime = I_GetTime();
}

/* G_CheckDemoStatus
 *
 * Called after a death or level completion to allow demos to be cleaned up
 * Returns true if a new demo loop action will take place
 */
boolean G_CheckDemoStatus (void)
{
    if (_g->timingdemo)
    {
        int endtime = I_GetTime();
        // killough -- added fps information and made it work for longer demos:
        unsigned realtics = endtime-_g->starttime;
        /* Integer FPS: the V810 libgcc has no soft double-float, and -timedemo is
         * never run on this target anyway. */
        unsigned fps = realtics ? ((unsigned)_g->gametic * TICRATE) / realtics : 0;
        I_Error ("Timed %u gametics in %u realtics = %u fps",
                 (unsigned) _g->gametic, realtics, fps);
    }

    if (_g->demoplayback)
    {
        if (_g->singledemo)
            exit(0);  // killough

        if (demolumpnum != -1)
        {
            demolumpnum = -1;
        }
        G_ReloadDefaults();    // killough 3/1/98
        D_AdvanceDemo ();
        return true;
    }
    return false;
}
