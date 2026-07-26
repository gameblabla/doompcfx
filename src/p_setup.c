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
 *  Do all the WAD I/O, get map description,
 *  set up initial state and misc. LUTs.
 *
 *-----------------------------------------------------------------------------*/

#include <math.h>

#include "doomstat.h"
#include "m_bbox.h"
#include "g_game.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_things.h"
#include "p_maputl.h"
#include "p_map.h"
#include "p_setup.h"
#include "p_spec.h"
#include "p_tick.h"
#include "p_enemy.h"
#include "s_sound.h"
#include "lprintf.h" //jff 10/6/98 for debug outputs
#include "v_video.h"

#include "global_data.h"

/* The map lumps below are baked to these exact runtime-struct layouts by
 * tools/bake_wad.py and cast straight from ROM (P_Load* divide by sizeof).
 * If the compiler's sizeof ever drifts from the baker, fail loudly here. */
_Static_assert(sizeof(vertex_t) == 8,  "vertex_t must be 8 bytes for ROM cast");
_Static_assert(sizeof(seg_t)    == 32, "seg_t must be 32 bytes for ROM cast");
_Static_assert(sizeof(line_t)   == 56, "line_t must be 56 bytes for ROM cast");
_Static_assert(sizeof(mapsidedef_t) == 12, "mapsidedef_t must be 12 bytes for ROM cast");

//
// P_LoadVertexes
//
// killough 5/3/98: reformatted, cleaned up
//
static void P_LoadVertexes (int lump)
{
  // VERTEXES are baked as native-BE vertex_t (fixed_t x,y) — cast straight from ROM.
  _g->numvertexes = W_LumpLength(lump) / sizeof(vertex_t);
  _g->vertexes = W_CacheLumpNum(lump);
}

//
// P_LoadSegs
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadSegs (int lump)
{
    // SEGS are baked as native-BE seg_t (inlined vertices + resolved indices) — cast
    // straight from ROM; no runtime expansion.
    int numsegs = W_LumpLength(lump) / sizeof(seg_t);
    _g->segs = (const seg_t *)W_CacheLumpNum(lump);

    if (!numsegs)
      I_Error("P_LoadSegs: no segs in level");
}

//
// P_LoadSubsectors
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadSubsectors (int lump)
{
  /* cph 2006/07/29 - make data a const mapsubsector_t *, so the loop below is simpler & gives no constness warnings */
  const mapsubsector_t *data;
  int  i;

  _g->numsubsectors = W_LumpLength (lump) / sizeof(mapsubsector_t);
  _g->subsectors = Z_Calloc(_g->numsubsectors,sizeof(subsector_t),PU_LEVEL,0);
  data = (const mapsubsector_t *)W_CacheLumpNum(lump);

  if ((!data) || (!_g->numsubsectors))
    I_Error("P_LoadSubsectors: no subsectors in level");

  for (i=0; i<_g->numsubsectors; i++)
  {
    _g->subsectors[i].numlines  = (unsigned short)SHORT(data[i].numsegs );
    _g->subsectors[i].firstline = (unsigned short)SHORT(data[i].firstseg);
  }
}

//
// P_LoadSectors
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadSectors (int lump)
{
  const byte *data; // cph - const*
  int  i;

  _g->numsectors = W_LumpLength (lump) / sizeof(mapsector_t);
  _g->sectors = Z_Calloc (_g->numsectors,sizeof(sector_t),PU_LEVEL,0);
  data = W_CacheLumpNum (lump); // cph - wad lump handling updated

  for (i=0; i<_g->numsectors; i++)
    {
      sector_t *ss = _g->sectors + i;
      const mapsector_t *ms = (const mapsector_t *) data + i;

      ss->floorheight = SHORT(ms->floorheight)<<FRACBITS;
      ss->ceilingheight = SHORT(ms->ceilingheight)<<FRACBITS;
      ss->floorpic = R_FlatNumForName(ms->floorpic);
      ss->ceilingpic = R_FlatNumForName(ms->ceilingpic);

      ss->lightlevel = SHORT(ms->lightlevel);
      ss->special = SHORT(ms->special);
      ss->oldspecial = SHORT(ms->special);
      ss->tag = SHORT(ms->tag);

      ss->thinglist = NULL;
      ss->touching_thinglist = NULL;            // phares 3/14/98
    }
}


