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
 *      WAD I/O functions.
 *
 *-----------------------------------------------------------------------------*/


#ifndef __W_WAD__
#define __W_WAD__

#include "doomtype.h"

//
// TYPES
//

typedef struct
{
  char identification[4];                  // Should be "IWAD" or "PWAD".
  int  numlumps;
  int  infotableofs;
} wadinfo_t;

typedef struct
{
  int  filepos;
  int  size;
  char name[8];
} filelump_t;


// killough 4/17/98: if W_CheckNumForName() called with only
// one argument, pass ns_global as the default namespace

void W_Init(void); // CPhipps - uses the above array

int PUREFUNC W_CheckNumForName(const char* name);   // killough 4/17/98
int PUREFUNC W_GetNumForName (const char* name);
const char* PUREFUNC W_GetNameForNum(int lump);


int PUREFUNC W_LumpLength (int lump);

// PC-FX: total lump count (for IdentifyVersion scanning the CD directory).
int PUREFUNC W_NumLumps (void);

// CPhipps - modified for 'new' lump locking
const void* PUREFUNC W_CacheLumpNum (int lump);

// CPhipps - convenience macros
#define W_CacheLumpName(name) W_CacheLumpNum(W_GetNumForName(name))

// PC-FX: precache an evictable (graphics) lump's compressed payload into resident
// RAM at level-load, so gameplay decompresses it from RAM and never reads the CD.
// Bracket a precache pass with W_PrecacheBegin/End (allocates a shared staging
// buffer). W_Begin/EndGameplay gate the "CD read during play" warning.
// `prio` orders arena fill when the pool overflows the resident budget: higher
// fills first, so lower-priority lumps are the ones dropped (and CD-load on first
// use). W_PC_FLAT (floors/ceilings) / W_PC_PATCH (wall textures) / W_PC_SPRITE (map
// things + in-hand weapon) are all reliably on screen; W_PC_EFFECT (runtime puff/
// blood/projectile/fog sprites) is the deliberate overflow victim — a transient
// miss there is the least visible. Within a priority the arena fills in disc order
// so the CD reads coalesce into sequential sweeps (see W_PrecacheEnd).
#define W_PC_FLAT   4
#define W_PC_PATCH  3
#define W_PC_SPRITE 2
#define W_PC_EFFECT 1
void W_PrecacheReserve(int maplump);
void W_PrecacheBegin(void);
void W_PrecacheLumpNum(int lump, int prio);
void W_PrecacheEnd(void);
// Try to stream this map's pre-assembled contiguous CD pack into the arena as one
// block (~0 seek). Called by W_PrecacheReserve BEFORE the map lumps load, so the pack's
// texture patches are resident for P_LoadSideDefs2. Returns 1 if a pack existed and was
// loaded, 0 to fall back to the scattered reserve+marking path. Uses the map marker
// passed to W_PrecacheReserve.
int W_LoadMapPack(void);
// Stream the BOOT asset pack (the whole-run PU_STATIC UI set — font, HUD, palette,
// colormap, TEXTURE1) from pcfx_mappacks.bin into permanent lump cache in one ~0-seek
// contiguous sweep at W_Init, so the boot-to-title path never seek-storms the CD for
// its ~140 scattered resident lumps. No-op if the pack lacks a "BOOT" entry (falls
// back to the scattered preload_static_run). Called at the end of W_Init.
void W_LoadBootPack(void);
// Make every still-uncached PU_STATIC lump resident at boot, before the first level
// reserves its arena. These lumps are never freed, so loading them here costs no extra
// RAM — it only keeps them from being stranded MID-HEAP when they would otherwise be
// cached on first use (first HUD frame / intermission / menu), where they become
// unpurgeable walls that split the free block and drop level loads off the fast pack
// path. Call at the end of boot, after the UI init that requests the common lumps.
void W_PreloadStatics(void);
// -DGEN_BOOTPACK_MANIFEST only: dump the ground-truth boot-resident lump set (every
// whole-run PU_STATIC lump the engine requested via W_CacheLumpNum from W_Init to the
// first title frame) as a `MAPPACK BOOT n` block the pack builder consumes. Called
// once from D_Display.
void W_BootManifestDump(void);
int  W_MapPackLoaded(void);   // 1 if this level's pool came from a pack (skip precache)
void W_ReportLoadCost(void);  // log the level's total CD-load cost (call after all reads)
void W_LevelGraphicsFreed(void);
void W_BeginGameplay(void);
void W_EndGameplay(void);

void ExtractFileBase(const char *, char *);       // killough

#endif
