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
 *    Network client. Passes information to/from server, staying
 *    synchronised.
 *    Contains the main wait loop, waiting for network input or
 *    time before doing the next tic.
 *    Rewritten for LxDoom, but based around bits of the old code.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include "doomtype.h"
#include "doomstat.h"
#include "d_net.h"
#include "z_zone.h"

#include "d_main.h"
#include "pcfx_time.h"
#include "g_game.h"
#include "m_menu.h"

#include "protocol.h"
#include "i_network.h"
#include "i_system.h"
#include "i_main.h"
#include "i_video.h"
#include "i_system_e32.h"
#include "lprintf.h"

#include "global_data.h"

#include "pcfx_present.h"  /* service a deferred page flip while waiting for tics */


void D_InitNetGame (void)
{
    _g->playeringame = true;
}

void D_BuildNewTiccmds(void)
{
    int newtics = I_GetTime() - _g->lastmadetic;
    _g->lastmadetic += newtics;

    /* Every I_StartTic() below runs back to back, so only the first one sees
     * any mouse motion (the port's counter is cleared by each read).  Tell the
     * input layer how many tics have to share that one sample. */
    I_SetTicBurst(newtics);

    while (newtics--)
    {
        I_StartTic();
        if (_g->maketic - _g->gametic > 3)
            break;

        G_BuildTiccmd(&_g->netcmd);
        _g->maketic++;
    }
}

void TryRunTics (void)
{
    int runtics;
    int entertime = I_GetTime();
#if defined(DEV_TIC_PROFILE) || defined(DEV_FRAME_TRACE_WORK)
    uint64_t _tw = itu_ticks();
#endif

    // Wait for tics to run
    while (1)
    {

        D_BuildNewTiccmds();

        runtics = (_g->maketic) - _g->gametic;
        if (runtics <= 0)
        {
            /* Tic-bound frame: the wait is idle anyway — service the previous
             * frame's deferred page flip the moment its raster window opens. */
            pcfx_present_tick();
            if (I_GetTime() - entertime > 10)
            {
                M_Ticker();
                return;
            }
        }
        else
            break;
    }
#if defined(DEV_TIC_PROFILE) || defined(DEV_FRAME_TRACE_WORK)
    {
        uint32_t _spin = (uint32_t)(itu_ticks() - _tw);
#ifdef DEV_FRAME_TRACE_WORK
        g_rp_tt_wait = _spin;
#endif
#ifdef DEV_TIC_PROFILE
        g_rpa_tt_wait += _spin;
        g_rpa_tt_runtics += (uint32_t)(runtics > 0 ? runtics : 0);
#endif
    }
#endif

    while (runtics-- > 0)
    {

        if (_g->advancedemo)
            D_DoAdvanceDemo ();

#ifdef DEV_TIC_PROFILE
        uint64_t _tm = itu_ticks();
#endif
        M_Ticker ();
#ifdef DEV_TIC_PROFILE
        g_rpa_tt_mticker += (uint32_t)(itu_ticks() - _tm);
#endif
#ifdef DEV_BENCH_AUTOMOVE
        /* This single-player port retains only one pending netcmd, not Doom's
         * usual ticcmd ring.  Rebuild the deterministic command for every tic
         * in a catch-up batch so renderer speed cannot alter the route. */
        G_BuildTiccmd(&_g->netcmd);
#endif
#ifdef DEV_BENCH_FREEZE_TIC
        /* Let the deterministic movement benchmark reach a fixed world state,
         * then keep consuming timer tics without advancing actors or geometry.
         * Both faster and slower renderers can consequently draw the identical
         * state repeatedly for framebuffer-equivalence checks. */
        if (_g->gametic < (DEV_BENCH_FREEZE_TIC))
#endif
        {
#ifdef DEV_TIC_PROFILE
            uint64_t _tg = itu_ticks();
#endif
            G_Ticker ();
#ifdef DEV_TIC_PROFILE
            g_rpa_tt_gticker += (uint32_t)(itu_ticks() - _tg);
#endif
        }
#ifdef DEV_FORCE_EXIT
        /* Dev hook: end the level at a fixed tic (EXTRA="-DDEV_FORCE_EXIT=350")
         * so headless runs reach the intermission deterministically — used with
         * a --commands file pressing A to step its stages. Fires once (gametic
         * is monotonic across levels). */
        if (_g->gametic == (DEV_FORCE_EXIT)) G_ExitLevel();
#endif
        _g->gametic++;
        pcfx_present_tick();   /* between catch-up tics: ~2-3 ms apart */
    }
}