//
// P_LoadNodes
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadNodes (int lump)
{
  numnodes = W_LumpLength (lump) / sizeof(mapnode_t);
  nodes = W_CacheLumpNum (lump); // cph - wad lump handling updated

  if ((!nodes) || (!numnodes))
  {
    // allow trivial maps
    if (_g->numsubsectors == 1)
      lprintf(LO_INFO,
          "P_LoadNodes: trivial map (no nodes, one subsector)\n");
    else
      I_Error("P_LoadNodes: no nodes in level");
  }
}


/*
 * P_LoadThings
 *
 * killough 5/3/98: reformatted, cleaned up
 * cph 2001/07/07 - don't write into the lump cache, especially non-idepotent
 * changes like byte order reversals. Take a copy to edit.
 */

// The thing pool is the largest per-level Z_Calloc (e.g. E1M6 = 463 things *
// 124-byte mobj_t = 57412 B). Allocate it separately, EARLY in P_SetupLevel
// (before P_LoadSideDefs2 loads textures), so it lands in the still-contiguous
// heap instead of failing to find a hole in the texture-fragmented one. Spawning
// stays late (needs the fully set-up map); this only reserves the buffer.
static void P_AllocThings (int lump)
{
    int numthings = W_LumpLength (lump) / sizeof(mapthing_t);

    _g->thingPool = Z_Calloc(numthings, sizeof(mobj_t), PU_LEVEL, NULL);
    _g->thingPoolSize = numthings;

    for(int i = 0; i < numthings; i++)
    {
        _g->thingPool[i].type = MT_NOTHING;
    }
}

static void P_LoadThings (int lump)
{
    int  i, numthings = W_LumpLength (lump) / sizeof(mapthing_t);
    const mapthing_t *data = W_CacheLumpNum (lump);

    if ((!data) || (!numthings))
        I_Error("P_LoadThings: no things in level");

    for (i=0; i<numthings; i++)
    {
        // On-disk mapthing is LE; swap into a writable copy before spawning.
        mapthing_t mt = data[i];
        mt.x       = SHORT(mt.x);
        mt.y       = SHORT(mt.y);
        mt.angle   = SHORT(mt.angle);
        mt.type    = SHORT(mt.type);
        mt.options = SHORT(mt.options);

        if (!P_IsDoomnumAllowed(mt.type))
            continue;

        // Do spawn all other stuff.
        P_SpawnMapThing(&mt);
    }
}

//
// P_LoadLineDefs
// Also counts secret lines for intermissions.
//        ^^^
// ??? killough ???
// Does this mean secrets used to be linedef-based, rather than sector-based?
//
// killough 4/4/98: split into two functions, to allow sidedef overloading
//
// killough 5/3/98: reformatted, cleaned up

static void P_LoadLineDefs (int lump)
{
    // LINEDEFS are baked as native-BE line_t (inlined vertices, dx/dy/bbox/slopetype
    // precomputed) — cast straight from ROM. Only the mutable linedata_t lives in RAM.
    _g->numlines = W_LumpLength (lump) / sizeof(line_t);
    _g->lines = W_CacheLumpNum(lump);
    _g->linedata = Z_Calloc(_g->numlines,sizeof(linedata_t),PU_LEVEL,0);

    for (int i = 0; i < _g->numlines; i++)
        _g->linedata[i].special = _g->lines[i].const_special;
}

// killough 4/4/98: delay using sidedefs until they are loaded
// killough 5/3/98: reformatted, cleaned up

static void P_LoadLineDefs2(int lump)
{
    /*
  int i = _g->numlines;
  register line_t *ld = _g->lines;
  for (;i--;ld++)
    {
      ld->frontsector = _g->sides[ld->sidenum[0]].sector; //e6y: Can't be NO_INDEX here
      ld->backsector  = ld->sidenum[1]!=NO_INDEX ? _g->sides[ld->sidenum[1]].sector : 0;
    }
    */
}

//
// P_LoadSideDefs
//
// killough 4/4/98: split into two functions

static void P_LoadSideDefs (int lump)
{
  _g->numsides = W_LumpLength(lump) / sizeof(mapsidedef_t);
  _g->sides = Z_Calloc(_g->numsides,sizeof(side_t),PU_LEVEL,0);
}

// killough 4/4/98: delay using texture names until
// after linedefs are loaded, to allow overloading.
// killough 5/3/98: reformatted, cleaned up

static void P_LoadSideDefs2(int lump)
{
    const byte *data = W_CacheLumpNum(lump); // cph - const*, wad lump handling updated
    int  i;

    for (i=0; i<_g->numsides; i++)
    {
        register const mapsidedef_t *msd = (const mapsidedef_t *) data + i;
        register side_t *sd = _g->sides + i;
        register sector_t *sec;

        /* SIDEDEFS are baked native-BE with texture NAMES already resolved to indices
         * (see tools/bake_wad.py) — read the short fields raw, no SHORT() swap. */
        sd->textureoffset = msd->textureoffset;
        sd->rowoffset = msd->rowoffset;

        { /* cph 2006/09/30 - catch out-of-range sector numbers; use sector 0 instead */
            unsigned short sector_num = msd->sector;
            if (sector_num >= _g->numsectors)
            {
                lprintf(LO_WARN,"P_LoadSideDefs2: sidedef %i has out-of-range sector num %u\n", i, sector_num);
                sector_num = 0;
            }
            sd->sector = sec = &_g->sectors[sector_num];
        }

        sd->midtexture    = msd->midtexture;
        sd->toptexture    = msd->toptexture;
        sd->bottomtexture = msd->bottomtexture;

        // Descriptors are immutable and built contiguously by R_InitTextures;
        // these calls do not touch patch data, including on the packed-map path.
        R_GetTexture(sd->midtexture);
        R_GetTexture(sd->toptexture);
        R_GetTexture(sd->bottomtexture);
    }
}

//
// jff 10/6/98
// New code added to speed up calculation of internal blockmap
// Algorithm is order of nlines*(ncols+nrows) not nlines*ncols*nrows
//

#define blkshift 7               /* places to shift rel position for cell num */
#define blkmask ((1<<blkshift)-1)/* mask for rel position within cell */
#define blkmargin 0              /* size guardband around map used */
                                 // jff 10/8/98 use guardband>0
                                 // jff 10/12/98 0 ok with + 1 in rows,cols

typedef struct linelist_t        // type used to list lines in each block
{
  long num;
  struct linelist_t *next;
} linelist_t;

//
// P_LoadBlockMap
//
// killough 3/1/98: substantially modified to work
// towards removing blockmap limit (a wad limitation)
//
// killough 3/30/98: Rewritten to remove blockmap limit,
// though current algorithm is brute-force and unoptimal.
//

static void P_LoadBlockMap (int lump)
{
    // BLOCKMAP is baked native-BE by tools/bake_wad.py — read raw, no SHORT().
    _g->blockmaplump = W_CacheLumpNum(lump);

    _g->bmaporgx = _g->blockmaplump[0]<<FRACBITS;
    _g->bmaporgy = _g->blockmaplump[1]<<FRACBITS;
    _g->bmapwidth = _g->blockmaplump[2];
    _g->bmapheight = _g->blockmaplump[3];


    // clear out mobj chains - CPhipps - use calloc
    _g->blocklinks = Z_Calloc (_g->bmapwidth*_g->bmapheight,sizeof(*_g->blocklinks),PU_LEVEL,0);

    _g->blockmap = _g->blockmaplump+4;
}

//
// P_LoadReject - load the reject table, padding it if it is too short
// totallines must be the number returned by P_GroupLines()
// an underflow will be padded with zeroes, or a doom.exe z_zone header
// 
// this function incorporates e6y's RejectOverrunAddInt code:
// e6y: REJECT overrun emulation code
// It's emulated successfully if the size of overflow no more than 16 bytes.
// No more desync on teeth-32.wad\teeth-32.lmp.
// http://www.doomworld.com/vb/showthread.php?s=&threadid=35214

static void P_LoadReject(int lumpnum)
{
  _g->rejectlump = lumpnum + ML_REJECT;
  _g->rejectmatrix = W_CacheLumpNum(_g->rejectlump);
}

//
// P_GroupLines
// Builds sector line lists and subsector sector numbers.
// Finds block bounding boxes for sectors.
//
// killough 5/3/98: reformatted, cleaned up
// cph 18/8/99: rewritten to avoid O(numlines * numsectors) section
// It makes things more complicated, but saves seconds on big levels
// figgi 09/18/00 -- adapted for gl-nodes

// cph - convenient sub-function
static void P_AddLineToSector(const line_t* li, sector_t* sector)
{
  sector->lines[sector->linecount++] = li;
}

// modified to return totallines (needed by P_LoadReject)
static int P_GroupLines (void)
{
    register const line_t *li;
    register sector_t *sector;
    int i,j, total = _g->numlines;

    // figgi
    for (i=0 ; i<_g->numsubsectors ; i++)
    {
        const seg_t *seg = &_g->segs[_g->subsectors[i].firstline];
        _g->subsectors[i].sector = NULL;
        for(j=0; j<_g->subsectors[i].numlines; j++)
        {
            if(seg->sidenum != NO_INDEX)
            {
                _g->subsectors[i].sector = _g->sides[seg->sidenum].sector;
                break;
            }
            seg++;
        }
        if(_g->subsectors[i].sector == NULL)
            I_Error("P_GroupLines: Subsector a part of no sector!\n");
    }

    // count number of lines in each sector
    for (i=0,li=_g->lines; i<_g->numlines; i++, li++)
    {
        LN_FRONTSECTOR(li)->linecount++;
        if (LN_BACKSECTOR(li) && LN_BACKSECTOR(li) != LN_FRONTSECTOR(li))
        {
            LN_BACKSECTOR(li)->linecount++;
            total++;
        }
    }

    {  // allocate line tables for each sector
        const line_t **linebuffer = Z_Malloc(total*sizeof(line_t *), PU_LEVEL, 0);

        // e6y: REJECT overrun emulation code
        // moved to P_LoadReject

        for (i=0, sector = _g->sectors; i<_g->numsectors; i++, sector++)
        {
            sector->lines = linebuffer;
            linebuffer += sector->linecount;
            sector->linecount = 0;
        }
    }

    // Enter those lines
    for (i=0,li=_g->lines; i<_g->numlines; i++, li++)
    {
        P_AddLineToSector(li, LN_FRONTSECTOR(li));
        if (LN_BACKSECTOR(li) && LN_BACKSECTOR(li) != LN_FRONTSECTOR(li))
            P_AddLineToSector(li, LN_BACKSECTOR(li));
    }

    for (i=0, sector = _g->sectors; i<_g->numsectors; i++, sector++)
    {
        fixed_t bbox[4];
        M_ClearBox(bbox);

        for(int l = 0; l < sector->linecount; l++)
        {
            M_AddToBox (bbox, sector->lines[l]->v1.x, sector->lines[l]->v1.y);
            M_AddToBox (bbox, sector->lines[l]->v2.x, sector->lines[l]->v2.y);
        }

        sector->soundorg.x = bbox[BOXRIGHT]/2+bbox[BOXLEFT]/2;
        sector->soundorg.y = bbox[BOXTOP]/2+bbox[BOXBOTTOM]/2;
    }

    return total; // this value is needed by the reject overrun emulation code
}


void P_FreeLevelData()
{
    R_ResetPlanes();

    Z_FreeTags(PU_LEVEL, PU_PURGELEVEL-1);

    // The precache compressed-graphics arena was PU_LEVEL and is now gone; drop the
    // w_comp[] pointers into it before anything (e.g. next level's texture load)
    // dereferences them.
    W_LevelGraphicsFreed();

    Z_Free(_g->braintargets);
    _g->braintargets = NULL;
    _g->numbraintargets_alloc = _g->numbraintargets = 0;
}

//
// P_SetupLevel
//
// killough 5/3/98: reformatted, cleaned up

void P_SetupLevel(int episode, int map, int playermask, skill_t skill)
{
    int   i;
    char  lumpname[9];
    int   lumpnum;

    _g->totallive = _g->totalkills = _g->totalitems = _g->totalsecret = 0;
    _g->wminfo.partime = 180;

    for (i=0; i<MAXPLAYERS; i++)
        _g->player.killcount = _g->player.secretcount = _g->player.itemcount = 0;

    // Initial height of PointOfView will be set by player think.
    _g->player.viewz = 1;

    // Make sure all sounds are stopped before Z_FreeTags.
    S_Start();

    // Loading: CD reads are expected here (behind the loading bar). Silence the
    // "CD read during play" warning until precache re-arms it below.
    W_EndGameplay();

    P_FreeLevelData();

    // Free the intermission/finale CD background buffer (the ~61 KB PU_STATIC s_buf in
    // pcfx_cdasset.c) NOW, before the arena reserve measures the heap. It is normally
    // freed by pcfx_cd_background_hide() from D_Display's first in-level frame — but that
    // runs AFTER P_SetupLevel, so on a level TRANSITION it is still resident here, pinned
    // mid-heap: it splits the free space so Z_LargestFreeBlock sees only ~710 KB (of
    // ~1.28 MB total free) and the map pack fails its one-chunk fit -> the load drops to
    // the ~24 s scattered seek-storm. Freeing it here coalesces the heap back to one big
    // block so EVERY transition takes the fast one-chunk pack path (matching a direct warp).
    { extern void pcfx_cd_background_hide(void); pcfx_cd_background_hide(); }

    // Work out this level's map lump number up-front (a resident directory lookup,
    // no allocation) so the arena reserve can size itself to how big THIS map's
    // PU_LEVEL data will be — a light map leaves a big arena (whole graphics pool
    // resident, zero in-game CD); a heavy map gives the arena back so the map fits.
    {
        char lname[9];
        if (_g->gamemode == commercial) sprintf(lname, "MAP%02d", map);
        else                            sprintf(lname, "E%dM%d", episode, map);
        int mlump = W_CheckNumForName(lname);

        // Reserve the compressed-graphics arena NOW, while the heap is emptiest
        // (only whole-run statics resident) so it can be one contiguous block.
        W_PrecacheReserve(mlump);

        // If this map has a contiguous CD pack, stream the WHOLE level — geometry
        // lumps (into lumpcache) then graphics (into the arena) — in one ~0-seek
        // sweep, right here while the heap is emptiest and BEFORE anything reads the
        // CD out of order. The geometry reads below then hit RAM (zero CD), and
        // R_PrecacheLevel has nothing left to do. This is the "collapse the level
        // load into one read" fix (see W_LoadMapPack).
        W_LoadMapPack();
    }

    // NB: no sky texture is built/precached — the PC-FX sky is the HuC6271 RAINBOW
    // hardware layer (pcfx_sky.bin, baked offline from SKY1 and DMA'd to KRAM); the
    // software renderer writes sky columns TRANSPARENT so the RAINBOW shows through
    // (r_hotpath.c ~1631) and never samples _g->skytexture. Building it (and packing
    // its patches, see R_PrecacheLevel) was dead weight — cut for a smaller pack.

    if (_g->rejectlump != -1)
    { // cph - unlock the reject table
        _g->rejectlump = -1;
    }

    P_InitThinkers();

    // if working with a devlopment map, reload it
    //    W_Reload ();     killough 1/31/98: W_Reload obsolete

    // find map name
    if (_g->gamemode == commercial)
    {
        sprintf(lumpname, "MAP%02d", map);           // killough 1/24/98: simplify
    }
    else
    {
        sprintf(lumpname, "E%dM%d", episode, map);   // killough 1/24/98: simplify
    }

    lumpnum = W_GetNumForName(lumpname);

    _g->leveltime = 0; _g->totallive = 0;

    // Load order is tuned to minimise zone FRAGMENTATION on a DIRECT (non-transition)
    // level load, where the heap is NOT first emptied by freeing a previous level
    // (e.g. a dev warp or loading a save mid-episode). Principle: place every LARGE
    // PU_LEVEL block (which is pinned all level) into the still-CONTIGUOUS heap FIRST,
    // before the remaining smaller structures. Texture descriptors are immutable
    // startup metadata now, but this order also keeps non-packed/modified-WAD level
    // allocations predictable and leaves the cache tail contiguous.
    //
    // Dependency notes (all verified): the map loaders don't need textures resolved
    // (bake-pre-resolved, zero-copy or plain Z_Calloc); P_LoadLineDefs2 is a no-op;
    // P_LoadSideDefs2 needs only sides[]+sectors[]; P_LoadNodes' trivial-map check
    // needs subsectors first; P_LoadReject only caches its lump (the "totallines from
    // P_GroupLines" comment on it is vestigial — the value is unused) so it can move
    // up; P_AllocThings only reserves the thing pool (spawning stays late, below).
    P_LoadReject    (lumpnum);                  // reject matrix (grows ~numsectors^2/8)
    P_LoadSegs      (lumpnum + ML_SEGS);        // biggest fixed lump (numsegs*32)
    P_LoadSubsectors(lumpnum + ML_SSECTORS);    // before Nodes (trivial-map check)
    P_LoadNodes     (lumpnum + ML_NODES);
    P_LoadBlockMap  (lumpnum + ML_BLOCKMAP);

    P_LoadVertexes  (lumpnum+ML_VERTEXES);
    P_LoadSectors   (lumpnum+ML_SECTORS);
    P_LoadSideDefs  (lumpnum+ML_SIDEDEFS);     // allocs sides[] (needed by SideDefs2)
    P_LoadLineDefs  (lumpnum+ML_LINEDEFS);
    P_AllocThings   (lumpnum+ML_THINGS);       // reserve the big thing pool (numthings*sizeof(mobj_t))

    P_LoadSideDefs2 (lumpnum+ML_SIDEDEFS);     // textures LAST (fragmentation now harmless)
    P_LoadLineDefs2 (lumpnum+ML_LINEDEFS);

    P_GroupLines();

    // Note: you don't need to clear player queue slots --
    // a much simpler fix is in g_game.c -- killough 10/98

    /* cph - reset all multiplayer starts */
    memset(_g->playerstarts,0,sizeof(_g->playerstarts));

    for (i = 0; i < MAXPLAYERS; i++)
        _g->player.mo = NULL;

    P_MapStart();

    P_LoadThings(lumpnum+ML_THINGS);

    {
        if (_g->playeringame && !_g->player.mo)
            I_Error("P_SetupLevel: missing player %d start\n", i+1);
    }

    // killough 3/26/98: Spawn icon landings:
    if (_g->gamemode==commercial)
        P_SpawnBrainTargets();

    // set up world state
    P_SpawnSpecials();

    P_MapEnd();

    // PC-FX: with the map + all its things spawned, pull every graphic this level
    // can show into resident RAM so the renderer never reads the CD in-game (the
    // cause of the freezes). This is the last CD access until the next level.
    R_PrecacheLevel();
    Z_DumpUsage("postprecache");
    W_BeginGameplay();

    // Heap is now fully committed for this level; hand spare RAM to the optional
    // pre-lit flat cache. It is purgeable under later texture-cache pressure;
    // tight maps with no spare get no cache and keep the 2-load span drawer.
    R_LitFlatLevelInit();
}

//
// P_Init
//
void P_Init (void)
{
    lprintf(LO_INFO, "P_InitSwitchList");
    P_InitSwitchList();

    lprintf(LO_INFO, "P_InitPicAnims");
    P_InitPicAnims();

    lprintf(LO_INFO, "R_InitSprites");
    R_InitSprites(sprnames);
}
