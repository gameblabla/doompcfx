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
 *      Rendering main loop and setup functions,
 *       utility functions (BSP, geometry, trigonometry).
 *      See tables.c, too.
 *
 *-----------------------------------------------------------------------------*/

//This is to keep the codesize under control.
//This whole file needs to fit within IWRAM.
#pragma GCC optimize ("Os")


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef GBA
    #include <time.h>
#endif

#include "doomstat.h"
#include "d_net.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_things.h"
#include "r_plane.h"
#include "r_draw.h"
#include "m_bbox.h"
#include "r_sky.h"
#include "v_video.h"
#include "lprintf.h"
#include "st_stuff.h"
#include "i_main.h"
#include "i_system.h"
#include "g_game.h"
#include "m_random.h"

#include "global_data.h"

#include "gba_functions.h"

#include "pcfx_time.h"   /* render profiler (rprof_sample / phase globals) */

#include "pcfx_time.h"   /* PC-FX platform millisecond clock (I_GetTime) */

#include "pcfx_weapon.h"  /* VDC hardware weapon sprites */
#include "pcfx_weapons.h" /* generated weapon frame/cell/palette tables */
#include "pcfx_present.h" /* pcfx_present_tick(): deferred flip poll between draw units */


//#define static

//*****************************************
//These are unused regions of VRAM.
//We can store things in here to free space
//in IWRAM.
//*****************************************

/* PC-FX: GBADoom packed these clip/projection tables into fixed-size "VRAM"
 * scratch slots sized for the old 120x160 screen. At 256x240 they must be full
 * MAX_SCREENWIDTH/HEIGHT arrays, and the projection tables (yslope/distscale/
 * xtoviewangle/viewangletox) must be COMPUTED for the active resolution rather
 * than copied from 120x160 baked consts (see R_SetupViewScaling below). We keep
 * the original pointer types so extern declarations elsewhere still match. */
static byte vram3_spare[1024];

//512 bytes: column cache index table (128 entries, not resolution-dependent).
static unsigned int* columnCacheEntries = (unsigned int*)&vram3_spare[0];

//Per-column clip/aux tables (MAX_SCREENWIDTH), backed by real storage.
static short floorclip_storage[MAX_SCREENWIDTH];             short* floorclip = floorclip_storage;
static short ceilingclip_storage[MAX_SCREENWIDTH];           short* ceilingclip = ceilingclip_storage;
static short screenheightarray_storage[MAX_SCREENWIDTH];     short* screenheightarray = screenheightarray_storage;
static short negonearray_storage[MAX_SCREENWIDTH];           short* negonearray = negonearray_storage;
static short wipe_y_lookup_storage[MAX_SCREENWIDTH];         short* wipe_y_lookup = wipe_y_lookup_storage;
static vissprite_t* vissprite_ptrs_storage[MAXVISSPRITES];  vissprite_t** vissprite_ptrs = vissprite_ptrs_storage;

//Projection tables, computed at runtime for the active resolution.
static fixed_t yslope_storage[MAX_SCREENHEIGHT];
static fixed_t distscale_storage[MAX_SCREENWIDTH];
static angle_t xtoviewangle_storage[MAX_SCREENWIDTH + 2];
/* R_SetupViewScaling clamps every entry to [-1, SCREENWIDTH + 1] while the
 * inverse table is built, then to [0, SCREENWIDTH] for rendering.  A signed
 * 16-bit element therefore preserves the sentinel and every consumed value,
 * while returning 8 KiB of resident RAM.  V810 ld.h sign-extends, so indexed
 * reads need no unsigned-widening instruction. */
static short   viewangletox_storage[FINEANGLES / 2];

#define yslope yslope_storage
#define distscale distscale_storage
#define xtoviewangle xtoviewangle_storage
#define viewangletox viewangletox_storage

//*****************************************
//Column cache stuff.
//GBA has 16kb of Video Memory for columns
//*****************************************

#ifndef GBA
static byte columnCache[128*128];
#else
    #define columnCache ((byte*)0x6014000)
#endif



//*****************************************
//Globals.
//*****************************************

int numnodes;
const mapnode_t *nodes;

fixed_t  viewx, viewy, viewz;

angle_t  viewangle;

#ifdef SOLIDCOL_BYTE_REFERENCE
static byte solidcol[MAX_SCREENWIDTH];
#else
/* One bit per logical screen column.  The original byte array made every BSP
 * bbox test and solid-wall clip linearly walk up to 128 bytes.  Four native
 * words let those operations skip 32 columns at a time while preserving the
 * exact half-open [first,last) clipping semantics. */
#define SOLIDCOL_WORDS ((MAX_SCREENWIDTH + 31) / 32)
static uint32_t solidcol[SOLIDCOL_WORDS];

static inline unsigned R_LowestSetBit(uint32_t bits)
{
    unsigned bit = 0;
    if (!(bits & 0xffffu)) { bits >>= 16; bit += 16; }
    if (!(bits & 0x00ffu)) { bits >>= 8;  bit += 8; }
    if (!(bits & 0x000fu)) { bits >>= 4;  bit += 4; }
    if (!(bits & 0x0003u)) { bits >>= 2;  bit += 2; }
    if (!(bits & 0x0001u)) bit++;
    return bit;
}

static inline boolean R_SolidColumn(int column)
{
    return (solidcol[(unsigned)column >> 5] >> (column & 31)) & 1u;
}

/* Return the first column in [first,last) with the requested state, or last. */
static int R_FindSolidColumn(int first, int last, boolean solid)
{
    while (first < last)
    {
        unsigned word_index = (unsigned)first >> 5;
        unsigned bit = (unsigned)first & 31u;
        uint32_t candidates = solid ? solidcol[word_index] : ~solidcol[word_index];
        candidates &= ~0u << bit;

        if (word_index == ((unsigned)(last - 1) >> 5))
        {
            unsigned endbit = (unsigned)last & 31u;
            if (endbit)
                candidates &= (1u << endbit) - 1u;
        }

        if (candidates)
        {
            int found = (int)(word_index << 5) + (int)R_LowestSetBit(candidates);
            return found < last ? found : last;
        }
        first = (int)((word_index + 1u) << 5);
    }
    return last;
}

static void R_SetSolidColumns(int first, int last)
{
    while (first < last)
    {
        unsigned word_index = (unsigned)first >> 5;
        unsigned bit = (unsigned)first & 31u;
        int word_end = (int)((word_index + 1u) << 5);
        int end = last < word_end ? last : word_end;
        unsigned count = (unsigned)(end - first);
        uint32_t mask = count == 32u ? ~0u : ((1u << count) - 1u) << bit;
        solidcol[word_index] |= mask;
        first = end;
    }
}

static inline void R_SetSolidColumn(int column)
{
    solidcol[(unsigned)column >> 5] |= 1u << (column & 31);
}

static inline boolean R_AllColumnsSolid(void)
{
    for (unsigned i = 0; i < SOLIDCOL_WORDS; i++)
        if (solidcol[i] != ~0u)
            return false;
    return true;
}
#endif

static byte spanstart[MAX_SCREENHEIGHT];                // killough 2/8/98


static const seg_t     *curline;
static side_t    *sidedef;
static const line_t    *linedef;
static sector_t  *frontsector;
static sector_t  *backsector;
static drawseg_t *ds_p;

#ifdef DEV_RCHECK
unsigned g_ds_drop, g_open_drop, g_vis_drop;   /* per-run drop counters             */
unsigned g_peak_ds, g_peak_open, g_peak_vis;   /* peak usage across the run          */
#endif

static visplane_t *floorplane, *ceilingplane;
static int             rw_angle1;

static angle_t         rw_normalangle; // angle to line origin
static fixed_t         rw_distance;

static int      rw_stopx;

static fixed_t  rw_scale;
static fixed_t  rw_scalestep;

static int      worldtop;
static int      worldbottom;

static int didsolidcol; /* True if at least one column was marked solid */

// True if any of the segs textures might be visible.
static boolean  segtextured;
static boolean  markfloor;      // False if the back side is the same plane.
static boolean  markceiling;
static boolean  maskedtexture;
static int      toptexture;
static int      bottomtexture;
static int      midtexture;

static fixed_t  rw_midtexturemid;
static fixed_t  rw_toptexturemid;
static fixed_t  rw_bottomtexturemid;

const lighttable_t *fullcolormap;
const lighttable_t *colormaps;

const lighttable_t* fixedcolormap;

#ifdef SERIAL_LOG
/* P0 render instrumentation.  Timers are sampled once per eight events so the
 * profiler no longer performs multiple expensive ITU reads for every wall column.
 * Pixel/event counters remain exact.  Dense-wall tuple sets use generation
 * tags, avoiding a multi-KiB clear in every frame. */
#define RPROF_SAMPLE_MASK 7u
#define RPROF_PAGE_SET_SIZE 256u
#define RPROF_COLUMN_SET_SIZE 1024u

static uint32_t rprof_generation;
static uint32_t rprof_wall_sample_seq;
static uint32_t rprof_bbox_sample_seq;
static uint32_t rprof_addline_sample_seq;
static uint32_t rprof_clip_sample_seq;
static uint32_t rprof_store_sample_seq;
static uint32_t rprof_plane_sample_seq;
static uint32_t *rprof_page_keys;
static uint32_t *rprof_page_generation;
static uint32_t *rprof_column_keys;
static uint32_t *rprof_column_generation;

static void R_ProfileEnsureStorage(void)
{
    if (!rprof_page_keys)
    {
        /* Static BSS is already pressed against the 2 MiB RAM/stack ceiling.
         * Keep profiling-only working storage in the zone instead. */
        const unsigned page_words = RPROF_PAGE_SET_SIZE * 2;
        const unsigned column_words = RPROF_COLUMN_SET_SIZE * 2;
        uint32_t *storage = Z_Calloc(page_words + column_words,
                                    sizeof(*storage), PU_STATIC, NULL);
        rprof_page_keys = storage;
        rprof_page_generation = storage + RPROF_PAGE_SET_SIZE;
        rprof_column_keys = storage + page_words;
        rprof_column_generation = rprof_column_keys + RPROF_COLUMN_SET_SIZE;
    }
}

static void R_ProfileBeginFrame(void)
{
    R_ProfileEnsureStorage();
    rprof_generation++;
    /* A wrap is practically unreachable, but generation zero denotes an empty
     * slot after BSS initialization and must never become the active epoch. */
    if (!rprof_generation)
        rprof_generation = 1;
    rprof_wall_sample_seq = rprof_bbox_sample_seq = 0;
    rprof_addline_sample_seq = rprof_clip_sample_seq = 0;
    rprof_store_sample_seq = 0;
    rprof_plane_sample_seq = 0;
}

static int R_ProfileSample(uint32_t *sequence)
{
    return (((*sequence)++ & RPROF_SAMPLE_MASK) == 0);
}

static int R_ProfileTupleInsert(uint32_t key, uint32_t *keys,
                                uint32_t *generations, unsigned size)
{
    unsigned slot = (key * 2654435761u) & (size - 1);
    for (unsigned probes = 0; probes < size; probes++)
    {
        if (generations[slot] != rprof_generation)
        {
            generations[slot] = rprof_generation;
            keys[slot] = key;
            return 1;
        }
        if (keys[slot] == key)
            return 0;
        slot = (slot + 1) & (size - 1);
    }
    return -1;
}

static void R_ProfileWallTuple(int lump, unsigned column,
                               const lighttable_t *cmap)
{
    /* Every renderer colormap points at a 256-entry slice of fullcolormap.
     * Six bits cover Doom's 32 normal maps while leaving room for variants. */
    unsigned cmap_id = (unsigned)(cmap - fullcolormap) >> 8;
    uint32_t page_key = ((uint32_t)(lump + 1) << 6) | (cmap_id & 63u);
    uint32_t column_key = (page_key << 5) | (column & 31u);
    int inserted = R_ProfileTupleInsert(page_key, rprof_page_keys,
        rprof_page_generation, RPROF_PAGE_SET_SIZE);
    if (inserted > 0)
        g_rp_wall_page_cmaps++;
    else if (inserted < 0)
        g_rp_wall_page_cmap_overflow++;

    inserted = R_ProfileTupleInsert(column_key, rprof_column_keys,
        rprof_column_generation, RPROF_COLUMN_SET_SIZE);
    if (inserted > 0)
        g_rp_wall_column_cmaps++;
    else if (inserted < 0)
        g_rp_wall_column_cmap_overflow++;
}
#endif

int extralight;                           // bumped light from gun blasts
draw_vars_t drawvars;

static short   *mfloorclip;   // dropoff overflow
static short   *mceilingclip; // dropoff overflow
static fixed_t spryscale;
static fixed_t sprtopscreen;

static angle_t  rw_centerangle;
static fixed_t  rw_offset;
static int      rw_lightlevel;

static short      *maskedtexturecol; // dropoff overflow

const texture_t **textures; // proff - 04/05/2000 removed static for OpenGL
fixed_t   *textureheight; //needed for texture pegging (and TFE fix - killough)

short       *flattranslation;             // for global animation
short       *texturetranslation;
#ifdef PCFX_TEXTURE_PAGES
int         *pcfx_flatlumps;              // optional PFTnnnnn 32x64 pages

// Dense PC-FX pages are copied into small, explicitly 2 KiB-aligned hot caches.
// Each 32x64x8 page then occupies exactly one modeled DRAM page regardless of
// the zone allocator's block-header alignment. Opaque wall pages are guaranteed
// by the baker; masked two-sided middles use the separate post renderer.
#define PCFX_PAGE_BYTES 2048
#define PCFX_WALL_PAGE_SLOTS 8
#define PCFX_FLAT_PAGE_SLOTS 4
static byte pcfx_wall_pages[PCFX_WALL_PAGE_SLOTS][PCFX_PAGE_BYTES]
    __attribute__((aligned(2048)));
static byte pcfx_flat_pages[PCFX_FLAT_PAGE_SLOTS][PCFX_PAGE_BYTES]
    __attribute__((aligned(2048)));
static int pcfx_wall_page_lump[PCFX_WALL_PAGE_SLOTS];
static int pcfx_flat_page_lump[PCFX_FLAT_PAGE_SLOTS];
static unsigned pcfx_wall_page_next;
static unsigned pcfx_flat_page_next;

static const byte *R_PCFCachedPage(int lump, boolean flat)
{
    int *ids = flat ? pcfx_flat_page_lump : pcfx_wall_page_lump;
    const unsigned slots = flat ? PCFX_FLAT_PAGE_SLOTS : PCFX_WALL_PAGE_SLOTS;
    byte *pages = flat ? &pcfx_flat_pages[0][0] : &pcfx_wall_pages[0][0];
    unsigned *next = flat ? &pcfx_flat_page_next : &pcfx_wall_page_next;

    for (unsigned i = 0; i < slots; i++)
        if (ids[i] == lump)
            return pages + i * PCFX_PAGE_BYTES;

    unsigned slot = *next;
    *next = (slot + 1) % slots;
    const byte *source = (const byte *)W_CacheLumpNum(lump);
    byte *dest = pages + slot * PCFX_PAGE_BYTES;
    memcpy(dest, source, PCFX_PAGE_BYTES);
    ids[slot] = lump;
    return dest;
}
#endif

fixed_t basexscale, baseyscale;

fixed_t  viewcos, viewsin;

static fixed_t  topfrac;
static fixed_t  topstep;
static fixed_t  bottomfrac;
static fixed_t  bottomstep;

static fixed_t  pixhigh;
static fixed_t  pixlow;

static fixed_t  pixhighstep;
static fixed_t  pixlowstep;

static int      worldhigh;
static int      worldlow;


static fixed_t planeheight;

size_t num_vissprite;

boolean highDetail = false;



//*****************************************
// Constants
//*****************************************

const int viewheight = SCREENHEIGHT-ST_SCALED_HEIGHT;
const int centery = (SCREENHEIGHT-ST_SCALED_HEIGHT)/2;
static const int centerxfrac = (SCREENWIDTH/2) << FRACBITS;
static const int centeryfrac = ((SCREENHEIGHT-ST_SCALED_HEIGHT)/2) << FRACBITS;

const fixed_t projection = (SCREENWIDTH/2) << FRACBITS;

static const fixed_t projectiony = ((SCREENHEIGHT * (SCREENWIDTH/2) * 320) / 200) / SCREENWIDTH * FRACUNIT;

static const fixed_t pspritescale = FRACUNIT*SCREENWIDTH/320;
static const fixed_t pspriteiscale = FRACUNIT*320/SCREENWIDTH;

static const fixed_t pspriteyscale = (SCREENHEIGHT << FRACBITS) / 200;
static const fixed_t pspriteyiscale = ((UINT_MAX) / ((SCREENHEIGHT << FRACBITS) / 200));


static angle_t clipangle; // = xtoviewangle[0], set by R_SetupViewScaling

#define FIELDOFVIEW 2048   // 90 degrees FOV in fineangle units (matches r_main.c)

/* Compute the projection lookup tables for the active SCREENWIDTH/HEIGHT. This
 * replaces the 120x160 baked consts GBADoom copied in R_InitBuffer, so the render
 * is correct at the PC-FX's 256x240 (128 fat-pixel columns, 208-line 3D view).
 * The formulas are stock Doom R_InitTextureMapping + view-scaling; verified to
 * reproduce the baked tables at 120x160. Called once from R_InitBuffer. */
void R_SetupViewScaling(void)
{
    const int viewwidth = SCREENWIDTH;
    const fixed_t centerxfrac_l = (SCREENWIDTH / 2) << FRACBITS;
    int i, x, t;

    fixed_t focallength =
        FixedDiv(centerxfrac_l, finetangent[FINEANGLES / 4 + FIELDOFVIEW / 2]);

    for (i = 0; i < FINEANGLES / 2; i++)
    {
        if (finetangent[i] > FRACUNIT * 2)        t = -1;
        else if (finetangent[i] < -FRACUNIT * 2)  t = viewwidth + 1;
        else
        {
            t = FixedMul(finetangent[i], focallength);
            t = (centerxfrac_l - t + FRACUNIT - 1) >> FRACBITS;
            if (t < -1) t = -1; else if (t > viewwidth + 1) t = viewwidth + 1;
        }
        viewangletox[i] = t;
    }

    for (x = 0; x <= viewwidth; x++)
    {
        i = 0;
        while (viewangletox[i] > x) i++;
        xtoviewangle[x] = (i << ANGLETOFINESHIFT) - ANG90;
    }

    for (i = 0; i < FINEANGLES / 2; i++)
    {
        if (viewangletox[i] == -1)            viewangletox[i] = 0;
        else if (viewangletox[i] == viewwidth + 1) viewangletox[i] = viewwidth;
    }

    clipangle = xtoviewangle[0];

    for (i = 0; i < viewheight; i++)
    {
        fixed_t dy = ((i - centery) << FRACBITS) + FRACUNIT / 2;
        if (dy < 0) dy = -dy;
        yslope[i] = FixedDiv(projectiony, dy);
    }

    for (i = 0; i < viewwidth; i++)
    {
        fixed_t cosadj = finecosine[xtoviewangle[i] >> ANGLETOFINESHIFT];
        if (cosadj < 0) cosadj = -cosadj;
        distscale[i] = FixedDiv(FRACUNIT, cosadj);
    }
}

static const int skytexturemid = 100*FRACUNIT;
static const fixed_t skyiscale = (FRACUNIT*200)/((SCREENHEIGHT-ST_HEIGHT)+16);


//********************************************
// On the GBA we exploit that an 8 bit write
// will mirror to the upper 8 bits too.
// it saves an OR and Shift per pixel.
//********************************************
#ifdef GBA
    typedef byte pixel;
#else
    typedef unsigned short pixel;
#endif

// FixedMul now lives as a fast SH-1 static inline in m_fixed.h.

// killough 5/3/98: reformatted

static CONSTFUNC int SlopeDiv(unsigned num, unsigned den)
{
    den = den >> 8;

    if (den == 0)
        return SLOPERANGE;

    const unsigned int ans = FixedApproxDiv(num << 3, den) >> FRACBITS;

    return (ans <= SLOPERANGE) ? ans : SLOPERANGE;
}

//
// R_PointOnSide
// Traverse BSP (sub) tree,
//  check point against partition plane.
// Returns side 0 (front) or 1 (back).
//
// killough 5/2/98: reformatted
//

static PUREFUNC int R_PointOnSide(fixed_t x, fixed_t y, const mapnode_t *node)
{
    // Nodes are stored on-disk (LE 16-bit map units); swap before use.
    const int ndx = SHORT(node->dx);
    const int ndy = SHORT(node->dy);

    fixed_t dx = (fixed_t)ndx << FRACBITS;
    fixed_t dy = (fixed_t)ndy << FRACBITS;

    fixed_t nx = (fixed_t)SHORT(node->x) << FRACBITS;
    fixed_t ny = (fixed_t)SHORT(node->y) << FRACBITS;

    if (!dx)
        return x <= nx ? ndy > 0 : ndy < 0;

    if (!dy)
        return y <= ny ? ndx < 0 : ndx > 0;

    x -= nx;
    y -= ny;

    // Try to quickly decide by looking at sign bits.
    if ((dy ^ dx ^ x ^ y) < 0)
        return (dy ^ x) < 0;  // (left is negative)

    return FixedMul(y, ndx) >= FixedMul(ndy, x);
}

//
// R_PointInSubsector
//
// killough 5/2/98: reformatted, cleaned up

subsector_t *R_PointInSubsector(fixed_t x, fixed_t y)
{
    int nodenum = numnodes-1;

    // special case for trivial maps (single subsector, no nodes)
    if (numnodes == 0)
        return _g->subsectors;

    while (!(nodenum & NF_SUBSECTOR))
        nodenum = (unsigned short)SHORT(nodes[nodenum].children[R_PointOnSide(x, y, nodes+nodenum)]);
    return &_g->subsectors[nodenum & ~NF_SUBSECTOR];
}

//
// R_PointToAngle
// To get a global angle from cartesian coordinates,
//  the coordinates are flipped until they are in
//  the first octant of the coordinate system, then
//  the y (<=x) is scaled and divided by x to get a
//  tangent (slope) value which is looked up in the
//  tantoangle[] table.
//


CONSTFUNC angle_t R_PointToAngle2(fixed_t vx, fixed_t vy, fixed_t x, fixed_t y)
{
    x -= vx;
    y -= vy;

    if ( (!x) && (!y) )
        return 0;

    if (x>= 0)
    {
        // x >=0
        if (y>= 0)
        {
            // y>= 0

            if (x>y)
            {
                // octant 0
                return tantoangle[ SlopeDiv(y,x)];
            }
            else
            {
                // octant 1
                return ANG90-1-tantoangle[ SlopeDiv(x,y)];
            }
        }
        else
        {
            // y<0
            y = -y;

            if (x>y)
            {
                // octant 8
                return -tantoangle[SlopeDiv(y,x)];
            }
            else
            {
                // octant 7
                return ANG270+tantoangle[ SlopeDiv(x,y)];
            }
        }
    }
    else
    {
        // x<0
        x = -x;

        if (y>= 0)
        {
            // y>= 0
            if (x>y)
            {
                // octant 3
                return ANG180-1-tantoangle[ SlopeDiv(y,x)];
            }
            else
            {
                // octant 2
                return ANG90+ tantoangle[ SlopeDiv(x,y)];
            }
        }
        else
        {
            // y<0
            y = -y;

            if (x>y)
            {
                // octant 4
                return ANG180+tantoangle[ SlopeDiv(y,x)];
            }
            else
            {
                // octant 5
                return ANG270-1-tantoangle[ SlopeDiv(x,y)];
            }
        }
    }
}

static PUREFUNC angle_t R_PointToAngle(fixed_t x, fixed_t y)
{
    return R_PointToAngle2(viewx, viewy, x, y);
}


// killough 5/2/98: move from r_main.c, made static, simplified

static PUREFUNC fixed_t R_PointToDist(fixed_t x, fixed_t y)
{
    fixed_t dx = D_abs(x - viewx);
    fixed_t dy = D_abs(y - viewy);

    if (dy > dx)
    {
        fixed_t t = dx;
        dx = dy;
        dy = t;
    }

    return FixedApproxDiv(dx, finesine[(tantoangle[FixedApproxDiv(dy,dx) >> DBITS] + ANG90) >> ANGLETOFINESHIFT]);
}

/* Floor on the diminished-lighting colormap level: no surface is ever rendered at
 * colormap 0 (pure, un-shaded full brightness). Even the sector nearest the eye
 * carries a touch of shade, so bright textures/sectors stop reading as flat
 * blown-out slabs — the "lit pixels that shouldn't be". Kept small so near
 * surfaces are still clearly the brightest in the scene. */
#define LIGHTMIN 4

static const lighttable_t* R_ColourMap(int lightlevel)
{
    if (fixedcolormap)
        return fixedcolormap;
    else
    {
        if (curline)
        {
            if (curline->v1.y == curline->v2.y)
                lightlevel -= 1 << LIGHTSEGSHIFT;
            else if (curline->v1.x == curline->v2.x)
                lightlevel += 1 << LIGHTSEGSHIFT;
        }

        lightlevel += extralight << LIGHTSEGSHIFT;

        int cm = ((256-lightlevel)>>2) - 24;

        if(cm >= NUMCOLORMAPS)
            cm = NUMCOLORMAPS-1;
        else if(cm < LIGHTMIN)          // never full-bright (matches the world tables)
            cm = LIGHTMIN;

        return fullcolormap + cm*256;
    }
}


//Load a colormap into IWRAM.
static const lighttable_t* R_LoadColorMap(int lightlevel)
{
    /* PC-FX has no fast IWRAM, so the GBA-era copy of the 256-entry map into a local
     * buffer was pure per-sprite overhead — just return the resident map pointer. */
    return R_ColourMap(lightlevel);
}

/* --------------------------------------------------------------- lighting ---
 * PC Doom-style diminished lighting. GBADoom shaded each surface by its sector
 * light only (R_ColourMap), so rooms looked uniformly "lit". These tables add
 * distance darkening: scalelight[light][scale] for walls/sprites (indexed by the
 * projected scale) and zlight[light][z] for floors/ceilings (indexed by depth).
 * `light` is the sector light bucket (0..LIGHTLEVELS-1). Built once from the
 * COLORMAP; brighter surfaces near the viewer, darker toward the horizon. */
#define DISTMAP 2

/* Vanilla Doom calibrates its diminished-lighting fall-off to a 320-wide view
 * (projection = 320/2 = 160) and normalises the wall table by 320/viewwidth. This
 * port renders through a fat-pixel framebuffer whose horizontal projection is only
 * SCREENWIDTH/2 = 64 word-units, and the light tables were indexed by that raw
 * word-space scale WITHOUT the 320/viewwidth normalisation. The net effect is that
 * floors/walls darkened with distance ~2.5x (=160/64) faster than vanilla: mid and
 * far surfaces fell to black much too soon, which — once index-0 stopped leaking
 * the bright sky through them — made non-diminishing full-bright (light 255) sectors
 * like lit stairs and door tracks pop harshly against near-black surroundings.
 *
 * Calibrate the fall-off to vanilla's world-distance rate by expressing both tables
 * in the 320-wide reference projection (LIGHT_PROJ) instead of the 64-word one. */
#define LIGHT_PROJ 160          /* vanilla 320/2 reference projection            */
#define LIGHT_XSCALE_NUM  (2 * LIGHT_PROJ)   /* rw_scale (word space) -> reference */
#define LIGHT_XSCALE_DEN  (SCREENWIDTH)      /* = 160/64 = 2.5x, as a ratio        */

/* Distance-darkening floor applied to EVERY light level, including the brightest.
 * Stock Doom leaves the brightest sectors (startmap 0) at full brightness at all
 * depths, so they read as flat, un-shaded slabs — worst on the PC-FX where index-0
 * no longer leaks the sky through the dark surroundings, so a full-bright stair or
 * door track pops out of an otherwise depth-shaded room. Seeding startmap with
 * LIGHTBRIGHT lets even a light-255 sector fade toward this level with distance
 * (still full-bright up close, since the near scale term swamps it), so nothing is
 * a flat full-bright surface and the whole scene carries depth like doomgeneric. */
#define LIGHTBRIGHT 8

static const lighttable_t *scalelight[LIGHTLEVELS][MAXLIGHTSCALE];
static const lighttable_t *zlight[LIGHTLEVELS][MAXLIGHTZ];
static const lighttable_t *const *walllights;   // scalelight[l] for the current seg
static const lighttable_t *const *planezlight;  // zlight[l] for the current plane
static int lighttables_built;

static void R_InitLightTables(void)
{
    for (int i = 0; i < LIGHTLEVELS; i++)
    {
        const int startmap = LIGHTBRIGHT
                           + ((LIGHTLEVELS - 1 - i) * 2) * NUMCOLORMAPS / LIGHTLEVELS;

        for (int j = 0; j < MAXLIGHTSCALE; j++)
        {
            /* j is the seg's word-space scale bucket; scale it to the 320-wide
             * reference before the vanilla startmap - scale/DISTMAP fall-off. */
            int level = startmap - (j * LIGHT_XSCALE_NUM / LIGHT_XSCALE_DEN) / DISTMAP;
            if (level < LIGHTMIN) level = LIGHTMIN;
            else if (level >= NUMCOLORMAPS) level = NUMCOLORMAPS - 1;
            scalelight[i][j] = colormaps + level * 256;
        }

        for (int j = 0; j < MAXLIGHTZ; j++)
        {
            const int scale = FixedDiv(LIGHT_PROJ * FRACUNIT, (j + 1) << LIGHTZSHIFT);
            int level = startmap - (scale >> LIGHTSCALESHIFT) / DISTMAP;
            if (level < LIGHTMIN) level = LIGHTMIN;
            else if (level >= NUMCOLORMAPS) level = NUMCOLORMAPS - 1;
            zlight[i][j] = colormaps + level * 256;
        }
    }
    lighttables_built = 1;
}

/* Sector light 0..255 -> light bucket 0..LIGHTLEVELS-1, plus gun-flash extralight.
 * Exactly doomgeneric's `(lightlevel>>LIGHTSEGSHIFT)+extralight`: gamma is NOT mixed
 * into the light here (that flattened bright sectors toward full-bright and washed
 * out depth) — the GAMMA BOOST is applied to the display palette instead. */
static int R_LightNum(int lightlevel)
{
    int l = (lightlevel >> LIGHTSEGSHIFT) + extralight;
    if (l < 0) l = 0;
    else if (l >= LIGHTLEVELS) l = LIGHTLEVELS - 1;
    return l;
}

//
// A column is a vertical slice/span from a wall texture that,
//  given the DOOM style restrictions on the view orientation,
//  will always have constant z depth.
// Thus a special case loop for very fast rendering can
//  be used. It has also been used with Wolfenstein 3D.
//

#pragma GCC push_options
#pragma GCC optimize ("Ofast")

#define COLEXTRABITS 9
#define COLBITS (FRACBITS + COLEXTRABITS)

inline static void R_DrawColumnPixel(unsigned short* dest, const byte* source, const lighttable_t* colormap, unsigned int frac)
{
    /* PC-FX: write straight into the KRAM column cursor (set by FB_col before the
     * loop). `dest` is unused — the KRAM write pointer auto-increments by one row. */
    (void)dest;
    /* colormap[] entries are pre-doubled fat pixels (see lighttable_t) -> one ld.h,
     * no per-pixel andi/shl/or. */
    FB_put(colormap[source[frac>>COLBITS]]);
}

static void R_DrawColumn (const draw_column_vars_t *dcvars)
{
    rprof_sample();
    int count = (dcvars->yh - dcvars->yl) + 1;

    // Zero length, column does not exceed a pixel.
    if (count <= 0)
        return;

    const byte *source = dcvars->source;
    const lighttable_t *colormap = dcvars->colormap;

    /* Aim the KRAM write cursor at this column (auto-increments one row per pixel).
     * `dest` is kept only so the unrolled loop below compiles unchanged; the
     * actual store target is the KRAM cursor inside R_DrawColumnPixel. */
    unsigned short* dest = drawvars.byte_topleft;
    FB_col((unsigned)dcvars->x, (unsigned)dcvars->yl);

    const unsigned int		fracstep = (dcvars->iscale << COLEXTRABITS);
    unsigned int frac = (dcvars->texturemid + (dcvars->yl - centery)*dcvars->iscale) << COLEXTRABITS;

    // Inner loop that does the actual texture mapping,
    //  e.g. a DDA-lile scaling.
    // This is as fast as it gets.

    /* 8x unroll (was 16x): the V810 icache is only 1 KB, and the 16x body pushed
     * this drawer + its per-column callers past it -> icache thrash. 8x fits it
     * under 1 KB (measured ~+13%% render on E1M6; pcfxemu models the icache). */
    unsigned int l = (count >> 3);

    while(l--)
    {
        R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;

        R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
    }

    unsigned int r = (count & 7);

    switch(r)
    {
        case 7:     R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        case 6:     R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        case 5:     R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        case 4:     R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        case 3:     R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        case 2:     R_DrawColumnPixel(dest, source, colormap, frac); dest+=SCREENWIDTH; frac+=fracstep;
        case 1:     R_DrawColumnPixel(dest, source, colormap, frac);
    }
}

// Dense wall page drawer. Texture-space coordinates retain the original Doom
// logical dimensions; the precomputed y scale maps them into the 64-row page.
// The mask also makes vertical wrapping explicit and prevents a short page from
// ever being sampled out of bounds.
#ifdef PCFX_TEXTURE_PAGES
typedef uint16_t litwall_pixel_t;
extern void pcfx_column_lit64(unsigned int count, unsigned int frac,
                              unsigned int fracstep,
                              const litwall_pixel_t *lit);
extern void pcfx_bake_litwall64(const byte *source,
                                const lighttable_t *colormap,
                                litwall_pixel_t *dest);

static void R_DrawColumnPCFX(const draw_column_vars_t *dcvars,
                             fixed_t yscale, fixed_t texturemid)
{
    rprof_sample();
    int count = (dcvars->yh - dcvars->yl) + 1;
    if (count <= 0)
        return;

    const byte *source = dcvars->source;
    const lighttable_t *colormap = dcvars->colormap;
    fixed_t iscale = FixedMul(dcvars->iscale, yscale);
    unsigned int fracstep = (unsigned)iscale << COLEXTRABITS;
    unsigned int frac = (unsigned)(texturemid +
        (dcvars->yl - centery) * iscale) << COLEXTRABITS;

    FB_col((unsigned)dcvars->x, (unsigned)dcvars->yl);
    /* Keep this loop compact. An 8x version remained below 1 KB in isolation but
     * displaced its caller in the V810's direct-mapped I-cache and regressed the
     * measured wall phase; the small loop wins despite its extra branch. */
    while (count--)
    {
        FB_put(colormap[source[(frac >> COLBITS) & 63]]);
        frac += fracstep;
    }
}

/* Dense pre-lit wall column. Each (page,column,colormap) entry contains final
 * doubled 16-bit palette words, removing both the dependent colormap load and
 * byte duplication from the direct-KRAM pixel loop. */
static void R_DrawColumnPCFXLit(const draw_column_vars_t *dcvars,
                                fixed_t yscale, fixed_t texturemid,
                                const litwall_pixel_t *lit)
{
    rprof_sample();
    int count = (dcvars->yh - dcvars->yl) + 1;
    if (count <= 0)
        return;

    fixed_t iscale = FixedMul(dcvars->iscale, yscale);
    unsigned int fracstep = (unsigned)iscale << COLEXTRABITS;
    unsigned int frac = (unsigned)(texturemid +
        (dcvars->yl - centery) * iscale) << COLEXTRABITS;

    FB_col((unsigned)dcvars->x, (unsigned)dcvars->yl);
    pcfx_column_lit64((unsigned)count, frac, fracstep, lit);
}
#endif

#define FUZZOFF (SCREENWIDTH)
#define FUZZTABLE 50

static const int fuzzoffset[FUZZTABLE] =
{
    FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF
};

//
// Framebuffer postprocessing.
// Creates a fuzzy image by copying pixels
//  from adjacent ones to left and right.
// Used with an all black colormap, this
//  could create the SHADOW effect,
//  i.e. spectres and invisible players.
//
static void R_DrawFuzzColumn (const draw_column_vars_t *dcvars)
{
    rprof_sample();
    int dc_yl = dcvars->yl;
    int dc_yh = dcvars->yh;

    // Adjust borders. Low...
    if (dc_yl <= 0)
        dc_yl = 1;

    // .. and high.
    if (dc_yh >= viewheight-1)
        dc_yh = viewheight - 2;

    int count = (dc_yh - dc_yl) + 1;

    // Zero length, column does not exceed a pixel.
    if (count <= 0)
        return;

    /* The real fuzz reads adjacent framebuffer pixels; that would be a KRAM
     * read-modify-write per pixel. Approximate the spectre shadow with the dark
     * colormap's index 0, drawn straight to the KRAM column cursor. */
    /* fullcolormap entries are pre-doubled fat pixels; take the dark map's index 0. */
    unsigned short fuzz = fullcolormap[6*256];

    FB_col((unsigned)dcvars->x, (unsigned)dc_yl);

    do
    {
        FB_put(fuzz);
    } while(--count);
}

#pragma GCC pop_options

#ifdef PCFX_TEXTURE_PAGES
/* Stationary-background replay records the exact screen intervals overwritten
 * by sprites and masked walls.  The implementation lives with the background
 * cache below; keep this declaration here so the shared masked-post walker can
 * mark writes without adding work to opaque wall columns. */
static void R_FrameBgMarkDirty(unsigned x, int yl, int yh, boolean fuzz);
static boolean R_FrameBgMaskedRecordPost(const draw_column_vars_t *dcvars);
static void R_FrameBgMaskedRecordBegin(const drawseg_t *ds, unsigned x);
static void R_FrameBgMaskedRecordEnd(void);
static boolean R_FrameBgMaskedReplay(const drawseg_t *ds, unsigned x);
#endif



//
// R_DrawMaskedColumn
// Used for sprites and masked mid textures.
// Masked means: partly transparent, i.e. stored
//  in posts/runs of opaque pixels.
//
static void R_DrawMaskedColumn(R_DrawColumn_f colfunc, draw_column_vars_t *dcvars,
                               const column_t *column, boolean is_masked_wall
#ifdef SERIAL_LOG
                               , int profile_is_masked_wall
#endif
                               )
{
    const fixed_t basetexturemid = dcvars->texturemid;

    const int fclip_x = mfloorclip[dcvars->x];
    const int cclip_x = mceilingclip[dcvars->x];

    while (column->topdelta != 0xff)
    {
        // calculate unclipped screen coordinates for post
        const int topscreen = sprtopscreen + spryscale*column->topdelta;
        const int bottomscreen = topscreen + spryscale*column->length;

        int yh = (bottomscreen-1)>>FRACBITS;
        int yl = (topscreen+FRACUNIT-1)>>FRACBITS;

        if(yh >= fclip_x)
            yh = fclip_x - 1;

        if(yl <= cclip_x)
            yl = cclip_x + 1;

        // killough 3/2/98, 3/27/98: Failsafe against overflow/crash:
        if (yh < viewheight && yl <= yh)
        {
#ifdef SERIAL_LOG
            int profile_yl = yl;
            int profile_yh = yh;
            /* Match R_DrawFuzzColumn's one-pixel top/bottom safety clamp so the
             * counter describes actual KRAM writes, not merely visible posts. */
            if (colfunc == R_DrawFuzzColumn)
            {
                if (profile_yl <= 0) profile_yl = 1;
                if (profile_yh >= viewheight - 1) profile_yh = viewheight - 2;
            }
            if (profile_yl <= profile_yh)
            {
                unsigned pixels = (unsigned)(profile_yh - profile_yl + 1);
                if (profile_is_masked_wall)
                    g_rp_masked_wall_pixels += pixels;
                else
                    g_rp_sprite_pixels += pixels;
            }
#endif
            dcvars->source =  (const byte*)column + 3;

            dcvars->texturemid = basetexturemid - (column->topdelta<<FRACBITS);

            dcvars->yh = yh;
            dcvars->yl = yl;

            // Drawn by either R_DrawColumn
            //  or (SHADOW) R_DrawFuzzColumn.
#ifdef PCFX_TEXTURE_PAGES
#ifdef PCFX_FRAME_BG_MASK_PERSIST_FORCE_OFF
            R_FrameBgMarkDirty((unsigned)dcvars->x, yl, yh,
                               colfunc == R_DrawFuzzColumn);
#endif
            /* Masked walls are invariant under the background signature and
             * remain resident in both KRAM pages. Only live sprite/fuzz writes
             * are damage that the next use of a page must repair. */
            if (is_masked_wall)
            {
                if (!R_FrameBgMaskedRecordPost(dcvars))
                    colfunc (dcvars);
            }
            else
            {
#ifndef PCFX_FRAME_BG_MASK_PERSIST_FORCE_OFF
                R_FrameBgMarkDirty((unsigned)dcvars->x, yl, yh,
                                   colfunc == R_DrawFuzzColumn);
#endif
                colfunc (dcvars);
            }
#else
            colfunc (dcvars);
#endif
        }

        column = (const column_t *)((const byte *)column + column->length + 4);
    }

    dcvars->texturemid = basetexturemid;
}

//
// R_DrawVisSprite
//  mfloorclip and mceilingclip should also be set.
//
// CPhipps - new wad lump handling, *'s to const*'s
static void R_DrawVisSprite(const vissprite_t *vis)
{
    fixed_t  frac;

    R_DrawColumn_f colfunc = R_DrawColumn;
    draw_column_vars_t dcvars;

    R_SetDefaultDrawColumnVars(&dcvars);

    dcvars.colormap = vis->colormap;

    // killough 4/11/98: rearrange and handle translucent sprites
    // mixed with translucent/non-translucenct 2s normals

    if (!dcvars.colormap)   // NULL colormap = shadow draw
        colfunc = R_DrawFuzzColumn;    // killough 3/14/98

    // proff 11/06/98: Changed for high-res
    dcvars.iscale = vis->iscale;
    dcvars.texturemid = vis->texturemid;
    frac = vis->startfrac;

    spryscale = vis->scale;
    sprtopscreen = centeryfrac - FixedMul(dcvars.texturemid, spryscale);


    const patch_t *patch = vis->patch;

    fixed_t xiscale = vis->xiscale;

    dcvars.x = vis->x1;
    /* PC-FX has one 16-bit KING word (two identical 8bpp pixels) per logical X.
     * The inherited high-detail loop sampled two columns at the same X and the
     * second fat-word write simply overwrote the first. Draw exactly one source
     * column per 128-wide logical destination column. This is bit-identical to
     * the real PC-FX low-detail path and removes the fake mode's duplicate work. */
    while(dcvars.x < SCREENWIDTH)
    {
        const column_t* column = (const column_t *) ((const byte *)patch + LONG(patch->columnofs[frac >> FRACBITS]));
        R_DrawMaskedColumn(colfunc, &dcvars, column, false
#ifdef SERIAL_LOG
                           , 0
#endif
                           );

        frac += xiscale;
        dcvars.x++;

        if(((frac >> FRACBITS) >= SHORT(patch->width)) || frac < 0)
            break;
    }
}

static const column_t* R_GetColumn(const texture_t* texture, int texcolumn)
{
    const unsigned int patchcount = texture->patchcount;
    const unsigned int widthmask = texture->widthmask;

    const int xc = texcolumn & widthmask;

    if(patchcount == 1)
    {
        //simple texture.  Patch data is PU_CACHE: re-fetch (re-streams if evicted).
        const patch_t* patch = (const patch_t*)W_CacheLumpNum(texture->patches[0].lump);

        return (const column_t *) ((const byte *)patch + LONG(patch->columnofs[xc]));
    }
    else
    {
        unsigned int i = 0;

        do
        {
            const texpatch_t* patch = &texture->patches[i];

            const int x1 = patch->originx;

            if(xc < x1)
                continue;

            const patch_t* realpatch = (const patch_t*)W_CacheLumpNum(patch->lump);

            const int x2 = x1 + SHORT(realpatch->width);

            if(xc < x2)
                return (const column_t *)((const byte *)realpatch + LONG(realpatch->columnofs[xc-x1]));

        } while(++i < patchcount);
    }

    return NULL;
}


static const texture_t* R_GetOrLoadTexture(int tex_num)
{
    const texture_t* tex = textures[tex_num];

    if(!tex)
        tex = R_GetTexture(tex_num);

    return tex;
}


//
// R_RenderMaskedSegRange
//

static void R_RenderMaskedSegRange(const drawseg_t *ds, int x1, int x2)
{
    int      texnum;
    draw_column_vars_t dcvars;

    R_SetDefaultDrawColumnVars(&dcvars);

    // Calculate light table.
    // Use different light tables
    //   for horizontal / vertical / diagonal. Diagonal?

    curline = ds->curline;  // OPTIMIZE: get rid of LIGHTSEGSHIFT globally

    frontsector = SG_FRONTSECTOR(curline);
    backsector = SG_BACKSECTOR(curline);

    texnum = _g->sides[curline->sidenum].midtexture;
    texnum = texturetranslation[texnum];

    // killough 4/13/98: get correct lightlevel for 2s normal textures
    rw_lightlevel = frontsector->lightlevel;

    maskedtexturecol = ds->maskedtexturecol;

    rw_scalestep = ds->scalestep;
    spryscale = ds->scale1 + (x1 - ds->x1)*rw_scalestep;
    mfloorclip = ds->sprbottomclip;
    mceilingclip = ds->sprtopclip;

    // find positioning
    if (_g->lines[curline->linenum].flags & ML_DONTPEGBOTTOM)
    {
        dcvars.texturemid = frontsector->floorheight > backsector->floorheight
                ? frontsector->floorheight : backsector->floorheight;
        dcvars.texturemid = dcvars.texturemid + textureheight[texnum] - viewz;
    }
    else
    {
        dcvars.texturemid =frontsector->ceilingheight<backsector->ceilingheight
                ? frontsector->ceilingheight : backsector->ceilingheight;
        dcvars.texturemid = dcvars.texturemid - viewz;
    }

    dcvars.texturemid += (_g->sides[curline->sidenum].rowoffset << FRACBITS);

    const texture_t* texture = R_GetOrLoadTexture(texnum);

    const lighttable_t *const *maskedlights =
        fixedcolormap ? NULL : scalelight[R_LightNum(rw_lightlevel)];
    dcvars.colormap = fixedcolormap ? fixedcolormap : maskedlights[MAXLIGHTSCALE - 1];

    // draw the columns. Both callers pass x1 <= x2 (R_DrawSprite only reaches
    // here for an overlapping drawseg, so r1 <= r2; the other call passes
    // ds->x1,ds->x2), so bottom-test the loop. (cf. d32xr b89de43)
    dcvars.x = x1;
    do
    {
        const int xc = maskedtexturecol[dcvars.x];

        if (xc != SHRT_MAX) // dropoff overflow
        {
#ifdef PCFX_TEXTURE_PAGES
            if (R_FrameBgMaskedReplay(ds, (unsigned)dcvars.x))
            {
                maskedtexturecol[dcvars.x] = SHRT_MAX;
                spryscale += rw_scalestep;
                continue;
            }

            R_FrameBgMaskedRecordBegin(ds, (unsigned)dcvars.x);
#endif
            sprtopscreen = centeryfrac - FixedMul(dcvars.texturemid, spryscale);

            dcvars.iscale = FixedReciprocal((unsigned)spryscale);

            if (maskedlights)   // diminished lighting per column
            {
                int li = (int)(spryscale >> LIGHTSCALESHIFT);
                if (li >= MAXLIGHTSCALE) li = MAXLIGHTSCALE - 1;
                else if (li < 0) li = 0;
                dcvars.colormap = maskedlights[li];
            }

            // draw the texture
            const column_t* column = R_GetColumn(texture, xc);

            R_DrawMaskedColumn(R_DrawColumn, &dcvars, column, true
#ifdef SERIAL_LOG
                               , 1
#endif
                               );
#ifdef PCFX_TEXTURE_PAGES
            R_FrameBgMaskedRecordEnd();
#endif

            maskedtexturecol[dcvars.x] = SHRT_MAX; // dropoff overflow
        }

        spryscale += rw_scalestep;
    } while (++dcvars.x <= x2);

    curline = NULL; /* cph 2001/11/18 - must clear curline now we're done with it, so R_ColourMap doesn't try using it for other things */
}


// killough 5/2/98: reformatted

static PUREFUNC int R_PointOnSegSide(fixed_t x, fixed_t y, const seg_t *line)
{
    const fixed_t lx = line->v1.x;
    const fixed_t ly = line->v1.y;
    const fixed_t ldx = line->v2.x - lx;
    const fixed_t ldy = line->v2.y - ly;

    if (!ldx)
        return x <= lx ? ldy > 0 : ldy < 0;

    if (!ldy)
        return y <= ly ? ldx < 0 : ldx > 0;

    x -= lx;
    y -= ly;

    // Try to quickly decide by looking at sign bits.
    if ((ldy ^ ldx ^ x ^ y) < 0)
        return (ldy ^ x) < 0;          // (left is negative)

    return FixedMul(y, ldx>>FRACBITS) >= FixedMul(ldy>>FRACBITS, x);
}


//
// R_DrawSprite
//

static void R_DrawSprite (const vissprite_t* spr)
{
    // Sprite lumps are PU_CACHE (evictable): the patch cached at BSP projection
    // time may have been purged by later allocations, so re-cache it (a no-op if
    // still resident) before any use of spr->patch below (clip calc + draw).
    ((vissprite_t*)spr)->patch = (const patch_t*)W_CacheLumpNum(spr->lump);

    short* clipbot = floorclip;
    short* cliptop = ceilingclip;

    fixed_t scale;
    fixed_t lowscale;

    for (int x = spr->x1 ; x<=spr->x2 ; x++)
    {
        clipbot[x] = viewheight;
        cliptop[x] = -1;
    }


    // Scan drawsegs from end to start for obscuring segs.
    // The first drawseg that has a greater scale is the clip seg.

    // Modified by Lee Killough:
    // (pointer check was originally nonportable
    // and buggy, by going past LEFT end of array):

    const drawseg_t* drawsegs  =_g->drawsegs;

    for (const drawseg_t* ds = ds_p; ds-- > drawsegs; )  // new -- killough
    {
        // determine if the drawseg obscures the sprite
        if (ds->x1 > spr->x2 || ds->x2 < spr->x1 || (!ds->silhouette && !ds->maskedtexturecol))
            continue;      // does not cover sprite

        const int r1 = ds->x1 < spr->x1 ? spr->x1 : ds->x1;
        const int r2 = ds->x2 > spr->x2 ? spr->x2 : ds->x2;

        if (ds->scale1 > ds->scale2)
        {
            lowscale = ds->scale2;
            scale = ds->scale1;
        }
        else
        {
            lowscale = ds->scale1;
            scale = ds->scale2;
        }

        if (scale < spr->scale || (lowscale < spr->scale && !R_PointOnSegSide (spr->gx, spr->gy, ds->curline)))
        {
            if (ds->maskedtexturecol)       // masked mid texture?
                R_RenderMaskedSegRange(ds, r1, r2);

            continue;               // seg is behind sprite
        }

        // clip this piece of the sprite
        // killough 3/27/98: optimized and made much shorter

        if (ds->silhouette & SIL_BOTTOM && spr->gz < ds->bsilheight) //bottom sil
        {
            for (int x = r1; x <= r2; x++)
            {
                if (clipbot[x] == viewheight)
                    clipbot[x] = ds->sprbottomclip[x];
            }

        }

        fixed_t gzt = spr->gz + (SHORT(spr->patch->topoffset) << FRACBITS);

        if (ds->silhouette & SIL_TOP && gzt > ds->tsilheight)   // top sil
        {
            for (int x=r1; x <= r2; x++)
            {
                if (cliptop[x] == -1)
                    cliptop[x] = ds->sprtopclip[x];
            }
        }
    }

    // all clipping has been performed, so draw the sprite
    mfloorclip = clipbot;
    mceilingclip = cliptop;
    R_DrawVisSprite (spr);
}


//
// R_DrawPSprite
//

static void R_DrawPSprite (pspdef_t *psp, int lightlevel)
{
    int           x1, x2;
    spritedef_t   *sprdef;
    spriteframe_t *sprframe;
    boolean       flip;
    vissprite_t   *vis;
    vissprite_t   avis;
    int           width;
    fixed_t       topoffset;

    // decide which patch to use
    sprdef = &_g->sprites[psp->state->sprite];

    sprframe = &sprdef->spriteframes[psp->state->frame & FF_FRAMEMASK];

    flip = (boolean) SPR_FLIPPED(sprframe, 0);

    const patch_t* patch = W_CacheLumpNum(sprframe->lump[0]+_g->firstspritelump);
    // calculate edges of the shape
    fixed_t       tx;
    tx = psp->sx-160*FRACUNIT;

    tx -= SHORT(patch->leftoffset)<<FRACBITS;
    x1 = (centerxfrac + FixedMul (tx, pspritescale))>>FRACBITS;

    tx += SHORT(patch->width)<<FRACBITS;
    x2 = ((centerxfrac + FixedMul (tx, pspritescale) ) >>FRACBITS) - 1;

    width = SHORT(patch->width);
    topoffset = SHORT(patch->topoffset)<<FRACBITS;



    // off the side
    if (x2 < 0 || x1 > SCREENWIDTH)
        return;

    // store information in a vissprite
    vis = &avis;
    vis->mobjflags = 0;
    // killough 12/98: fix psprite positioning problem
    vis->texturemid = (BASEYCENTER<<FRACBITS) /* +  FRACUNIT/2 */ -
            (psp->sy-topoffset);
    vis->x1 = x1 < 0 ? 0 : x1;
    vis->x2 = x2 >= SCREENWIDTH ? SCREENWIDTH-1 : x2;
    // proff 11/06/98: Added for high-res
    vis->scale = pspriteyscale;
    vis->iscale = pspriteyiscale;

    if (flip)
    {
        vis->xiscale = - pspriteiscale;
        vis->startfrac = (width<<FRACBITS)-1;
    }
    else
    {
        vis->xiscale = pspriteiscale;
        vis->startfrac = 0;
    }

    if (vis->x1 > x1)
        vis->startfrac += vis->xiscale*(vis->x1-x1);

    vis->patch = patch;

    if (_g->player.powers[pw_invisibility] > 4*32 || _g->player.powers[pw_invisibility] & 8)
        vis->colormap = NULL;                    // shadow draw
    else if (fixedcolormap)
        vis->colormap = fixedcolormap;           // fixed color
    else if (psp->state->frame & FF_FULLBRIGHT)
        vis->colormap = fullcolormap;            // full bright // killough 3/20/98
    else
        vis->colormap = R_LoadColorMap(lightlevel);  // local light

    R_DrawVisSprite(vis);
}



//
// R_DrawPlayerSprites
//

static void R_DrawPlayerSprites(void)
{

  int i, lightlevel = _g->player.mo->subsector->sector->lightlevel;
  pspdef_t *psp;

  // clip to screen bounds (software fallback path still needs these)
  mfloorclip = screenheightarray;
  mceilingclip = negonearray;

  // The player weapon is composited by the two HuC6270 VDCs as 256-colour
  // hardware sprites (platform/pcfx_weapon.c) instead of being drawn into the
  // KING framebuffer.  For each active psprite we replicate R_DrawPSprite's
  // screen positioning, then place the generated VDC frame; weapons without VDC
  // art (e.g. SSG/rocket/plasma/BFG) fall back to the software drawer.
  pcfx_weapon_begin();
  for (i=0, psp=_g->player.psprites; i<NUMPSPRITES; i++,psp++)
  {
    if (!psp->state)
      continue;

    int fidx = pcfx_weapon_lookup(psp->state->sprite,
                                  psp->state->frame & FF_FRAMEMASK);
    if (fidx < 0)
    {
      R_DrawPSprite(psp, lightlevel);   // software fallback
      continue;
    }

    // Position with the GENERATED frame's OWN offsets (from doom1.wad, the WAD
    // the sprite ART was converted from) — NOT the runtime baked WAD's offsets.
    // The baked (squashware) WAD re-trims several psprites and shifts their
    // offsets to compensate for its own trimmed dimensions (e.g. PISGB0 is
    // 79x82 @(-104,-86) in doom1 but 61x72 @(-122,-96) baked; PISFA0 topoffset
    // is -66 vs -76).  The hardware sprite art is doom1's, so placing it with
    // the baked offsets shifts the gun (worst on the firing frame) and the
    // muzzle flash no longer lines up with the barrel.  Art and offset must come
    // from the SAME WAD, so use the generated frame's doom1 lx/ty here.
    const pcfx_wep_frame_t *wf = &pcfx_weapon_frames[fidx];
    int lo = wf->lx;
    int to = wf->ty;

    // Horizontal: same as R_DrawPSprite; x1 is a fat-pixel column (SCREENWIDTH
    // = 128 words), *2 -> physical 256px screen coordinate for the VDC.
    fixed_t tx = psp->sx - 160*FRACUNIT - (lo << FRACBITS);
    int x1 = (centerxfrac + FixedMul(tx, pspritescale)) >> FRACBITS;
    // Vertical (full 240-line res): mirror R_DrawVisSprite's sprtopscreen.
    fixed_t texturemid = (BASEYCENTER << FRACBITS) - (psp->sy - (to << FRACBITS));
    int y_top = (centeryfrac - FixedMul(texturemid, pspriteyscale)) >> FRACBITS;

    pcfx_weapon_add(i, fidx, x1 * 2, y_top);
  }
  pcfx_weapon_end();
}


//
// R_SortVisSprites
//
// Rewritten by Lee Killough to avoid using unnecessary
// linked lists, and to use faster sorting algorithm.
//
static int compare (const void* l, const void* r)
{
    const vissprite_t* vl = *(const vissprite_t**)l;
    const vissprite_t* vr = *(const vissprite_t**)r;

    return vr->scale - vl->scale;
}

static void R_SortVisSprites (void)
{
    int i = num_vissprite;

    if (i)
    {
        while (--i>=0)
            vissprite_ptrs[i] = _g->vissprites+i;

        qsort(vissprite_ptrs, num_vissprite, sizeof (vissprite_t*), compare);
    }
}

//
// R_DrawMasked
//

static void R_DrawMasked(void)
{
    int i;
    drawseg_t *ds;
    drawseg_t* drawsegs = _g->drawsegs;


    R_SortVisSprites();

    // draw all vissprites back to front
    for (i = num_vissprite ;--i>=0; )
    {
        pcfx_present_tick();
        R_DrawSprite(vissprite_ptrs[i]);         // killough
    }

    // render any remaining masked mid textures

    // Modified by Lee Killough:
    // (pointer check was originally nonportable
    // and buggy, by going past LEFT end of array):
    for (ds=ds_p ; ds-- > drawsegs ; )  // new -- killough
        if (ds->maskedtexturecol)
        {
            pcfx_present_tick();
            R_RenderMaskedSegRange(ds, ds->x1, ds->x2);
        }

    R_DrawPlayerSprites ();
}


//
// R_DrawSpan
// With DOOM style restrictions on view orientation,
//  the floors and ceilings consist of horizontal slices
//  or spans with constant z depth.
// However, rotation around the world z axis is possible,
//  thus this mapping, while simpler and faster than
//  perspective correct texture mapping, has to traverse
//  the texture at an angle in all but a few cases.
// In consequence, flats are not stored by column (like walls),
//  and the inner loop has to step in texture space u and v.
//

#pragma GCC push_options
#pragma GCC optimize ("Ofast")

inline static void R_DrawSpanPixel(unsigned short* dest, const byte* source, const lighttable_t* colormap, unsigned int position)
{
    /* PC-FX: write straight into the KRAM span cursor (set by FB_span). `source`
     * is the raw 8bpp flat and `colormap` is this row's diminished-light map (it
     * changes with depth down the plane) whose entries are pre-doubled fat pixels. */
    (void)dest;
    FB_put(colormap[source[((position >> 4) & 0x0fc0) | (position >> 26)]]);
}

#ifdef PCFX_TEXTURE_PAGES
/* Hand-scheduled two-load span kernel (platform/pcfx_span32.S). It exists for
 * the 2 KiB DRAM page, not for instruction count: the C drawer below alternates
 * flat[texel] and colormap[texel] loads, which live in different DRAM pages, so
 * every pixel paid two +3-cycle page changes. The kernel clusters four texel
 * loads then four colormap loads -> two page changes per four pixels. Output is
 * bit-identical. Define PCFX_SPAN32_C_REFERENCE to fall back to the C drawer. */
extern void pcfx_span32(unsigned int count, const draw_span_vars_t *dsvars);

/* The kernel reads the descriptor with fixed offsets. */
typedef char pcfx_span32_layout_check[
    (offsetof(draw_span_vars_t, position) == 0 &&
     offsetof(draw_span_vars_t, step)     == 4 &&
     offsetof(draw_span_vars_t, source)   == 8 &&
     offsetof(draw_span_vars_t, colormap) == 12) ? 1 : -1];

inline static void R_DrawSpanPixel32(const byte* source,
                                     const lighttable_t* colormap,
                                     unsigned int position)
{
    // Original 64-unit world repeat is retained: U is halved into 32 columns,
    // while V remains 64 rows. The page is row-major (row stride 32).
    unsigned int index = ((position >> 5) & 0x07e0) | (position >> 27);
    FB_put(colormap[source[index]]);
}

#ifdef PCFX_SPAN32_C_REFERENCE
static void R_DrawSpan32(unsigned int y, unsigned int x1, unsigned int x2,
                         const draw_span_vars_t *dsvars)
{
    rprof_sample();
    unsigned int count = x2 - x1;
    const byte *source = dsvars->source;
    const lighttable_t *colormap = dsvars->colormap;
    unsigned int position = dsvars->position;
    const unsigned int step = dsvars->step;
    FB_span(x1, y);

    unsigned int l = count >> 3;
    while (l--)
    {
        R_DrawSpanPixel32(source, colormap, position); position += step;
        R_DrawSpanPixel32(source, colormap, position); position += step;
        R_DrawSpanPixel32(source, colormap, position); position += step;
        R_DrawSpanPixel32(source, colormap, position); position += step;
        R_DrawSpanPixel32(source, colormap, position); position += step;
        R_DrawSpanPixel32(source, colormap, position); position += step;
        R_DrawSpanPixel32(source, colormap, position); position += step;
        R_DrawSpanPixel32(source, colormap, position); position += step;
    }
    switch (count & 7)
    {
        case 7: R_DrawSpanPixel32(source, colormap, position); position += step;
        case 6: R_DrawSpanPixel32(source, colormap, position); position += step;
        case 5: R_DrawSpanPixel32(source, colormap, position); position += step;
        case 4: R_DrawSpanPixel32(source, colormap, position); position += step;
        case 3: R_DrawSpanPixel32(source, colormap, position); position += step;
        case 2: R_DrawSpanPixel32(source, colormap, position); position += step;
        case 1: R_DrawSpanPixel32(source, colormap, position);
    }
}
#endif /* PCFX_SPAN32_C_REFERENCE */
#endif

static void R_DrawSpan(unsigned int y, unsigned int x1, unsigned int x2, const draw_span_vars_t *dsvars)
{
    rprof_sample();
    unsigned int count = (x2 - x1);

    const byte *source = dsvars->source;
    const byte *colormap = dsvars->colormap;

    /* Aim the KRAM cursor at this span (auto-increments one word per pixel).
     * `dest` is retained only so the unrolled loop compiles; the store target is
     * the KRAM cursor inside R_DrawSpanPixel. */
    unsigned short* dest = drawvars.byte_topleft;
    FB_span((unsigned)x1, (unsigned)y);

    const unsigned int step = dsvars->step;
    unsigned int position = dsvars->position;

    /* 8x unroll (was 16x): fit the hot span drawer in the 1 KB V810 icache. */
    unsigned int l = (count >> 3);

    while(l--)
    {
        R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;

        R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
    }

    unsigned int r = (count & 7);

    switch(r)
    {
        case 7:     R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        case 6:     R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        case 5:     R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        case 4:     R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        case 3:     R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        case 2:     R_DrawSpanPixel(dest, source, colormap, position); dest++; position+=step;
        case 1:     R_DrawSpanPixel(dest, source, colormap, position);
    }
}

#ifndef GBA
/* Pre-lit span: `lit` already holds the depth-lit palette index for each texel, so
 * the hot pixel is ONE load + KRAM store (vs. R_DrawSpanPixel's two dependent
 * loads: flat[texel] then colormap[that]). FB_puti re-doubles the index to a fat
 * pixel — output is bit-identical to R_DrawSpan. Same 8x unroll / icache fit. */
inline static void R_DrawLitSpanPixel(const byte* lit, unsigned int position)
{
    unsigned int idx = lit[((position >> 4) & 0x0fc0) | (position >> 26)];
    FB_put((unsigned short)(idx | (idx << 8)));   /* re-double to a fat pixel */
}

#ifdef PCFX_TEXTURE_PAGES
extern void pcfx_span_cached16(unsigned int count,
                               const unsigned short *pixels);
extern void pcfx_kram_read_cached16(unsigned int count,
                                    unsigned short *pixels);
#ifdef PCFX_SPAN_C_REFERENCE
inline static void R_DrawLitSpanPixel32(const byte *lit, unsigned int position)
{
    unsigned int index = ((position >> 5) & 0x07e0) | (position >> 27);
    unsigned int colour = lit[index];
    FB_put((unsigned short)(colour | (colour << 8)));
}
#else
extern void pcfx_span_lit32(unsigned int count, unsigned int position,
                            unsigned int step, const byte *lit);
extern void pcfx_bake_litflat2048(const byte *source,
                                  const lighttable_t *colormap, byte *dest);
extern void pcfx_span_lit32_record(unsigned int count, unsigned int position,
                                   unsigned int step, const byte *lit);
unsigned short *g_pcfx_plane_record_dst;
#endif
#endif

static void R_DrawSpanLit(unsigned int y, unsigned int x1, unsigned int x2,
                          unsigned int position, unsigned int step,
                          const byte* lit)
{
    rprof_sample();
    unsigned int count = (x2 - x1);

    FB_span((unsigned)x1, (unsigned)y);

    unsigned int l = (count >> 3);
    while(l--)
    {
        R_DrawLitSpanPixel(lit, position); position+=step;
        R_DrawLitSpanPixel(lit, position); position+=step;
        R_DrawLitSpanPixel(lit, position); position+=step;
        R_DrawLitSpanPixel(lit, position); position+=step;

        R_DrawLitSpanPixel(lit, position); position+=step;
        R_DrawLitSpanPixel(lit, position); position+=step;
        R_DrawLitSpanPixel(lit, position); position+=step;
        R_DrawLitSpanPixel(lit, position); position+=step;
    }

    unsigned int r = (count & 7);
    switch(r)
    {
        case 7:     R_DrawLitSpanPixel(lit, position); position+=step;
        case 6:     R_DrawLitSpanPixel(lit, position); position+=step;
        case 5:     R_DrawLitSpanPixel(lit, position); position+=step;
        case 4:     R_DrawLitSpanPixel(lit, position); position+=step;
        case 3:     R_DrawLitSpanPixel(lit, position); position+=step;
        case 2:     R_DrawLitSpanPixel(lit, position); position+=step;
        case 1:     R_DrawLitSpanPixel(lit, position);
    }
}

#ifdef PCFX_TEXTURE_PAGES
static void R_DrawSpanLit32(unsigned int y, unsigned int x1, unsigned int x2,
                            unsigned int position, unsigned int step,
                            const byte* lit)
{
    rprof_sample();
    unsigned int count = x2 - x1;
    FB_span(x1, y);
#ifdef PCFX_SPAN_C_REFERENCE
    unsigned int blocks = count >> 2;
    while (blocks--)
    {
        R_DrawLitSpanPixel32(lit, position); position += step;
        R_DrawLitSpanPixel32(lit, position); position += step;
        R_DrawLitSpanPixel32(lit, position); position += step;
        R_DrawLitSpanPixel32(lit, position); position += step;
    }
    switch (count & 3)
    {
        case 3: R_DrawLitSpanPixel32(lit, position); position += step;
        case 2: R_DrawLitSpanPixel32(lit, position); position += step;
        case 1: R_DrawLitSpanPixel32(lit, position);
    }
#else
    pcfx_span_lit32(count, position, step, lit);
#endif
}
#endif
#endif

#pragma GCC pop_options

#ifndef GBA
/* Pre-lit dense wall-column cache. Up to 512 entries retain substantially more
 * of the full-turn working set. Tight maps fall back to the proven 256 entries. */
#ifdef PCFX_TEXTURE_PAGES
#define LITWALL_MAX_SLOTS      512u
#define LITWALL_MIN_SLOTS      256u
#ifndef LITWALL_WAYS
#define LITWALL_WAYS           4u
#endif
#define LITWALL_COLUMN_TEXELS  64u
#define LITWALL_COLUMN_BYTES   (LITWALL_COLUMN_TEXELS * sizeof(litwall_pixel_t))
/* The existing lit-flat cache is allocated first.  Keep a smaller margin above
 * the 180 KB gameplay-decompression reserve here so E1M6 can hold both; this
 * block is PU_CACHE and is automatically discarded if that reserve is needed. */
#ifndef LITWALL_HEAP_FLOOR
#define LITWALL_HEAP_FLOOR     (192u * 1024u)
#endif
/* A rapid in-place turn briefly exceeds the old 64-bake cutoff while the
 * rolling view warms, then settles around 43 bakes/frame with roughly 106
 * hits/frame on E1M6.  Disabling permanently after that transient discarded a
 * measured 16% whole-frame win.  Keep the guard for genuinely pathological
 * maps, above the observed 185-tuple turning peak. */
#define LITWALL_THRASH_BAKES   192u
#define LITWALL_THRASH_STRIKES 8

static byte *litwall_block;
static byte *litwall_data;
static uint32_t *litwall_keys;
static byte *litwall_repl;
static int litwall_slots;
static unsigned litwall_sets;
static uint32_t lw_bakes, lw_bakes_prev;
static int lw_strikes;
#ifdef LITWALL_FORCE_OFF
volatile int g_litwall_mode = 2;
#else
volatile int g_litwall_mode = 1;
#endif

static inline uint32_t R_LitWallKey(int lump, unsigned column,
                                    const lighttable_t *cmap)
{
    unsigned cmap_id = (unsigned)(cmap - fullcolormap) >> 8;
    /* Six cmap bits keep Doom's two special maps (32/33) distinct from normal
     * light map 0/1 when a fixed colormap power-up is active. */
    return ((uint32_t)(lump + 1) << 11) | ((cmap_id & 63u) << 5) |
           (column & 31u);
}

static inline unsigned R_LitWallSet(uint32_t key)
{
    return (key ^ (key >> 7) ^ (key >> 14)) & (litwall_sets - 1);
}

static void R_LitWallLevelInit(void)
{
    if (litwall_block)
        Z_Free(litwall_block);
    litwall_block = litwall_data = NULL;
    litwall_keys = NULL;
    litwall_repl = NULL;
    litwall_slots = 0;
    litwall_sets = 0;
    lw_bakes = lw_bakes_prev = 0;
    lw_strikes = 0;

    /* Both ablation builds retain identical code/layout; only the non-zero data
     * initializer differs, so one saved state is valid for bit-exact A/B runs. */
    unsigned slots = LITWALL_MAX_SLOTS;
    while (g_litwall_mode != 2 && slots >= LITWALL_MIN_SLOTS)
    {
        const unsigned sets = slots / LITWALL_WAYS;
        const unsigned data_bytes = slots * LITWALL_COLUMN_BYTES;
        const unsigned key_bytes = slots * sizeof(uint32_t);
        const unsigned repl_bytes = sets;
        const unsigned need = data_bytes + key_bytes + repl_bytes;
        if (Z_LargestFreeBlock() >= need &&
            Z_TotalFree() >= need + LITWALL_HEAP_FLOOR)
        {
            litwall_block = Z_Malloc((int)need, PU_CACHE,
                                     (void **)&litwall_block);
            if (litwall_block)
            {
                litwall_data = litwall_block;
                litwall_keys = (uint32_t *)(litwall_data + data_bytes);
                litwall_repl = (byte *)litwall_keys + key_bytes;
                litwall_slots = (int)slots;
                litwall_sets = sets;
                memset(litwall_repl, 0, repl_bytes);
                memset(litwall_keys, 0, key_bytes);
                break;
            }
        }
        slots >>= 1;
    }
}

static void R_LitWallFrameCheck(void)
{
    if (g_litwall_mode == 2)
    {
        litwall_slots = 0;
        if (litwall_block)
            Z_Free(litwall_block);
        return;
    }
    if (!litwall_slots)
        return;
    uint32_t baked = lw_bakes - lw_bakes_prev;
    lw_bakes_prev = lw_bakes;
    if (baked > LITWALL_THRASH_BAKES)
    {
        if (++lw_strikes >= LITWALL_THRASH_STRIKES)
        {
            litwall_slots = 0;
            if (litwall_block)
                Z_Free(litwall_block);
        }
    }
    else
        lw_strikes = 0;
}

static __attribute__((noinline,cold)) const litwall_pixel_t *
R_BakeLitWallColumn(uint32_t key, unsigned set,
                    const lighttable_t *cmap, const byte *source)
{
    unsigned base = set * LITWALL_WAYS;
#ifdef SERIAL_LOG
    g_rp_wall_lit_misses++;
    g_rp_wall_lit_bakes++;
#endif
    lw_bakes++;
    unsigned slot = base + (litwall_repl[set]++ & (LITWALL_WAYS - 1));

    if (litwall_keys[slot])
    {
#ifdef SERIAL_LOG
        g_rp_wall_lit_evictions++;
#endif
    }

    litwall_pixel_t *dst = (litwall_pixel_t *)
        (litwall_data + slot * LITWALL_COLUMN_BYTES);
    pcfx_bake_litwall64(source, cmap, dst);
    litwall_keys[slot] = key;
    return dst;
}

static inline __attribute__((always_inline)) const litwall_pixel_t *
R_GetLitWallColumn(int lump, unsigned column, const lighttable_t *cmap,
                   const byte *source)
{
    if (!litwall_block)
    {
        litwall_slots = 0;
        return NULL;
    }

    uint32_t key = R_LitWallKey(lump, column, cmap);
    unsigned set = R_LitWallSet(key);
    unsigned base = set * LITWALL_WAYS;
    for (unsigned way = 0; way < LITWALL_WAYS; way++)
    {
        unsigned slot = base + way;
        if (litwall_keys[slot] == key)
        {
#ifdef SERIAL_LOG
            g_rp_wall_lit_hits++;
#endif
            return (const litwall_pixel_t *)
                (litwall_data + slot * LITWALL_COLUMN_BYTES);
        }
    }
    return R_BakeLitWallColumn(key, set, cmap, source);
}
#endif

// Pre-lit flat cache. A flat span's hot pixel is bound by TWO dependent external
// loads: source[texel] then colormap[that] (see R_DrawSpanPixel). We bake the
// depth-lit palette INDEX (low byte of colormap[flat[i]]) into an 8-bit lit-flat
// once per (flat-lump, colormap) pair, so the span loop becomes ONE load + store
// (R_DrawSpanLit re-doubles via FB_puti). Output is bit-identical.
//
// The cache is persistent across frames, so a bake amortizes over every frame
// that reuses that (flat,colormap) pair. A single E1M6 frame's working set is
// ~34-40 distinct pairs (measured), so the slot count must cover a whole frame
// or it thrashes (re-bakes every frame) — 8 slots thrash even on a static view
// (~31 bakes/frame); 48 slots hold the measured complete E1M6 turning set.
//
// The buffer comes from the ZONE, not a static .bss array: a static array this
// size gets laid across the V810 __gp pointer, and baking it overwrites the
// gp-relative-addressed libc/liberis globals (hard hang). Zone memory sits above
// the gp window.
//
// SIZING IS DYNAMIC AND PER-LEVEL. The cache must cover a whole frame's working
// set or it thrashes (re-bakes 4096 texels/miss) — worse than the 2-load draw.
// A full LITFLAT_MAX_SLOTS cache is 256 KB for original 64x64 flats, or 128 KB
// for the generated 32x64 PC-FX pages. R_LitFlatLevelInit() grabs it at level
// load ONLY if the free heap allows while leaving the gameplay-decompress floor
// intact; tighter maps fall back to the 2-load drawer with no correctness cost.
//
// It is deliberately an owner-tracked PU_CACHE block, not pinned PU_LEVEL.
// Gameplay texture/patch decompression can fragment the nominal reserve until
// Z_TotalFree() is ample but no single hole is large enough. In that situation
// Z_Malloc must be able to purge this optional optimization. Z_Free then
// clears litflat_data through the owner pointer; the next plane notices and
// permanently falls back to the ordinary two-load span path for this level.
// A runtime thrash guard also disables the lit path if a map's set exceeds it.
#define LITFLAT_MAX_SLOTS   64
#define LITFLAT_TEXELS      4096        // original 64*64 flat
// Keep at least this much free after grabbing the cache — above the 180 KB
// gameplay-decompress floor (w_wad.c GAMEPLAY_RESERVE), with margin.
#define LITFLAT_HEAP_FLOOR  (224u * 1024u)
// If a frame bakes more than this many flats, the map's working set exceeds the
// cache (thrash). A short run of such frames disables the lit path for the level.
#define LITFLAT_THRASH_BAKES  16
#define LITFLAT_THRASH_STRIKES 8

// Stored as 8-bit LIT INDICES (colormap[flat[i]] is a pre-doubled fat-pixel word;
// its low byte is the palette index). Half the RAM of storing the 16-bit word, so
// more maps afford the full cache; R_DrawSpanLit re-doubles via FB_puti (identical
// output, +1 shift/or per pixel — cheaper than the RAM load it removes).
static byte *litflat_data;              // [litflat_slots][litflat_stride], zone
static unsigned litflat_stride;         // 2048 for PC-FX builds, otherwise 4096
static int litflat_lump[LITFLAT_MAX_SLOTS];
static const lighttable_t* litflat_cmap[LITFLAT_MAX_SLOTS];
#ifdef PCFX_TEXTURE_PAGES
static byte litflat_pcfx32[LITFLAT_MAX_SLOTS];
#endif
#define LITFLAT_HASH_SIZE 128            // 2x slots; power of two
static signed char litflat_hash[LITFLAT_HASH_SIZE]; // -1 empty, -2 tombstone, else slot
static unsigned int litflat_next;
static int litflat_slots;               // 0 = disabled (2-load fallback) this level
static uint32_t lf_bakes, lf_bakes_prev;
static int lf_strikes;
static int s_flatlump;                  // current visplane's flat lump number
static int litflat_last_lump;
static const lighttable_t *litflat_last_cmap;
static const byte *litflat_last_result;
#ifdef PCFX_TEXTURE_PAGES
static boolean litflat_last_pcfx32;
#endif

#ifdef PCFX_TEXTURE_PAGES
/* Exact previous-frame plane-span replay.  Static views repeatedly submit the
 * same (geometry, mapping, flat, light) spans; recomputing 20K texture
 * coordinates for those spans is wasted work.  Cache the final doubled 16-bit
 * words and replay them through direct KRAM writes.
 * Any descriptor mismatch switches the rest of the frame to record mode, so
 * moving geometry never consumes stale pixels and refreshes the next frame's
 * cache in-place. */
#define PLANESPAN_CACHE_MAX_SPANS 512u
#define PLANESPAN_CACHE_PIXELS    (SCREENWIDTH * (SCREENHEIGHT - ST_SCALED_HEIGHT))
#define PLANESPAN_CACHE_HEAP_FLOOR (128u * 1024u)

typedef struct
{
    uint32_t position;
    uint32_t step;
    const byte *lit;
    const lighttable_t *cmap;
    int lump;
    uint16_t y;
    uint8_t x1;
    uint8_t count;
} planespan_cache_desc_t;

#define PLANESPAN_CACHE_DESC_BYTES \
    (PLANESPAN_CACHE_MAX_SPANS * sizeof(planespan_cache_desc_t))

static byte *planespan_cache_block;
static planespan_cache_desc_t *planespan_cache_desc;
static unsigned short *planespan_cache_pixels;
static unsigned planespan_cached_spans;
static unsigned planespan_cached_pixels;
static unsigned planespan_frame_spans;
static unsigned planespan_frame_pixels;
static boolean planespan_cache_valid;
static boolean planespan_cache_replay;
static boolean planespan_cache_failed;
static boolean planespan_cache_bypass;
static boolean planespan_have_view;
static fixed_t planespan_last_viewx;
static fixed_t planespan_last_viewy;
static fixed_t planespan_last_viewz;
static angle_t planespan_last_viewangle;
#ifdef PLANESPAN_CACHE_FORCE_OFF
volatile int g_planespan_cache_mode = 0;
#else
volatile int g_planespan_cache_mode = 1;
#endif

/* Stationary-view opaque-background replay.
 *
 * Walls/BSP plus planes cost roughly 60 ms in the measured E1M6 frame, while
 * sprites and masked walls must remain live because actors change every tic.
 * After an ordinary render we read the completed opaque background (before the
 * masked pass) into the same optional cache allocation used by plane replay.
 * A later frame with identical background inputs streams those 128x208 words
 * straight back through the KING write port, rebuilds the current sprite list,
 * and runs the normal masked pass over the clean background.  Direct KRAM output
 * remains the only display path; this cache is temporal source data, not a RAM
 * framebuffer used by the ordinary renderer.
 *
 * Dependencies are recorded only for sectors/sides/textures/flats consumed by
 * the last traversal.  Overflow or a purged PU_CACHE block falls back to the
 * complete renderer. */
#define FRAME_BG_MAX_SECTORS 128u
#define FRAME_BG_MAX_SIDES   128u
#define FRAME_BG_MAX_TEXTURES 64u
#define FRAME_BG_MAX_FLATS    32u

static unsigned short frame_bg_sectors[FRAME_BG_MAX_SECTORS];
static unsigned short frame_bg_sprite_sectors[FRAME_BG_MAX_SECTORS];
static unsigned short frame_bg_sides[FRAME_BG_MAX_SIDES];
static unsigned short frame_bg_textures[FRAME_BG_MAX_TEXTURES];
static unsigned short frame_bg_flats[FRAME_BG_MAX_FLATS];
static unsigned frame_bg_num_sectors;
static unsigned frame_bg_num_sprite_sectors;
static unsigned frame_bg_num_sides;
static unsigned frame_bg_num_textures;
static unsigned frame_bg_num_flats;
static unsigned short *frame_bg_pixels;
static unsigned short *frame_bg_openings;
static unsigned frame_bg_opening_count;
static unsigned frame_bg_opening_capacity;
static uint32_t frame_bg_signature;
static fixed_t frame_bg_viewx, frame_bg_viewy, frame_bg_viewz;
static angle_t frame_bg_viewangle;
static boolean frame_bg_valid;
static boolean frame_bg_overflow;
static boolean frame_bg_recording;
static boolean frame_bg_have_last_view;
static fixed_t frame_bg_last_viewx, frame_bg_last_viewy, frame_bg_last_viewz;
static angle_t frame_bg_last_viewangle;

/* Each KRAM framebuffer page retains the last opaque background plus the live
 * masked pixels subsequently drawn over it.  Once a page has been primed with
 * the current cached background, restore only those overwritten intervals on
 * its next use instead of streaming all 26,624 words again.  One min/max run
 * per logical column is deliberately conservative: it may restore transparent
 * gaps between posts, but can never leave a stale sprite pixel behind. */
static uint8_t frame_bg_dirty_top[KFB_NUM_BUFS][SCREENWIDTH];
static uint8_t frame_bg_dirty_bottom[KFB_NUM_BUFS][SCREENWIDTH];
static boolean frame_bg_page_valid[KFB_NUM_BUFS];
static boolean frame_bg_dirty_recording;
static unsigned frame_bg_dirty_page;

/* Final masked-wall columns are invariant while the background signature is
 * valid, but must still be submitted at their normal points among live sprites.
 * Cache them by (drawseg, screen x) in the otherwise-unused tail of the opening
 * snapshot area.  No additional zone allocation is needed. */
#define FRAME_BG_MASK_HASH_SIZE 256u
typedef struct
{
    uint16_t key;
    uint16_t offset;
} frame_bg_mask_ref_t;

static frame_bg_mask_ref_t *frame_bg_mask_refs;
static byte *frame_bg_mask_base;
static uint8_t *frame_bg_mask_restore_top;
static uint8_t *frame_bg_mask_restore_bottom;
static unsigned frame_bg_mask_capacity;
static unsigned frame_bg_mask_used;
static unsigned frame_bg_mask_entry_start;
static unsigned frame_bg_mask_entry_posts;
static unsigned frame_bg_mask_entry_slot;
static uint16_t frame_bg_mask_entry_key;
static boolean frame_bg_mask_entry_active;
static boolean frame_bg_mask_entry_failed;
static boolean frame_bg_mask_recording;
static boolean frame_bg_mask_replay;

#ifdef PCFX_FRAME_BG_MASK_FORCE_OFF
volatile int g_pcfx_frame_bg_mask_mode = 0;
#else
volatile int g_pcfx_frame_bg_mask_mode = 1;
#endif

#ifdef PCFX_FRAME_BG_DIRTY_FORCE_OFF
volatile int g_pcfx_frame_bg_dirty_mode = 0;
#else
volatile int g_pcfx_frame_bg_dirty_mode = 1;
#endif

extern void pcfx_column_cached16(unsigned int count,
                                 const unsigned short *source,
                                 unsigned int source_stride_bytes);
#ifdef PCFX_FRAME_BG_FORCE_OFF
volatile int g_pcfx_frame_bg_mode = 0;
#else
volatile int g_pcfx_frame_bg_mode = 1;
#endif

static inline uint32_t R_FrameBgMix(uint32_t hash, uint32_t value)
{
    return hash ^ (value + 0x9e3779b9u + (hash << 6) + (hash >> 2));
}

static __attribute__((noinline,cold)) uint32_t
R_FrameBgSignature(const player_t *player)
{
    uint32_t h = 0x811c9dc5u;
#define FRAME_BG_MIX(v) do { h = R_FrameBgMix(h, (uint32_t)(v)); } while (0)
    FRAME_BG_MIX(player->mo->x);
    FRAME_BG_MIX(player->mo->y);
    FRAME_BG_MIX(player->mo->angle);
    FRAME_BG_MIX(player->viewz);
    FRAME_BG_MIX(player->extralight);
    FRAME_BG_MIX(player->fixedcolormap);

    for (unsigned i = 0; i < frame_bg_num_sectors; i++)
    {
        const sector_t *sec = &_g->sectors[frame_bg_sectors[i]];
        FRAME_BG_MIX(sec->floorheight);
        FRAME_BG_MIX(sec->ceilingheight);
        FRAME_BG_MIX((unsigned short)sec->floorpic |
                     ((uint32_t)(unsigned short)sec->ceilingpic << 16));
        FRAME_BG_MIX((unsigned short)sec->lightlevel);
    }
    for (unsigned i = 0; i < frame_bg_num_sides; i++)
    {
        const side_t *side = &_g->sides[frame_bg_sides[i]];
        FRAME_BG_MIX((unsigned short)side->textureoffset |
                     ((uint32_t)(unsigned short)side->rowoffset << 16));
        FRAME_BG_MIX(side->toptexture | (side->bottomtexture << 10) |
                     (side->midtexture << 20));
    }
    for (unsigned i = 0; i < frame_bg_num_textures; i++)
        FRAME_BG_MIX((unsigned short)texturetranslation[frame_bg_textures[i]]);
    for (unsigned i = 0; i < frame_bg_num_flats; i++)
        FRAME_BG_MIX((unsigned short)flattranslation[frame_bg_flats[i]]);
#undef FRAME_BG_MIX
    return h;
}

static void R_FrameBgBeginRender(void)
{
    frame_bg_num_sectors = 0;
    frame_bg_num_sprite_sectors = 0;
    frame_bg_num_sides = 0;
    frame_bg_num_textures = 0;
    frame_bg_num_flats = 0;
    frame_bg_overflow = false;
}

static boolean R_FrameBgEligible(void);

static inline unsigned R_FrameBgPage(void)
{
    /* True render-buffer index (0..KFB_NUM_BUFS-1). The old ">= KFB_PAGE_WORDS
     * ? 1 : 0" aliased the deferred presenter's third buffer onto page 1's
     * validity/dirty slot, so dirty-only repair ran against a buffer holding
     * different content — the level-start flicker that lasted until the first
     * movement forced full renders. (Buffer bases are no longer a linear
     * array — buffer 2 lives in the bit-17 KRAM half — so the index comes
     * from pcfx_fb_buf_index, not division.) */
    return pcfx_fb_buf_index(g_pcfx_fb_base);
}

static void R_FrameBgClearDirty(unsigned page)
{
    memset(frame_bg_dirty_top[page], 0xff,
           sizeof(frame_bg_dirty_top[page]));
    memset(frame_bg_dirty_bottom[page], 0,
           sizeof(frame_bg_dirty_bottom[page]));
}

static void R_FrameBgMarkDirty(unsigned x, int yl, int yh, boolean fuzz)
{
    if (!frame_bg_dirty_recording || x >= SCREENWIDTH)
        return;
    if (fuzz)
    {
        if (yl <= 0) yl = 1;
        if (yh >= viewheight - 1) yh = viewheight - 2;
    }
    if (yl < 0) yl = 0;
    if (yh >= viewheight) yh = viewheight - 1;
    if (yl > yh)
        return;

    unsigned page = frame_bg_dirty_page;
    if ((unsigned)yl < frame_bg_dirty_top[page][x])
        frame_bg_dirty_top[page][x] = (uint8_t)yl;
    if ((unsigned)yh > frame_bg_dirty_bottom[page][x])
        frame_bg_dirty_bottom[page][x] = (uint8_t)yh;
}

static inline boolean R_FrameBgMaskedKey(const drawseg_t *ds, unsigned x,
                                         uint16_t *key)
{
    unsigned dsi = (unsigned)(ds - _g->drawsegs);
    if (dsi >= MAXDRAWSEGS || x >= SCREENWIDTH)
        return false;
    *key = (uint16_t)(dsi * SCREENWIDTH + x);
    return true;
}

static inline unsigned R_FrameBgMaskedHash(uint16_t key)
{
    return ((unsigned)key * 33u ^ ((unsigned)key >> 5)) &
           (FRAME_BG_MASK_HASH_SIZE - 1u);
}

static void R_FrameBgMaskedBeginCapture(void)
{
    frame_bg_mask_refs = NULL;
    frame_bg_mask_base = NULL;
    frame_bg_mask_restore_top = NULL;
    frame_bg_mask_restore_bottom = NULL;
    frame_bg_mask_capacity = frame_bg_mask_used = 0;
    frame_bg_mask_entry_active = false;
    frame_bg_mask_entry_failed = false;
    frame_bg_mask_replay = false;
    frame_bg_mask_recording = false;

    if (!g_pcfx_frame_bg_mask_mode || !planespan_cache_block ||
        !frame_bg_openings)
        return;

    uintptr_t desc_begin = (uintptr_t)frame_bg_openings;
    uintptr_t desc_end = desc_begin + PLANESPAN_CACHE_DESC_BYTES;
    uintptr_t refs_at = (uintptr_t)(frame_bg_openings + frame_bg_opening_count);
    refs_at = (refs_at + 3u) & ~(uintptr_t)3u;
    uint8_t *restore_top = (uint8_t *)refs_at;
    uint8_t *restore_bottom = restore_top + SCREENWIDTH;
    refs_at += 2u * SCREENWIDTH;
    uintptr_t data_at = refs_at +
        FRAME_BG_MASK_HASH_SIZE * sizeof(frame_bg_mask_ref_t);
    if (data_at >= desc_end)
        return;

    frame_bg_mask_restore_top = restore_top;
    frame_bg_mask_restore_bottom = restore_bottom;
    frame_bg_mask_refs = (frame_bg_mask_ref_t *)refs_at;
    frame_bg_mask_base = (byte *)data_at;
    frame_bg_mask_capacity = (unsigned)(desc_end - data_at);
    memset(frame_bg_mask_refs, 0xff,
           FRAME_BG_MASK_HASH_SIZE * sizeof(*frame_bg_mask_refs));
    frame_bg_mask_recording = true;
}

static void R_FrameBgMaskedRecordBegin(const drawseg_t *ds, unsigned x)
{
    frame_bg_mask_entry_active = false;
    frame_bg_mask_entry_failed = false;
    if (!frame_bg_mask_recording || !planespan_cache_block ||
        !frame_bg_mask_refs || frame_bg_mask_used + 4u > frame_bg_mask_capacity)
        return;

    uint16_t key;
    if (!R_FrameBgMaskedKey(ds, x, &key))
        return;

    unsigned slot = R_FrameBgMaskedHash(key);
    for (unsigned probes = 0; probes < FRAME_BG_MASK_HASH_SIZE; probes++)
    {
        if (frame_bg_mask_refs[slot].key == 0xffffu ||
            frame_bg_mask_refs[slot].key == key)
            break;
        slot = (slot + 1u) & (FRAME_BG_MASK_HASH_SIZE - 1u);
        if (probes + 1u == FRAME_BG_MASK_HASH_SIZE)
            return;
    }

    frame_bg_mask_entry_start = frame_bg_mask_used;
    frame_bg_mask_entry_posts = 0;
    frame_bg_mask_entry_slot = slot;
    frame_bg_mask_entry_key = key;
    frame_bg_mask_used += 4u; /* byte post count plus three reserved bytes */
    frame_bg_mask_entry_active = true;
}

/* Draw one already-clipped masked-wall post while recording its final lit KRAM
 * words. This duplicates the compact generic column arithmetic only on capture
 * frames; replay becomes a sequential halfword load plus direct KRAM write. */
static boolean R_FrameBgMaskedRecordPost(const draw_column_vars_t *dcvars)
{
    if (!frame_bg_mask_entry_active || frame_bg_mask_entry_failed ||
        !planespan_cache_block)
        return false;

    unsigned count = (unsigned)(dcvars->yh - dcvars->yl + 1);
    unsigned bytes = 2u + count * sizeof(uint16_t);
    if (!count || count > 256u || frame_bg_mask_entry_posts >= 255u ||
        frame_bg_mask_used + bytes > frame_bg_mask_capacity)
    {
        frame_bg_mask_entry_failed = true;
        return false;
    }

    byte *post = frame_bg_mask_base + frame_bg_mask_used;
    post[0] = (byte)dcvars->yl;
    post[1] = (byte)(count - 1u);
    uint16_t *record = (uint16_t *)(post + 2);

    rprof_sample();
    const byte *source = dcvars->source;
    const lighttable_t *colormap = dcvars->colormap;
    unsigned fracstep = dcvars->iscale << COLEXTRABITS;
    unsigned frac = (dcvars->texturemid +
        (dcvars->yl - centery) * dcvars->iscale) << COLEXTRABITS;
    FB_col((unsigned)dcvars->x, (unsigned)dcvars->yl);
    for (unsigned i = 0; i < count; i++)
    {
        uint16_t pixel = colormap[source[frac >> COLBITS]];
        record[i] = pixel;
        FB_put(pixel);
        frac += fracstep;
    }

    frame_bg_mask_used += bytes;
    frame_bg_mask_entry_posts++;
    return true;
}

static void R_FrameBgMaskedRecordEnd(void)
{
    if (!frame_bg_mask_entry_active)
        return;

    if (frame_bg_mask_entry_failed || !planespan_cache_block)
        frame_bg_mask_used = frame_bg_mask_entry_start;
    else
    {
        byte *header = frame_bg_mask_base + frame_bg_mask_entry_start;
        header[0] = (byte)frame_bg_mask_entry_posts;
        header[1] = header[2] = header[3] = 0;

        /* Publish the entry only after every post fits, so replay never sees a
         * partial column left behind by an exhausted descriptor tail. */
        frame_bg_mask_refs[frame_bg_mask_entry_slot].offset =
            (uint16_t)frame_bg_mask_entry_start;
        frame_bg_mask_refs[frame_bg_mask_entry_slot].key =
            frame_bg_mask_entry_key;
    }
    frame_bg_mask_entry_active = false;
}

static __attribute__((noinline,optimize("Os"))) boolean
R_FrameBgMaskedReplay(const drawseg_t *ds, unsigned x)
{
    if (!frame_bg_mask_replay || !planespan_cache_block ||
        !frame_bg_mask_refs || !frame_bg_mask_base)
        return false;

    uint16_t key;
    if (!R_FrameBgMaskedKey(ds, x, &key))
        return false;
    unsigned slot = R_FrameBgMaskedHash(key);
    for (unsigned probes = 0; probes < FRAME_BG_MASK_HASH_SIZE; probes++)
    {
        uint16_t stored = frame_bg_mask_refs[slot].key;
        if (stored == key)
            break;
        if (stored == 0xffffu)
            return false;
        slot = (slot + 1u) & (FRAME_BG_MASK_HASH_SIZE - 1u);
        if (probes + 1u == FRAME_BG_MASK_HASH_SIZE)
            return false;
    }

    unsigned offset = frame_bg_mask_refs[slot].offset;
    if (offset + 4u > frame_bg_mask_used)
        return false;
    const byte *post = frame_bg_mask_base + offset + 4u;
    const byte *end = frame_bg_mask_base + frame_bg_mask_used;
    unsigned posts = frame_bg_mask_base[offset];
    for (unsigned p = 0; p < posts; p++)
    {
        if (post + 2 > end)
            return false;
        unsigned y = post[0];
        unsigned count = (unsigned)post[1] + 1u;
        if (post + 2u + count * sizeof(uint16_t) > end ||
            y + count > (unsigned)viewheight)
            return false;
        const uint16_t *pixels = (const uint16_t *)(post + 2);
#ifdef PCFX_FRAME_BG_MASK_PERSIST_FORCE_OFF
        R_FrameBgMarkDirty(x, (int)y, (int)(y + count - 1u), false);
        FB_col(x, y);
        pcfx_span_cached16(count, pixels);
#ifdef SERIAL_LOG
        g_rp_masked_wall_pixels += count;
#endif
#else
        unsigned dirty_top = frame_bg_mask_restore_top[x];
        unsigned dirty_bottom = frame_bg_mask_restore_bottom[x];
        unsigned current_top = frame_bg_dirty_top[frame_bg_dirty_page][x];
        if (current_top != 0xffu)
        {
            unsigned current_bottom =
                frame_bg_dirty_bottom[frame_bg_dirty_page][x];
            if (dirty_top == 0xffu || current_top < dirty_top)
                dirty_top = current_top;
            if (dirty_top != 0xffu && current_bottom > dirty_bottom)
                dirty_bottom = current_bottom;
        }
        unsigned first = y > dirty_top ? y : dirty_top;
        unsigned last = y + count - 1u;
        if (last > dirty_bottom)
            last = dirty_bottom;
        if (first <= last)
        {
            unsigned write_count = last - first + 1u;
            FB_col(x, first);
            pcfx_span_cached16(write_count, pixels + first - y);
#ifdef SERIAL_LOG
            g_rp_masked_wall_pixels += write_count;
#endif
        }
#endif
        post += 2u + count * sizeof(uint16_t);
    }
    return true;
}

static void R_FrameBgPrepareRender(const player_t *player)
{
    frame_bg_dirty_recording = false;
    frame_bg_mask_recording = false;
    frame_bg_mask_replay = false;
    /* A complete render is about to overwrite the current KRAM target.  The
     * other page may still retain this cached background, but this page is no
     * longer eligible for dirty-only repair unless R_FrameBgCapture primes it
     * again below.  This is essential when the camera moves away and returns. */
    frame_bg_page_valid[R_FrameBgPage()] = false;
    frame_bg_recording = frame_bg_have_last_view && R_FrameBgEligible() &&
        player->mo && player->mo->x == frame_bg_last_viewx &&
        player->mo->y == frame_bg_last_viewy &&
        player->viewz == frame_bg_last_viewz &&
        player->mo->angle == frame_bg_last_viewangle;
    if (player->mo)
    {
        frame_bg_last_viewx = player->mo->x;
        frame_bg_last_viewy = player->mo->y;
        frame_bg_last_viewz = player->viewz;
        frame_bg_last_viewangle = player->mo->angle;
        frame_bg_have_last_view = true;
    }
    if (frame_bg_recording)
        R_FrameBgBeginRender();
}

static __attribute__((noinline,cold)) void
R_FrameBgMarkValue(unsigned short *values, unsigned *count,
                   unsigned limit, unsigned value)
{
    if (!frame_bg_recording || frame_bg_overflow)
        return;
    for (unsigned i = 0; i < *count; i++)
        if (values[i] == value)
            return;
    if (*count >= limit)
    {
        frame_bg_overflow = true;
        return;
    }
    values[(*count)++] = (unsigned short)value;
}

static void R_FrameBgMarkSector(const sector_t *sector, boolean sprites)
{
    if (!frame_bg_recording || !sector)
        return;
    unsigned index = (unsigned)(sector - _g->sectors);
    R_FrameBgMarkValue(frame_bg_sectors, &frame_bg_num_sectors,
                       FRAME_BG_MAX_SECTORS, index);
    if (sprites)
        R_FrameBgMarkValue(frame_bg_sprite_sectors,
                           &frame_bg_num_sprite_sectors,
                           FRAME_BG_MAX_SECTORS, index);
}

static boolean R_FrameBgEligible(void)
{
    return g_pcfx_frame_bg_mode && !_g->menuactive &&
           !(_g->automapmode & am_active);
}

static __attribute__((noinline,cold)) boolean
R_FrameBgProbe(const player_t *player)
{
    if (!planespan_cache_block)
    {
        frame_bg_pixels = NULL;
        frame_bg_valid = false;
        for (unsigned p = 0; p < KFB_NUM_BUFS; p++)
            frame_bg_page_valid[p] = false;
        frame_bg_dirty_recording = false;
        frame_bg_mask_recording = false;
        frame_bg_mask_replay = false;
    }
    if (!frame_bg_valid || !frame_bg_pixels || !R_FrameBgEligible() ||
        !player->mo || player->mo->x != frame_bg_viewx ||
        player->mo->y != frame_bg_viewy || player->viewz != frame_bg_viewz ||
        player->mo->angle != frame_bg_viewangle)
        return false;
    return R_FrameBgSignature(player) == frame_bg_signature;
}

static __attribute__((noinline,cold)) void
R_FrameBgCapture(const player_t *player)
{
    if (!frame_bg_recording || !planespan_cache_block || !frame_bg_pixels ||
        !frame_bg_openings ||
        frame_bg_overflow ||
        !R_FrameBgEligible())
    {
        frame_bg_valid = false;
        return;
    }

    frame_bg_opening_count = (unsigned)(_g->lastopening - _g->openings);
    if (frame_bg_opening_count > frame_bg_opening_capacity)
    {
        frame_bg_valid = false;
        return;
    }
    BlockCopy(frame_bg_openings, _g->openings,
              frame_bg_opening_count * sizeof(*frame_bg_openings));

    /* The independent KING read cursor can snapshot the hidden KRAM page after
     * opaque walls/planes without disturbing the direct write cursor. */
    king_kram_set_read(g_pcfx_fb_base, 1);
    pcfx_kram_read_cached16(PLANESPAN_CACHE_PIXELS, frame_bg_pixels);

    frame_bg_signature = R_FrameBgSignature(player);
    frame_bg_viewx = player->mo->x;
    frame_bg_viewy = player->mo->y;
    frame_bg_viewz = player->viewz;
    frame_bg_viewangle = player->mo->angle;
    frame_bg_valid = true;

    /* This page already contains the exact clean background we just captured.
     * Record what the following masked pass overwrites.  The other pages must
     * each be primed once with a full replay before dirty-only restoration is
     * safe on them. */
    for (unsigned p = 0; p < KFB_NUM_BUFS; p++)
        frame_bg_page_valid[p] = false;
    frame_bg_dirty_page = R_FrameBgPage();
    frame_bg_page_valid[frame_bg_dirty_page] = true;
    R_FrameBgClearDirty(frame_bg_dirty_page);
    frame_bg_dirty_recording = true;
    R_FrameBgMaskedBeginCapture();
}

static inline void R_FrameBgReplay(void)
{
    const unsigned page = R_FrameBgPage();
    if (!g_pcfx_frame_bg_dirty_mode || !frame_bg_page_valid[page])
    {
        /* The first use of a page after a new background capture is an exact
         * full replay.  From then on, only masked/sprite damage is repaired. */
        FB_span(0, 0);
        pcfx_span_cached16(PLANESPAN_CACHE_PIXELS, frame_bg_pixels);
        frame_bg_page_valid[page] = true;
#ifndef PCFX_FRAME_BG_MASK_PERSIST_FORCE_OFF
        if (frame_bg_mask_restore_top)
        {
            memset(frame_bg_mask_restore_top, 0, SCREENWIDTH);
            memset(frame_bg_mask_restore_bottom, viewheight - 1, SCREENWIDTH);
        }
#endif
    }
    else
    {
#ifndef PCFX_FRAME_BG_MASK_PERSIST_FORCE_OFF
        if (frame_bg_mask_restore_top)
        {
            BlockCopy(frame_bg_mask_restore_top,
                      frame_bg_dirty_top[page], SCREENWIDTH);
            BlockCopy(frame_bg_mask_restore_bottom,
                      frame_bg_dirty_bottom[page], SCREENWIDTH);
        }
#endif
        for (unsigned x = 0; x < SCREENWIDTH; x++)
        {
            unsigned top = frame_bg_dirty_top[page][x];
            if (top != 0xffu)
            {
                unsigned bottom = frame_bg_dirty_bottom[page][x];
                FB_col(x, top);
                pcfx_column_cached16(bottom - top + 1,
                    frame_bg_pixels + top * SCREENWIDTH + x,
                    SCREENWIDTH * sizeof(*frame_bg_pixels));
            }
        }
    }
    /* Masked-wall drawing consumes its texture-column entries by replacing
     * them with SHRT_MAX.  Restore the pre-masked openings image every replay
     * so masked walls and sprite clipping remain identical on every frame. */
    BlockCopy(_g->openings, frame_bg_openings,
              frame_bg_opening_count * sizeof(*frame_bg_openings));

    R_FrameBgClearDirty(page);
    frame_bg_dirty_page = page;
    frame_bg_dirty_recording = true;
    frame_bg_mask_recording = false;
    frame_bg_mask_replay = g_pcfx_frame_bg_mask_mode &&
                           frame_bg_mask_refs && frame_bg_mask_base;
}

static void R_PlaneSpanCacheInit(void)
{
    if (planespan_cache_block)
        Z_Free(planespan_cache_block);
    planespan_cache_block = NULL;
    planespan_cache_desc = NULL;
    planespan_cache_pixels = NULL;
    frame_bg_pixels = NULL;
    frame_bg_openings = NULL;
    planespan_cache_valid = false;
    planespan_have_view = false;
    frame_bg_valid = false;
    frame_bg_have_last_view = false;
    frame_bg_recording = false;
    for (unsigned p = 0; p < KFB_NUM_BUFS; p++)
        frame_bg_page_valid[p] = false;
    frame_bg_dirty_recording = false;
    frame_bg_mask_refs = NULL;
    frame_bg_mask_base = NULL;
    frame_bg_mask_restore_top = NULL;
    frame_bg_mask_restore_bottom = NULL;
    frame_bg_mask_capacity = frame_bg_mask_used = 0;
    frame_bg_mask_entry_active = false;
    frame_bg_mask_recording = false;
    frame_bg_mask_replay = false;
    for (unsigned p = 0; p < KFB_NUM_BUFS; p++)
        R_FrameBgClearDirty(p);

    const unsigned desc_bytes = PLANESPAN_CACHE_MAX_SPANS *
                                sizeof(*planespan_cache_desc);
    const unsigned plane_bytes = PLANESPAN_CACHE_PIXELS * sizeof(uint16_t);
    /* Background replay and per-span replay are mutually exclusive uses of the
     * same 52 KB pixel store.  Sharing keeps the optimization inside the RAM
     * budget that already succeeds on E1M6. */
    const unsigned need = desc_bytes + plane_bytes;
    if (g_planespan_cache_mode && Z_LargestFreeBlock() >= need &&
        Z_TotalFree() >= need + PLANESPAN_CACHE_HEAP_FLOOR)
    {
        planespan_cache_block = Z_Malloc((int)need, PU_CACHE,
                                         (void **)&planespan_cache_block);
        if (planespan_cache_block)
        {
            planespan_cache_desc =
                (planespan_cache_desc_t *)planespan_cache_block;
            frame_bg_openings = (unsigned short *)planespan_cache_desc;
            frame_bg_opening_capacity = desc_bytes / sizeof(*frame_bg_openings);
            planespan_cache_pixels =
                (unsigned short *)(planespan_cache_block + desc_bytes);
            frame_bg_pixels = planespan_cache_pixels;
        }
    }
}

static inline void R_PlaneSpanCacheBegin(void)
{
    if (!planespan_cache_block)
    {
        planespan_cache_desc = NULL;
        planespan_cache_pixels = NULL;
        planespan_cache_valid = false;
    }
    planespan_frame_spans = 0;
    planespan_frame_pixels = 0;
#ifndef PCFX_SPAN_C_REFERENCE
    if (g_pcfx_frame_bg_mode && frame_bg_recording)
    {
        /* The shared pixel store contains a row-major opaque background, not
         * span-order data.  Normal frames use the fast uncached assembly span
         * kernel; matching frames replay the whole background instead. */
        planespan_cache_bypass = true;
        planespan_cache_replay = false;
        planespan_cache_failed = !planespan_cache_block;
        return;
    }
#endif
    planespan_cache_bypass = planespan_have_view &&
        (viewx != planespan_last_viewx || viewy != planespan_last_viewy ||
         viewz != planespan_last_viewz || viewangle != planespan_last_viewangle);
    planespan_last_viewx = viewx;
    planespan_last_viewy = viewy;
    planespan_last_viewz = viewz;
    planespan_last_viewangle = viewangle;
    planespan_have_view = true;
    planespan_cache_replay = planespan_cache_valid && !planespan_cache_bypass;
    planespan_cache_failed = !planespan_cache_block;
}

static inline void R_PlaneSpanCacheEnd(void)
{
    /* Do not spend a moving frame populating data that the next camera pose
     * cannot reuse.  Keep the old stable-view cache; the first stationary
     * frame will replace it after its exact descriptor mismatch. */
    if (planespan_cache_bypass)
        return;
    if (planespan_cache_failed)
    {
        planespan_cache_valid = false;
        return;
    }
    planespan_cached_spans = planespan_frame_spans;
    planespan_cached_pixels = planespan_frame_pixels;
    planespan_cache_valid = true;
}

/* 1 = replay cached words, 0 = record freshly sampled words, -1 = bypass. */
static inline int R_PlaneSpanCacheProbe(unsigned y, unsigned x1,
                                        unsigned count, unsigned position,
                                        unsigned step, const byte *lit,
                                        const lighttable_t *cmap,
                                        const unsigned short **cached,
                                        unsigned short **record)
{
    if (planespan_cache_bypass)
        return -1;
    if (planespan_cache_failed || count > 255u ||
        planespan_frame_spans >= PLANESPAN_CACHE_MAX_SPANS ||
        planespan_frame_pixels + count > PLANESPAN_CACHE_PIXELS)
    {
        planespan_cache_failed = true;
        planespan_cache_replay = false;
        return -1;
    }

    planespan_cache_desc_t *desc =
        &planespan_cache_desc[planespan_frame_spans];
    unsigned short *pixels = planespan_cache_pixels + planespan_frame_pixels;
    boolean hit = planespan_cache_replay &&
                  planespan_frame_spans < planespan_cached_spans &&
                  planespan_frame_pixels + count <= planespan_cached_pixels &&
                  desc->position == position && desc->step == step &&
                  desc->lit == lit && desc->cmap == cmap &&
                  desc->lump == s_flatlump && desc->y == y &&
                  desc->x1 == x1 && desc->count == count;

    if (!hit)
    {
        planespan_cache_replay = false;
        desc->position = position;
        desc->step = step;
        desc->lit = lit;
        desc->cmap = cmap;
        desc->lump = s_flatlump;
        desc->y = (uint16_t)y;
        desc->x1 = (uint8_t)x1;
        desc->count = (uint8_t)count;
        *record = pixels;
    }
    else
        *cached = pixels;

    planespan_frame_spans++;
    planespan_frame_pixels += count;
    return hit ? 1 : 0;
}

#endif

// Called at the END of P_SetupLevel (heap fully committed). W_PrecacheReserve
// purges PU_CACHE before the next map, and the explicit free also makes this
// routine safe if it is ever called twice without that transition cleanup.
void R_LitFlatLevelInit(void)
{
#ifdef SERIAL_LOG
    /* Allocate permanent profiler storage before the purgeable render caches;
     * a first-frame PU_STATIC allocation would otherwise evict them. */
    R_ProfileEnsureStorage();
#endif
    if (litflat_data)
        Z_Free(litflat_data);
    litflat_data = NULL;
    litflat_slots = 0;
    litflat_next = 0;
    litflat_last_lump = -1;
    litflat_last_cmap = NULL;
    litflat_last_result = NULL;
    lf_bakes = lf_bakes_prev = 0;
    lf_strikes = 0;
    memset(litflat_hash, -1, sizeof(litflat_hash));

    /* A dense PC-FX flat is 32x64, so each pre-lit entry needs only 2 KB. Mixed
     * WADs may still contain original 64x64 fallback flats; those simply use the
     * normal two-load drawer and never enter this dense cache. */
    litflat_stride = LITFLAT_TEXELS;
#ifdef PCFX_TEXTURE_PAGES
    litflat_stride = PCFX_PAGE_BYTES;
#endif

    unsigned flat_slots = LITFLAT_MAX_SLOTS;
    /* Translation exercises more distance-light tuples than an in-place turn.
     * Retain all 64 flat entries and let the independently gated wall cache
     * choose its 256-column fallback on memory-tight maps.  The mixed-movement
     * benchmark showed that avoiding 2 KiB flat rebakes outweighs the larger
     * wall cache in actual forward/reverse movement. */
    unsigned need = flat_slots * litflat_stride;
    // One contiguous block for the cache, AND enough left over for gameplay decode.
#ifdef LITFLAT_FORCE_OFF
    if (0)
#else
    if (Z_LargestFreeBlock() >= need && Z_TotalFree() >= need + LITFLAT_HEAP_FLOOR)
#endif
    {
        litflat_data = Z_Malloc((int)need, PU_CACHE, (void **)&litflat_data);
        if (litflat_data)
        {
            litflat_slots = (int)flat_slots;
            for(int i = 0; i < litflat_slots; i++)
                litflat_lump[i] = -1;   // no real lump is < 0 -> every slot misses
        }
    }
#ifdef PCFX_TEXTURE_PAGES
    R_LitWallLevelInit();
    R_PlaneSpanCacheInit();
#endif
}

// Once per rendered frame: if the level is sustaining a high bake rate the cache
// is thrashing (working set > slots), so drop to the 2-load drawer for good.
void R_LitFlatFrameCheck(void)
{
    if (!litflat_slots) return;
    uint32_t baked = lf_bakes - lf_bakes_prev;
    lf_bakes_prev = lf_bakes;
    if (baked > LITFLAT_THRASH_BAKES)
    {
        if (++lf_strikes >= LITFLAT_THRASH_STRIKES)
            litflat_slots = 0;          // disable for the rest of the level
    }
    else
        lf_strikes = 0;
}

#ifdef PCFX_TEXTURE_PAGES
static inline unsigned R_LitFlatHash(int lump, const lighttable_t* cmap,
                                     boolean pcfx32)
#else
static inline unsigned R_LitFlatHash(int lump, const lighttable_t* cmap)
#endif
{
    /* Colormaps are 256 16-bit entries = 512-byte aligned steps. Shifting the
     * pointer by nine exposes the light-table number instead of hashing zeros. */
    unsigned h = (unsigned)lump * 33u ^ (unsigned)((uintptr_t)cmap >> 9);
#ifdef PCFX_TEXTURE_PAGES
    h ^= (unsigned)pcfx32 << 6;
#endif
    return h & (LITFLAT_HASH_SIZE - 1);
}

#ifdef PCFX_TEXTURE_PAGES
static inline boolean R_LitFlatMatches(int slot, int lump,
                                       const lighttable_t* cmap, boolean pcfx32)
#else
static inline boolean R_LitFlatMatches(int slot, int lump,
                                       const lighttable_t* cmap)
#endif
{
    return litflat_lump[slot] == lump && litflat_cmap[slot] == cmap
#ifdef PCFX_TEXTURE_PAGES
        && litflat_pcfx32[slot] == (byte)pcfx32
#endif
        ;
}

#ifdef PCFX_TEXTURE_PAGES
static __attribute__((noinline,cold)) const byte *
R_BakeLitFlat(int lump, const lighttable_t *cmap, const byte *src,
              boolean pcfx32)
#else
static __attribute__((noinline,cold)) const byte *
R_BakeLitFlat(int lump, const lighttable_t *cmap, const byte *src)
#endif
{
    unsigned slot = litflat_next;
    if (++litflat_next >= (unsigned)litflat_slots)
        litflat_next = 0;
    lf_bakes++;

    if (litflat_lump[slot] >= 0)
    {
        unsigned oldhash = R_LitFlatHash(litflat_lump[slot], litflat_cmap[slot]
#ifdef PCFX_TEXTURE_PAGES
                                         , litflat_pcfx32[slot]
#endif
                                         );
        for (unsigned probes = 0; probes < LITFLAT_HASH_SIZE; probes++)
        {
            int oldslot = litflat_hash[oldhash];
            if (oldslot == -1)
                break;
            if (oldslot == (int)slot)
            {
                litflat_hash[oldhash] = -2;
                break;
            }
            oldhash = (oldhash + 1) & (LITFLAT_HASH_SIZE - 1);
        }
    }

    byte *dst = litflat_data + slot * litflat_stride;
#ifdef PCFX_TEXTURE_PAGES
    const unsigned texels = pcfx32 ? PCFX_PAGE_BYTES : LITFLAT_TEXELS;
#else
    const unsigned texels = LITFLAT_TEXELS;
#endif
    if (texels > litflat_stride)
        return NULL;
#ifdef PCFX_TEXTURE_PAGES
    if (!pcfx32 && litflat_stride < LITFLAT_TEXELS)
        return NULL;
#endif
#if defined(PCFX_TEXTURE_PAGES) && !defined(PCFX_FLAT_BAKE_C_REFERENCE)
    if (pcfx32)
    {
        pcfx_bake_litflat2048(src, cmap, dst);
#ifdef PCFX_FLAT_BAKE_VERIFY
        for (unsigned i = 0; i < texels; i++)
            if (dst[i] != (byte)cmap[src[i]])
                I_Error("PCFX flat bake mismatch");
#endif
    }
    else
#endif
        for (unsigned i = 0; i < texels; i++)
            dst[i] = (byte)cmap[src[i]];

    litflat_lump[slot] = lump;
    litflat_cmap[slot] = cmap;
#ifdef PCFX_TEXTURE_PAGES
    litflat_pcfx32[slot] = (byte)pcfx32;
#endif

    unsigned hash = R_LitFlatHash(lump, cmap
#ifdef PCFX_TEXTURE_PAGES
                                  , pcfx32
#endif
                                  );
    int first_tombstone = -1;
    int insert_at = -1;
    for (unsigned probes = 0; probes < LITFLAT_HASH_SIZE; probes++)
    {
        int entry = litflat_hash[hash];
        if (entry == -2 && first_tombstone < 0)
            first_tombstone = (int)hash;
        else if (entry == -1)
        {
            insert_at = first_tombstone >= 0 ? first_tombstone : (int)hash;
            break;
        }
        hash = (hash + 1) & (LITFLAT_HASH_SIZE - 1);
    }
    if (insert_at < 0)
        insert_at = first_tombstone;
    if (insert_at >= 0)
        litflat_hash[insert_at] = (signed char)slot;
    return dst;
}

#ifdef PCFX_TEXTURE_PAGES
static inline __attribute__((always_inline)) const byte*
R_GetLitFlat(int lump, const lighttable_t* cmap, const byte* src, boolean pcfx32)
#else
static inline __attribute__((always_inline)) const byte*
R_GetLitFlat(int lump, const lighttable_t* cmap, const byte* src)
#endif
{
    // An unrelated W_CacheLumpNum/Z_Malloc may have purged the optional block
    // since the preceding plane. Its owner pointer is cleared by Z_Free.
    if (!litflat_data)
    {
        litflat_slots = 0;
        return NULL;
    }

    /* R_MakeSpans emits neighbouring rows of one visplane consecutively, and
     * Doom's distance lighting maps many adjacent rows to the same colormap.
     * Avoid hashing and probing static metadata again for the common identical
     * (flat, light) tuple. */
    if (lump == litflat_last_lump && cmap == litflat_last_cmap
#ifdef PCFX_TEXTURE_PAGES
        && pcfx32 == litflat_last_pcfx32
#endif
       )
        return litflat_last_result;

#ifdef PCFX_TEXTURE_PAGES
    /* A 64x64 fallback does not fit a 2 KB dense slot. It remains correct on the
     * existing two-load span path while generated 32x64 flats use this cache. */
    if (!pcfx32 && litflat_stride < LITFLAT_TEXELS)
        return NULL;
#endif

    unsigned hash = R_LitFlatHash(lump, cmap
#ifdef PCFX_TEXTURE_PAGES
                                  , pcfx32
#endif
                                  );
    for (unsigned probes = 0; probes < LITFLAT_HASH_SIZE; probes++)
    {
        int slot = litflat_hash[hash];
        if (slot == -1)
            break;
        if (slot >= 0 && R_LitFlatMatches(slot, lump, cmap
#ifdef PCFX_TEXTURE_PAGES
                                           , pcfx32
#endif
                                           ))
        {
            litflat_last_lump = lump;
            litflat_last_cmap = cmap;
#ifdef PCFX_TEXTURE_PAGES
            litflat_last_pcfx32 = pcfx32;
#endif
            return litflat_last_result =
                litflat_data + (unsigned)slot * litflat_stride;
        }
        hash = (hash + 1) & (LITFLAT_HASH_SIZE - 1);
    }

    const byte *result = R_BakeLitFlat(lump, cmap, src
#ifdef PCFX_TEXTURE_PAGES
                         , pcfx32
#endif
                         );
    litflat_last_lump = lump;
    litflat_last_cmap = cmap;
#ifdef PCFX_TEXTURE_PAGES
    litflat_last_pcfx32 = pcfx32;
#endif
    return litflat_last_result = result;
}
#endif

static void R_MapPlane(unsigned int y, unsigned int x1, unsigned int x2, draw_span_vars_t *dsvars)
{
#ifdef SERIAL_LOG
    int profile_plane = R_ProfileSample(&rprof_plane_sample_seq);
    uint64_t profile_plane_t0 = profile_plane ? itu_ticks() : 0;
    uint64_t profile_plane_t1;
    g_rp_plane_spans++;
    /* Span drawers use x2 as an exclusive endpoint in this port. */
    if (x2 > x1)
        g_rp_plane_pixels += x2 - x1;
#endif
    const fixed_t distance = FixedMul(planeheight, yslope[y]);
    dsvars->step = ((FixedMul(distance,basexscale) << 10) & 0xffff0000) | ((FixedMul(distance,baseyscale) >> 6) & 0x0000ffff);

    // diminished lighting: this row's colormap by depth into the plane
#ifndef GBA
    const byte* lit = NULL;
#endif
    if (planezlight)
    {
        unsigned int li = (unsigned int)distance >> LIGHTZSHIFT;
        if (li >= MAXLIGHTZ) li = MAXLIGHTZ - 1;
        dsvars->colormap = planezlight[li];
#ifndef GBA
        // Pre-lit fast path (1 load/pixel), when this level has the cache. Only the
        // diminished-light path is lit-flatted; the fixedcolormap (invuln) path
        // keeps the 2-load drawer.
        if (litflat_slots)
#ifdef PCFX_TEXTURE_PAGES
            lit = R_GetLitFlat(s_flatlump, dsvars->colormap, dsvars->source,
                               dsvars->pcfx32);
#else
            lit = R_GetLitFlat(s_flatlump, dsvars->colormap, dsvars->source);
#endif
#endif
    }

    fixed_t length = FixedMul (distance, distscale[x1]);
    angle_t angle = (viewangle + xtoviewangle[x1])>>ANGLETOFINESHIFT;

    // killough 2/28/98: Add offsets
    unsigned int xfrac =  viewx + FixedMul(finecosine[angle], length);
    unsigned int yfrac = -viewy - FixedMul(finesine[angle],   length);

    dsvars->position = ((xfrac << 10) & 0xffff0000) | ((yfrac >> 6)  & 0x0000ffff);

#ifdef SERIAL_LOG
    if (profile_plane)
    {
        profile_plane_t1 = itu_ticks();
        g_rp_plane_setup += (uint32_t)(profile_plane_t1 - profile_plane_t0);
    }
#endif

#ifndef GBA
    if (lit)
    {
#ifdef PCFX_TEXTURE_PAGES
        if (dsvars->pcfx32)
        {
            const unsigned count = x2 - x1;
#ifndef PCFX_SPAN_C_REFERENCE
            /* Exact whole-background replay supersedes the older sequential
             * span recorder on stationary views. Moving views cannot reuse its
             * descriptors, so submit the direct-KRAM leaf unconditionally. */
            FB_span(x1, y);
            pcfx_span_lit32(count, dsvars->position, dsvars->step, lit);
#else
            R_DrawSpanLit32(y, x1, x2, dsvars->position, dsvars->step, lit);
#endif
        }
        else
#endif
            R_DrawSpanLit(y, x1, x2, dsvars->position, dsvars->step, lit);
    }
    else
#endif
#ifdef PCFX_TEXTURE_PAGES
    if (dsvars->pcfx32)
    {
#ifdef PCFX_SPAN32_C_REFERENCE
        R_DrawSpan32(y, x1, x2, dsvars);
#else
        /* Same shape as the lit path above: seat the cursor here and submit the
         * run through the direct-KRAM leaf, so the hot drawer is a placed leaf
         * rather than a .text function that shares icache lines with us. */
        FB_span(x1, y);
        pcfx_span32(x2 - x1, dsvars);
#endif
    }
    else
#endif
        R_DrawSpan(y, x1, x2, dsvars);
#ifdef SERIAL_LOG
    if (profile_plane)
    {
        g_rp_plane_draw += (uint32_t)(itu_ticks() - profile_plane_t1);
        g_rp_plane_samples++;
    }
#endif
}

//
// R_MakeSpans
//

static void R_MakeSpans(int x, unsigned int t1, unsigned int b1, unsigned int t2, unsigned int b2, draw_span_vars_t *dsvars)
{
    for (; t1 < t2 && t1 <= b1; t1++)
        R_MapPlane(t1, spanstart[t1], x, dsvars);

    for (; b1 > b2 && b1 >= t1; b1--)
        R_MapPlane(b1, spanstart[b1], x, dsvars);

    while (t2 < t1 && t2 <= b2)
        spanstart[t2++] = x;

    while (b2 > b1 && b2 >= t2)
        spanstart[b2--] = x;
}



// New function, by Lee Killough

static void R_DoDrawPlane(visplane_t *pl)
{
    register int x;
    draw_column_vars_t dcvars;

    R_SetDefaultDrawColumnVars(&dcvars);

    if (pl->minx <= pl->maxx)
    {
        if (pl->picnum == _g->skyflatnum)
        { // sky flat

            /* PC-FX: the sky is the hardware RAINBOW background layer, panned by
             * the view angle. So instead of drawing the SKY1 texture we leave the
             * sky columns TRANSPARENT (palette index 0), which lets the RAINBOW
             * show through. Every sky pixel must be written each frame (the KRAM
             * pages are double-buffered), so fill the run with index 0. */
            for (x = pl->minx; x <= pl->maxx; x++)
            {
                int yl = pl->top[x];
                int yh = pl->bottom[x];
                if (yl != -1 && yl <= yh)
                {
                    int count = yh - yl + 1;
                    FB_col((unsigned)x, (unsigned)yl);
                    while (count-- > 0)
                        FB_put(0);   /* index 0 = transparent -> RAINBOW sky */
                }
            }
        }
        else
        {     // regular flat

            draw_span_vars_t dsvars;

            // Raw 8bpp flat; R_MapPlane picks a per-row colormap from planezlight
            // so the floor/ceiling darkens with depth (was one flat colormap).
            {
#ifdef PCFX_TEXTURE_PAGES
                R_FrameBgMarkValue(frame_bg_flats, &frame_bg_num_flats,
                                   FRAME_BG_MAX_FLATS, pl->picnum);
#endif
                int flatnum = flattranslation[pl->picnum];
                int original = _g->firstflat + flatnum;
#ifdef PCFX_TEXTURE_PAGES
                int optimized = pcfx_flatlumps[flatnum];
                int _fl = optimized >= 0 ? optimized : original;
#else
                int _fl = original;
#endif
#ifndef GBA
                s_flatlump = _fl;       // key for the per-row lit-flat cache
#endif
#ifdef PCFX_TEXTURE_PAGES
                dsvars.pcfx32 = optimized >= 0;
                dsvars.source = dsvars.pcfx32
                    ? R_PCFCachedPage(_fl, true) : W_CacheLumpNum(_fl);
#else
                dsvars.source = W_CacheLumpNum(_fl);
#endif
            }

            if (fixedcolormap)
            {
                planezlight = NULL;
                dsvars.colormap = fixedcolormap;
            }
            else
            {
                planezlight = zlight[R_LightNum(pl->lightlevel)];
                dsvars.colormap = planezlight[MAXLIGHTZ - 1];
            }

            planeheight = D_abs(pl->height-viewz);

            const int stop = pl->maxx + 1;

            pl->top[pl->minx-1] = pl->top[stop] = 0xff; // dropoff overflow

            for (x = pl->minx ; x <= stop ; x++)
            {
                R_MakeSpans(x,pl->top[x-1],pl->bottom[x-1], pl->top[x],pl->bottom[x], &dsvars);
            }
        }
    }
}




//*******************************************

//
// R_ScaleFromGlobalAngle
// Returns the texture mapping scale
//  for the current line (horizontal span)
//  at the given angle.
// rw_distance must be calculated first.
//
// killough 5/2/98: reformatted, cleaned up
// CPhipps - moved here from r_main.c

static fixed_t R_ScaleFromGlobalAngle(angle_t visangle)
{
  int     anglea = ANG90 + (visangle-viewangle);
  int     angleb = ANG90 + (visangle-rw_normalangle);

  int     den = FixedMul(rw_distance, finesine[anglea>>ANGLETOFINESHIFT]);

// proff 11/06/98: Changed for high-res
  fixed_t num = FixedMul(projectiony, finesine[angleb>>ANGLETOFINESHIFT]);

  return den > num>>16 ? (num = FixedDiv(num, den)) > 64*FRACUNIT ?
    64*FRACUNIT : num < 256 ? 256 : num : 64*FRACUNIT;
}


//
// R_NewVisSprite
//
static vissprite_t *R_NewVisSprite(void)
{
    if (num_vissprite >= MAXVISSPRITES)
    {
#ifdef DEV_RCHECK
        g_vis_drop++;
#endif
#ifdef RANGECHECK
        I_Error("Vissprite overflow.");
#endif
        return NULL;
    }

    return _g->vissprites + num_vissprite++;
}


//
// R_ProjectSprite
// Generates a vissprite for a thing if it might be visible.
//

static void R_ProjectSprite (mobj_t* thing, int lightlevel)
{
    const fixed_t fx = thing->x;
    const fixed_t fy = thing->y;
    const fixed_t fz = thing->z;

    const fixed_t tr_x = fx - viewx;
    const fixed_t tr_y = fy - viewy;

    const fixed_t tz = FixedMul(tr_x,viewcos)-(-FixedMul(tr_y,viewsin));

    // thing is behind view plane?
    if (tz < MINZ)
        return;

    //Too far away. Always draw Cyberdemon and Spiderdemon. They are big sprites!
    if( (tz > MAXZ) && (thing->type != MT_CYBORG) && (thing->type != MT_SPIDER) )
        return;

    fixed_t tx = -(FixedMul(tr_y,viewcos)+(-FixedMul(tr_x,viewsin)));

    // too far off the side?
    if (D_abs(tx)>(tz<<2))
        return;

    // decide which patch to use for sprite relative to player
    const spritedef_t* sprdef = &_g->sprites[thing->sprite];
    const spriteframe_t* sprframe = &sprdef->spriteframes[thing->frame & FF_FRAMEMASK];

    unsigned int rot = 0;

    if (sprframe->rotate)
    {
        // choose a different rotation based on player view
        angle_t ang = R_PointToAngle(fx, fy);
        rot = (ang-thing->angle+(unsigned)(ANG45/2)*9)>>29;
    }

    const boolean flip = (boolean)SPR_FLIPPED(sprframe, rot);
    const patch_t* patch = W_CacheLumpNum(sprframe->lump[rot] + _g->firstspritelump);

    /* calculate edges of the shape
     * cph 2003/08/1 - fraggle points out that this offset must be flipped
     * if the sprite is flipped; e.g. FreeDoom imp is messed up by this. */
    if (flip)
        tx -= (SHORT(patch->width) - SHORT(patch->leftoffset)) << FRACBITS;
    else
        tx -= SHORT(patch->leftoffset) << FRACBITS;

    const fixed_t xscale = FixedDiv(projection, tz);

    fixed_t xl = (centerxfrac + FixedMul(tx,xscale));

    // off the side?
    if(xl > (SCREENWIDTH << FRACBITS))
        return;

    fixed_t xr = (centerxfrac + FixedMul(tx + (SHORT(patch->width) << FRACBITS),xscale)) - FRACUNIT;

    // off the side?
    if(xr < 0)
        return;

    //Too small.
    if(xr <= (xl + (FRACUNIT >> 2)))
        return;


    const int x1 = (xl >> FRACBITS);
    const int x2 = (xr >> FRACBITS);

    // store information in a vissprite
    vissprite_t* vis = R_NewVisSprite ();

    //No more vissprites.
    if(!vis)
        return;

    vis->mobjflags = thing->flags;
    // proff 11/06/98: Changed for high-res
    vis->scale = FixedDiv(projectiony, tz);
    /* Vertical texture step = 1 / vertical scale. It MUST use the same projection
     * (projectiony) as vis->scale above; the old `tz >> 7` assumed a 128-unit
     * projection, but projectiony is 192 here, so it sampled the sprite texture
     * ~1.5x too fast — the post ran off its end about 2/3 of the way down and the
     * bottom third read past it, scrambling every sprite. Mirror the wall/xiscale
     * path (FixedReciprocal of the matching scale). */
    vis->iscale = FixedReciprocal((unsigned)vis->scale);
    vis->patch = patch;
    vis->lump = sprframe->lump[rot] + _g->firstspritelump;  // re-cache at draw (PU_CACHE)
    vis->gx = fx;
    vis->gy = fy;
    vis->gz = fz;
    vis->texturemid = (fz + (SHORT(patch->topoffset) << FRACBITS)) - viewz;
    vis->x1 = x1 < 0 ? 0 : x1;
    vis->x2 = x2 >= SCREENWIDTH ? SCREENWIDTH-1 : x2;


    //const fixed_t iscale = FixedDiv (FRACUNIT, xscale);
    const fixed_t iscale = FixedReciprocal(xscale);

    if (flip)
    {
        vis->startfrac = (SHORT(patch->width)<<FRACBITS)-1;
        vis->xiscale = -iscale;
    }
    else
    {
        vis->startfrac = 0;
        vis->xiscale = iscale;
    }

    if (vis->x1 > x1)
        vis->startfrac += vis->xiscale*(vis->x1-x1);

    // get light level
    if (thing->flags & MF_SHADOW)
        vis->colormap = NULL;             // shadow draw
    else if (fixedcolormap)
        vis->colormap = fixedcolormap;      // fixed map
    else if (thing->frame & FF_FULLBRIGHT)
        vis->colormap = fullcolormap;     // full bright  // killough 3/20/98
    else
    {      // diminished light: darker with distance (by the sprite's scale)
        int li = (int)(xscale >> LIGHTSCALESHIFT);
        if (li >= MAXLIGHTSCALE) li = MAXLIGHTSCALE - 1;
        else if (li < 0) li = 0;
        vis->colormap = scalelight[R_LightNum(lightlevel)][li];
    }
}

//
// R_AddSprites
// During BSP traversal, this adds sprites by sector.
//
// killough 9/18/98: add lightlevel as parameter, fixing underwater lighting
static void R_AddSprites(subsector_t* subsec, int lightlevel)
{
  sector_t* sec=subsec->sector;
  mobj_t *thing;

  // BSP is traversed by subsector.
  // A sector might have been split into several
  //  subsectors during BSP building.
  // Thus we check whether its already added.

  if (sec->validcount == _g->validcount)
    return;

  // Well, now it will be done.
  sec->validcount = _g->validcount;

  // Handle all things in sector.

  for (thing = sec->thinglist; thing; thing = thing->snext)
    R_ProjectSprite(thing, lightlevel);
}

//
// R_FindPlane
//
// killough 2/28/98: Add offsets


// New function, by Lee Killough

static visplane_t *new_visplane(unsigned hash)
{
    visplane_t *check = _g->freetail;

    if (!check)
        check = Z_Calloc(1, sizeof(visplane_t), PU_LEVEL, NULL);
    else
    {
        if (!(_g->freetail = _g->freetail->next))
            _g->freehead = &_g->freetail;
    }

    check->next = _g->visplanes[hash];
    _g->visplanes[hash] = check;

    return check;
}

static visplane_t *R_FindPlane(fixed_t height, int picnum, int lightlevel)
{
    visplane_t *check;
    unsigned hash;                      // killough

    if (picnum == _g->skyflatnum)
        height = lightlevel = 0;         // killough 7/19/98: most skies map together

    // New visplane algorithm uses hash table -- killough
    hash = visplane_hash(picnum,lightlevel,height);

    for (check=_g->visplanes[hash]; check; check=check->next)  // killough
        if (height == check->height &&
                picnum == check->picnum &&
                lightlevel == check->lightlevel)
            return check;

    check = new_visplane(hash);         // killough

    check->height = height;
    check->picnum = picnum;
    check->lightlevel = lightlevel;
    check->minx = SCREENWIDTH; // Was SCREENWIDTH -- killough 11/98
    check->maxx = -1;

    BlockSet(check->top, UINT_MAX, sizeof(check->top));

    check->modified = false;

    return check;
}

/*
 * R_DupPlane
 *
 * cph 2003/04/18 - create duplicate of existing visplane and set initial range
 */
static visplane_t *R_DupPlane(const visplane_t *pl, int start, int stop)
{
    unsigned hash = visplane_hash(pl->picnum, pl->lightlevel, pl->height);
    visplane_t *new_pl = new_visplane(hash);

    new_pl->height = pl->height;
    new_pl->picnum = pl->picnum;
    new_pl->lightlevel = pl->lightlevel;
    new_pl->minx = start;
    new_pl->maxx = stop;

    BlockSet(new_pl->top, UINT_MAX, sizeof(new_pl->top));

    new_pl->modified = false;

    return new_pl;
}


//
// R_CheckPlane
//
static visplane_t *R_CheckPlane(visplane_t *pl, int start, int stop)
{
    int intrl, intrh, unionl, unionh, x;

    if (start < pl->minx)
        intrl   = pl->minx, unionl = start;
    else
        unionl  = pl->minx,  intrl = start;

    if (stop  > pl->maxx)
        intrh   = pl->maxx, unionh = stop;
    else
        unionh  = pl->maxx, intrh  = stop;

    for (x=intrl ; x <= intrh && pl->top[x] == 0xff; x++) // dropoff overflow
        ;

    if (x > intrh) { /* Can use existing plane; extend range */
        pl->minx = unionl; pl->maxx = unionh;
        return pl;
    } else /* Cannot use existing plane; create a new one */
        return R_DupPlane(pl,start,stop);
}

static void R_DrawColumnInCache(const column_t* patch, byte* cache, int originy, int cacheheight)
{
    while (patch->topdelta != 0xff)
    {
        const byte* source = (const byte *)patch + 3;
        int count = patch->length;
        int position = originy + patch->topdelta;

        if (position < 0)
        {
            count += position;
            position = 0;
        }

        if (position + count > cacheheight)
            count = cacheheight - position;

        if (count > 0)
            ByteCopy(cache + position, source, count);

        patch = (const column_t *)(  (const byte *)patch + patch->length + 4);
    }
}

/*
 * Draw a column of pixels of the specified texture.
 * If the texture is simple (1 patch, full height) then just draw
 * straight from const patch_t*.
*/

#define CACHE_WAYS 4

#define CACHE_MASK (CACHE_WAYS-1)
#define CACHE_STRIDE (128 / CACHE_WAYS)
#define CACHE_KEY_MASK (CACHE_STRIDE-1)

#define CACHE_ENTRY(c, t) ((c << 16 | t))

#define CACHE_HASH(c, t) (((c >> 1) ^ t) & CACHE_KEY_MASK)

static unsigned int FindColumnCacheItem(unsigned int texture, unsigned int column)
{
    //static unsigned int looks, peeks;
    //looks++;

    unsigned int cx = CACHE_ENTRY(column, texture);

    unsigned int key = CACHE_HASH(column, texture);

    unsigned int* cc = (unsigned int*)&columnCacheEntries[key];

    unsigned int i = key;

    do
    {
        //peeks++;
        unsigned int cy = *cc;

        if((cy == cx) || (cy == 0))
            return i;

        cc+=CACHE_STRIDE;
        i+=CACHE_STRIDE;

    } while(i < 128);


    //No space. Random eviction.
    return ((M_Random() & CACHE_MASK) * CACHE_STRIDE) + key;
}


static const byte* R_ComposeColumn(const unsigned int texture, const texture_t* tex, int texcolumn, unsigned int iscale)
{
    //static int total, misses;
    /* KING output is permanently one 16-bit fat word per 128-wide logical X.
     * Keep the low-detail composite-column quantisation regardless of the legacy
     * menu flag; exact-column "high detail" cannot add display resolution here. */
    int colmask = 0xfffe;

    if(tex->width > 8)
    {
        if(iscale > (4 << FRACBITS))
            colmask = 0xfff0;
        else if(iscale > (3 << FRACBITS))
            colmask = 0xfff8;
        else if (iscale > (2 << FRACBITS))
            colmask = 0xfffc;
    }


    const int xc = (texcolumn & colmask) & tex->widthmask;

    unsigned int cachekey = FindColumnCacheItem(texture, xc);

    byte* colcache = &columnCache[cachekey*128];
    unsigned int cacheEntry = columnCacheEntries[cachekey];

    //total++;

    if(cacheEntry != CACHE_ENTRY(xc, texture))
    {
        //misses++;
        byte tmpCache[128];


        columnCacheEntries[cachekey] = CACHE_ENTRY(xc, texture);

        unsigned int i = 0;
        unsigned int patchcount = tex->patchcount;

        do
        {
            const texpatch_t* patch = &tex->patches[i];

            const int x1 = patch->originx;

            if(xc < x1)
                continue;

            // Patch data is PU_CACHE: re-fetch (re-streams if evicted). Each
            // matching column is copied into tmpCache immediately, so a later
            // patch's fetch evicting an earlier one here is harmless.
            const patch_t* realpatch = (const patch_t*)W_CacheLumpNum(patch->lump);

            const int x2 = x1 + SHORT(realpatch->width);

            if(xc < x2)
            {
                const column_t* patchcol = (const column_t *)((const byte *)realpatch + LONG(realpatch->columnofs[xc-x1]));

                R_DrawColumnInCache (patchcol,
                                     tmpCache,
                                     patch->originy,
                                     tex->height);

            }

        } while(++i < patchcount);

        //Block copy will drop low 2 bits of len.
        BlockCopy(colcache, tmpCache, (tex->height + 3));
    }

    return colcache;
}

#ifdef PCFX_TEXTURE_PAGES
typedef struct
{
    const lighttable_t *cmap;
    const litwall_pixel_t *lit;
    unsigned column;
} litwall_last_t;

typedef struct
{
    const texture_t *tex;
    const byte *page;
    litwall_last_t lit;
    fixed_t scaled_texturemid;
} segtexture_t;

#ifndef PCFX_DEFER_WALL_PREP_INIT
#define PCFX_DEFER_WALL_PREP_INIT 0
#endif
/* Retain the data symbol used by old profiling scripts, but make the ablation
 * a compile-time choice.  The dispatcher now has an explicit cache-shaped
 * section, so carrying the never-taken release branch only bloats its live
 * cache footprint.  Initializer 1 remains the behavioral ablation. */
volatile int g_pcfx_defer_wall_prep
    __attribute__((section(".data.pcfx_defer_wall_prep"))) =
    PCFX_DEFER_WALL_PREP_INIT;
#endif

/* Keep the legacy composer as cold compatibility code and, on PC-FX, as a
 * layout anchor for the ordinary renderer text.  The dense-only dispatcher
 * below has no reference or run-time branch to it. */
static __attribute__((noinline,cold,used,section(".pcfx_wallcold"))) void
R_PrepareGenericSegTextureColumn(unsigned int texture, const texture_t *tex,
                                 int texcolumn, draw_column_vars_t *dcvars);

/* The release dispatcher is kept at cache index 0x1a0.  Its compact 0x166-byte
 * body overlaps the seg loop's least-executed band; accepting return-block
 * refills is cheaper than evicting the hot loop head and backedge blocks. */
static __attribute__((noinline,hot,section(".pcfx_walldispatch"))) void
#ifdef PCFX_TEXTURE_PAGES
R_DrawSegTextureColumn(segtexture_t *seg,
#else
R_DrawSegTextureColumn(unsigned int texture, const texture_t* tex,
#endif
                                   int texcolumn, draw_column_vars_t* dcvars)
{
#ifdef PCFX_TEXTURE_PAGES
    const texture_t *tex = seg->tex;
    litwall_last_t *lit_ref = &seg->lit;
#if PCFX_DEFER_WALL_PREP_INIT
    if (!dcvars->iscale)
    {
        dcvars->iscale = FixedReciprocal((unsigned)rw_scale);
        if (walllights)
        {
            int li = (int)(rw_scale >> LIGHTSCALESHIFT);
            if (li >= MAXLIGHTSCALE) li = MAXLIGHTSCALE - 1;
            else if (li < 0) li = 0;
            dcvars->colormap = walllights[li];
        }
    }
#endif
#endif
#ifdef SERIAL_LOG
    g_rp_wall_columns++;
    int profile_sample = R_ProfileSample(&rprof_wall_sample_seq);
    uint64_t fetch_t0 = profile_sample ? itu_ticks() : 0;
#endif
    // tex is resolved once per seg by the caller (R_RenderSegLoop) instead of
    // re-looking it up (R_GetOrLoadTexture) on every column.

#ifdef PCFX_TEXTURE_PAGES
    const byte* page = NULL;
    const litwall_pixel_t* litcolumn = NULL;
    unsigned pcx = 0;
    page = seg->page;
    if (!page)
        seg->page = page = R_PCFCachedPage(tex->pcfx_lump, false);
    int xc = texcolumn & tex->widthmask;
    pcx = ((unsigned)xc * (unsigned)tex->pcfx_xscale) >> FRACBITS;
    if (pcx >= 32) pcx = 31;
    dcvars->source = page + pcx * 64;
    if (litwall_slots)
    {
        if (litwall_block && lit_ref->lit && lit_ref->column == pcx &&
            lit_ref->cmap == dcvars->colormap)
        {
            litcolumn = lit_ref->lit;
#ifdef SERIAL_LOG
            g_rp_wall_lit_hits++;
#endif
        }
        else
        {
            litcolumn = R_GetLitWallColumn(tex->pcfx_lump, pcx,
                                           dcvars->colormap, dcvars->source);
            lit_ref->column = pcx;
            lit_ref->cmap = dcvars->colormap;
            lit_ref->lit = litcolumn;
        }
    }
#else
    R_PrepareGenericSegTextureColumn(texture, tex, texcolumn, dcvars);
#endif

#ifdef SERIAL_LOG
    uint64_t draw_t0 = profile_sample ? itu_ticks() : 0;
    if (profile_sample)
    {
        g_rp_fetch += (uint32_t)(draw_t0 - fetch_t0);
        g_rp_fetch_samples++;
    }
#endif
#ifdef PCFX_TEXTURE_PAGES
    if (litcolumn)
        R_DrawColumnPCFXLit(dcvars, tex->pcfx_yscale,
                            seg->scaled_texturemid, litcolumn);
    else
        R_DrawColumnPCFX(dcvars, tex->pcfx_yscale, seg->scaled_texturemid);
#else
        R_DrawColumn(dcvars);
#endif
#ifdef SERIAL_LOG
    uint64_t draw_t1 = profile_sample ? itu_ticks() : 0;
    int pixel_count = dcvars->yh - dcvars->yl + 1;
    if (pixel_count > 0)
        g_rp_wall_pixels += (unsigned)pixel_count;
#ifdef PCFX_TEXTURE_PAGES
    R_ProfileWallTuple(tex->pcfx_lump, pcx, dcvars->colormap);
#endif
    if (profile_sample)
    {
        g_rp_draw += (uint32_t)(draw_t1 - draw_t0);
        g_rp_draw_samples++;
        if (pixel_count > 0)
            g_rp_draw_pixels += (unsigned)pixel_count;
    }
#endif
}

static __attribute__((noinline,cold,used,section(".pcfx_wallcold"))) void
R_PrepareGenericSegTextureColumn(unsigned int texture, const texture_t *tex,
                                 int texcolumn, draw_column_vars_t *dcvars)
{
    if (tex->overlapped == 0)
    {
        const column_t *column = R_GetColumn(tex, texcolumn);
        dcvars->source = (const byte *)column + 3;
    }
    else
        dcvars->source = R_ComposeColumn(texture, tex, texcolumn,
                                         dcvars->iscale);
}

//
// R_RenderSegLoop
// Draws zero, one, or two textures (and possibly a masked texture) for walls.
// Can draw or mark the starting pixel of floor and ceiling textures.
// CALLED: CORE LOOPING ROUTINE.
//

#define HEIGHTBITS 12
#define HEIGHTUNIT (1<<HEIGHTBITS)

/* With inline ABI prologues, R_RenderSegLoop's generated per-column body is
 * 950 bytes (V810 GCC 4.9.4), from function offset 0x1e0 through 0x594.  The
 * V810 instruction cache is a
 * direct-mapped 1 KiB window; arbitrary placement makes that backward loop
 * cross a cache boundary and evict its own first instructions every column.
 * Start the function at +0x220 within an aligned window so the loop begins at
 * the next 1 KiB boundary and fits wholly inside it.  The padding is never
 * executed and this named section keeps the relationship stable at link time. */
__asm__(".section .pcfx_renderseg,\"ax\"\n"
        ".balign 1024\n"
        ".space 0x21e\n" /* assembler rounds the function itself to +0x220 */
        ".previous\n");
static __attribute__((noinline,hot,section(".pcfx_renderseg")))
void R_RenderSegLoop (int rw_x)
{
    draw_column_vars_t dcvars;
    fixed_t  texturecolumn = 0;   // shut up compiler warning

    R_SetDefaultDrawColumnVars(&dcvars);

    // Pick this seg's wall light table (sector light + fake contrast); the actual
    // colormap is chosen per column below from the projected scale (distance).
    if (fixedcolormap)
    {
        walllights = NULL;
        dcvars.colormap = fixedcolormap;
    }
    else
    {
        int lightnum = R_LightNum(rw_lightlevel);
        if (curline)
        {
            if (curline->v1.y == curline->v2.y)      lightnum--;   // E-W wall: darker
            else if (curline->v1.x == curline->v2.x) lightnum++;   // N-S wall: brighter
            if (lightnum < 0) lightnum = 0;
            else if (lightnum >= LIGHTLEVELS) lightnum = LIGHTLEVELS - 1;
        }
        walllights = scalelight[lightnum];
        dcvars.colormap = walllights[MAXLIGHTSCALE - 1];
    }

    // Textures are constant for the whole seg: resolve the pointers once here
    // rather than per column inside R_DrawSegTextureColumn.
    const texture_t* midtex = midtexture    ? R_GetOrLoadTexture(midtexture)    : NULL;
    const texture_t* toptex = toptexture    ? R_GetOrLoadTexture(toptexture)    : NULL;
    const texture_t* bottex = bottomtexture ? R_GetOrLoadTexture(bottomtexture) : NULL;

#ifdef PCFX_TEXTURE_PAGES
    /* Dense pages and vertical texture origins are invariant for this whole seg.
     * Each page pointer is filled lazily on the first visible column, then reused
     * without scanning the eight-slot page cache. Hoist one of the two former
     * per-column FixedMul operations from the page drawer as well. */
    segtexture_t midseg = {
        midtex, NULL, { NULL, NULL, 0 },
        midtex ? FixedMul(rw_midtexturemid, midtex->pcfx_yscale) : 0
    };
    segtexture_t topseg = {
        toptex, NULL, { NULL, NULL, 0 },
        toptex ? FixedMul(rw_toptexturemid, toptex->pcfx_yscale) : 0
    };
    segtexture_t botseg = {
        bottex, NULL, { NULL, NULL, 0 },
        bottex ? FixedMul(rw_bottomtexturemid, bottex->pcfx_yscale) : 0
    };

#endif

    // rw_x = start, rw_stopx = stop+1, and a stored seg always has start <= stop,
    // so this loop always runs at least once -> bottom-tested do/while drops the
    // compiler's initial entry guard branch. (cf. d32xr b89de43)
    /* Keep the measured backwards-branch target at function offset 0x1c4;
     * changing this instruction perturbs the dispatcher's caller overlap. */
    __asm__ volatile ("nop");
    do
    {
        // mark floor / ceiling areas

        int yh = bottomfrac>>HEIGHTBITS;
        int yl = (topfrac+HEIGHTUNIT-1)>>HEIGHTBITS;

        int cc_rwx = ceilingclip[rw_x];
        int fc_rwx = floorclip[rw_x];

        // no space above wall?
        int bottom,top = cc_rwx+1;

        if (yl < top)
            yl = top;

        if (markceiling)
        {
            bottom = yl-1;

            if (bottom >= fc_rwx)
                bottom = fc_rwx-1;

            if (top <= bottom)
            {
                ceilingplane->top[rw_x] = top;
                ceilingplane->bottom[rw_x] = bottom;
                ceilingplane->modified = true;
            }
            // SoM: this should be set here
            cc_rwx = bottom;
        }

        bottom = fc_rwx-1;
        if (yh > bottom)
            yh = bottom;

        if (markfloor)
        {

            top  = yh < cc_rwx ? cc_rwx : yh;

            if (++top <= bottom)
            {
                floorplane->top[rw_x] = top;
                floorplane->bottom[rw_x] = bottom;
                floorplane->modified = true;
            }
            // SoM: This should be set here to prevent overdraw
            fc_rwx = top;
        }

        // texturecolumn and lighting are independent of wall tiers
        if (segtextured)
        {
            // calculate texture offset
            angle_t angle =(rw_centerangle+xtoviewangle[rw_x])>>ANGLETOFINESHIFT;

            texturecolumn = rw_offset-FixedMul(finetangent[angle],rw_distance);

            texturecolumn >>= FRACBITS;

            dcvars.x = rw_x;
#if defined(PCFX_TEXTURE_PAGES) && PCFX_DEFER_WALL_PREP_INIT
            dcvars.iscale = 0;
#else
            {
                dcvars.iscale = FixedReciprocal((unsigned)rw_scale);
                if (walllights)
                {
                    int li = (int)(rw_scale >> LIGHTSCALESHIFT);
                    if (li >= MAXLIGHTSCALE) li = MAXLIGHTSCALE - 1;
                    else if (li < 0) li = 0;
                    dcvars.colormap = walllights[li];
                }
            }
#endif
        }

        // draw the wall tiers
        if (midtexture)
        {

            dcvars.yl = yl;     // single sided line
            dcvars.yh = yh;
            dcvars.texturemid = rw_midtexturemid;
            //

#ifdef PCFX_TEXTURE_PAGES
            R_DrawSegTextureColumn(&midseg, texturecolumn, &dcvars);
#else
            R_DrawSegTextureColumn(midtexture, midtex,
                                   texturecolumn, &dcvars);
#endif

            cc_rwx = viewheight;
            fc_rwx = -1;
        }
        else
        {

            // two sided line
            if (toptexture)
            {
                // top wall
                int mid = pixhigh>>HEIGHTBITS;
                pixhigh += pixhighstep;

                if (mid >= fc_rwx)
                    mid = fc_rwx-1;

                if (mid >= yl)
                {
                    dcvars.yl = yl;
                    dcvars.yh = mid;
                    dcvars.texturemid = rw_toptexturemid;

#ifdef PCFX_TEXTURE_PAGES
                    R_DrawSegTextureColumn(&topseg, texturecolumn, &dcvars);
#else
                    R_DrawSegTextureColumn(toptexture, toptex,
                                           texturecolumn, &dcvars);
#endif

                    cc_rwx = mid;
                }
                else
                    cc_rwx = yl-1;
            }
            else  // no top wall
            {

                if (markceiling)
                    cc_rwx = yl-1;
            }

            if (bottomtexture)          // bottom wall
            {
                int mid = (pixlow+HEIGHTUNIT-1)>>HEIGHTBITS;
                pixlow += pixlowstep;

                // no space above wall?
                if (mid <= cc_rwx)
                    mid = cc_rwx+1;

                if (mid <= yh)
                {
                    dcvars.yl = mid;
                    dcvars.yh = yh;
                    dcvars.texturemid = rw_bottomtexturemid;

#ifdef PCFX_TEXTURE_PAGES
                    R_DrawSegTextureColumn(&botseg, texturecolumn, &dcvars);
#else
                    R_DrawSegTextureColumn(bottomtexture, bottex,
                                           texturecolumn, &dcvars);
#endif

                    fc_rwx = mid;
                }
                else
                    fc_rwx = yh+1;
            }
            else        // no bottom wall
            {
                if (markfloor)
                    fc_rwx = yh+1;
            }

            // cph - if we completely blocked further sight through this column,
            // add this info to the solid columns array for r_bsp.c
            if ((markceiling || markfloor) && (fc_rwx <= cc_rwx + 1))
            {
#ifdef SOLIDCOL_BYTE_REFERENCE
                solidcol[rw_x] = 1;
#else
                R_SetSolidColumn(rw_x);
#endif
                didsolidcol = 1;
            }

            // save texturecol for backdrawing of masked mid texture
            if (maskedtexture)
                maskedtexturecol[rw_x] = texturecolumn;
        }

        rw_scale += rw_scalestep;
        topfrac += topstep;
        bottomfrac += bottomstep;

        floorclip[rw_x] = fc_rwx;
        ceilingclip[rw_x] = cc_rwx;
    } while (++rw_x < rw_stopx);
}

static boolean R_CheckOpenings(const int start)
{
    int pos = _g->lastopening - _g->openings;
    int need = (rw_stopx - start)*4 + pos;

#ifdef DEV_RCHECK
    if (need > (int)g_peak_open) g_peak_open = need;
    if (need > MAXOPENINGS) g_open_drop++;
#endif
#ifdef RANGECHECK
    if(need > MAXOPENINGS)
        I_Error("Openings overflow. Need = %d", need);
#endif

    return need <= MAXOPENINGS;
}

//
// R_StoreWallRange
// A wall segment will be drawn
//  between start and stop pixels (inclusive).
//
static void R_StoreWallRange(const int start, const int stop)
{
#ifdef SERIAL_LOG
    g_rp_store_calls++;
    int profile_store = R_ProfileSample(&rprof_store_sample_seq);
    uint64_t profile_store_t0 = profile_store ? itu_ticks() : 0;
#define RPROF_STORE_FINISH() do { \
    if (profile_store) { \
        g_rp_store += (uint32_t)(itu_ticks() - profile_store_t0); \
        g_rp_store_samples++; \
    } \
} while (0)
#else
#define RPROF_STORE_FINISH() do { } while (0)
#endif
    fixed_t hyp;
    angle_t offsetangle;

    // don't overflow and crash
    if (ds_p == &_g->drawsegs[MAXDRAWSEGS])
    {
#ifdef DEV_RCHECK
        g_ds_drop++;
#endif
#ifdef RANGECHECK
        I_Error("Drawsegs overflow.");
#endif
        RPROF_STORE_FINISH();
        return;
    }


    linedata_t* linedata = &_g->linedata[curline->linenum];

    // mark the segment as visible for auto map
    linedata->r_flags |= ML_MAPPED;

    sidedef = &_g->sides[curline->sidenum];
    linedef = &_g->lines[curline->linenum];
#ifdef PCFX_TEXTURE_PAGES
    R_FrameBgMarkValue(frame_bg_sides, &frame_bg_num_sides,
                       FRAME_BG_MAX_SIDES, curline->sidenum);
#endif

    // calculate rw_distance for scale calculation
    rw_normalangle = curline->angle + ANG90;

    offsetangle = rw_normalangle-rw_angle1;

    if (D_abs(offsetangle) > ANG90)
        offsetangle = ANG90;

    hyp = (viewx==curline->v1.x && viewy==curline->v1.y)?
                0 : R_PointToDist (curline->v1.x, curline->v1.y);

    rw_distance = FixedMul(hyp, finecosine[offsetangle>>ANGLETOFINESHIFT]);

    int rw_x = ds_p->x1 = start;
    ds_p->x2 = stop;
    ds_p->curline = curline;
    rw_stopx = stop+1;

    //Openings overflow. Nevermind.
    if(!R_CheckOpenings(start))
    {
        RPROF_STORE_FINISH();
        return;
    }

    // calculate scale at both ends and step
    ds_p->scale1 = rw_scale = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[start]);

    if (stop > start)
    {
        ds_p->scale2 = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[stop]);
        ds_p->scalestep = rw_scalestep = IDiv32(ds_p->scale2-rw_scale, stop-start);
    }
    else
        ds_p->scale2 = ds_p->scale1;

    // calculate texture boundaries
    //  and decide if floor / ceiling marks are needed

    worldtop = frontsector->ceilingheight - viewz;
    worldbottom = frontsector->floorheight - viewz;

    midtexture = toptexture = bottomtexture = maskedtexture = 0;
    ds_p->maskedtexturecol = NULL;

    if (!backsector)
    {
        // single sided line
#ifdef PCFX_TEXTURE_PAGES
        R_FrameBgMarkValue(frame_bg_textures, &frame_bg_num_textures,
                           FRAME_BG_MAX_TEXTURES, sidedef->midtexture);
#endif
        midtexture = texturetranslation[sidedef->midtexture];

        // a single sided line is terminal, so it must mark ends
        markfloor = markceiling = true;

        if (linedef->flags & ML_DONTPEGBOTTOM)
        {         // bottom of texture at bottom
            fixed_t vtop = frontsector->floorheight + textureheight[sidedef->midtexture];
            rw_midtexturemid = vtop - viewz;
        }
        else        // top of texture at top
            rw_midtexturemid = worldtop;

        rw_midtexturemid += FixedMod( (sidedef->rowoffset << FRACBITS), textureheight[midtexture]);

        ds_p->silhouette = SIL_BOTH;
        ds_p->sprtopclip = screenheightarray;
        ds_p->sprbottomclip = negonearray;
        ds_p->bsilheight = INT_MAX;
        ds_p->tsilheight = INT_MIN;
    }
    else      // two sided line
    {
        ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
        ds_p->silhouette = 0;

        if(linedata->r_flags & RF_CLOSED)
        { /* cph - closed 2S line e.g. door */
            // cph - killough's (outdated) comment follows - this deals with both
            // "automap fixes", his and mine
            // killough 1/17/98: this test is required if the fix
            // for the automap bug (r_bsp.c) is used, or else some
            // sprites will be displayed behind closed doors. That
            // fix prevents lines behind closed doors with dropoffs
            // from being displayed on the automap.

            ds_p->silhouette = SIL_BOTH;
            ds_p->sprbottomclip = negonearray;
            ds_p->bsilheight = INT_MAX;
            ds_p->sprtopclip = screenheightarray;
            ds_p->tsilheight = INT_MIN;

        }
        else
        { /* not solid - old code */

            if (frontsector->floorheight > backsector->floorheight)
            {
                ds_p->silhouette = SIL_BOTTOM;
                ds_p->bsilheight = frontsector->floorheight;
            }
            else
                if (backsector->floorheight > viewz)
                {
                    ds_p->silhouette = SIL_BOTTOM;
                    ds_p->bsilheight = INT_MAX;
                }

            if (frontsector->ceilingheight < backsector->ceilingheight)
            {
                ds_p->silhouette |= SIL_TOP;
                ds_p->tsilheight = frontsector->ceilingheight;
            }
            else
                if (backsector->ceilingheight < viewz)
                {
                    ds_p->silhouette |= SIL_TOP;
                    ds_p->tsilheight = INT_MIN;
                }
        }

        worldhigh = backsector->ceilingheight - viewz;
        worldlow = backsector->floorheight - viewz;

        // hack to allow height changes in outdoor areas
        if (frontsector->ceilingpic == _g->skyflatnum && backsector->ceilingpic == _g->skyflatnum)
            worldtop = worldhigh;

        markfloor = worldlow != worldbottom
                || backsector->floorpic != frontsector->floorpic
                || backsector->lightlevel != frontsector->lightlevel
                ;

        markceiling = worldhigh != worldtop
                || backsector->ceilingpic != frontsector->ceilingpic
                || backsector->lightlevel != frontsector->lightlevel
                ;

        if (backsector->ceilingheight <= frontsector->floorheight || backsector->floorheight >= frontsector->ceilingheight)
            markceiling = markfloor = true;   // closed door

        if (worldhigh < worldtop)   // top texture
        {
#ifdef PCFX_TEXTURE_PAGES
            R_FrameBgMarkValue(frame_bg_textures, &frame_bg_num_textures,
                               FRAME_BG_MAX_TEXTURES, sidedef->toptexture);
#endif
            toptexture = texturetranslation[sidedef->toptexture];
            rw_toptexturemid = linedef->flags & ML_DONTPEGTOP ? worldtop :
                                                                        backsector->ceilingheight+textureheight[sidedef->toptexture]-viewz;
            rw_toptexturemid += FixedMod( (sidedef->rowoffset << FRACBITS), textureheight[toptexture]);
        }

        if (worldlow > worldbottom) // bottom texture
        {
#ifdef PCFX_TEXTURE_PAGES
            R_FrameBgMarkValue(frame_bg_textures, &frame_bg_num_textures,
                               FRAME_BG_MAX_TEXTURES, sidedef->bottomtexture);
#endif
            bottomtexture = texturetranslation[sidedef->bottomtexture];
            rw_bottomtexturemid = linedef->flags & ML_DONTPEGBOTTOM ? worldtop : worldlow;

            rw_bottomtexturemid += FixedMod( (sidedef->rowoffset << FRACBITS), textureheight[bottomtexture]);
        }

        // allocate space for masked texture tables
        if (sidedef->midtexture)    // masked midtexture
        {
#ifdef PCFX_TEXTURE_PAGES
            R_FrameBgMarkValue(frame_bg_textures, &frame_bg_num_textures,
                               FRAME_BG_MAX_TEXTURES, sidedef->midtexture);
#endif
            maskedtexture = true;
            ds_p->maskedtexturecol = maskedtexturecol = _g->lastopening - rw_x;
            _g->lastopening += rw_stopx - rw_x;
        }
    }

    // calculate rw_offset (only needed for textured lines)
    segtextured = ((midtexture | toptexture | bottomtexture | maskedtexture) > 0);

    if (segtextured)
    {
        rw_offset = FixedMul (hyp, -finesine[offsetangle >>ANGLETOFINESHIFT]);

        rw_offset += (sidedef->textureoffset << FRACBITS) + curline->offset;

        rw_centerangle = ANG90 + viewangle - rw_normalangle;

        rw_lightlevel = frontsector->lightlevel;
    }

    // if a floor / ceiling plane is on the wrong side of the view
    // plane, it is definitely invisible and doesn't need to be marked.
    if (frontsector->floorheight >= viewz)       // above view plane
        markfloor = false;
    if (frontsector->ceilingheight <= viewz &&
            frontsector->ceilingpic != _g->skyflatnum)   // below view plane
        markceiling = false;

    // calculate incremental stepping values for texture edges
    worldtop >>= 4;
    worldbottom >>= 4;

    topstep = -FixedMul (rw_scalestep, worldtop);
    topfrac = (centeryfrac>>4) - FixedMul (worldtop, rw_scale);

    bottomstep = -FixedMul (rw_scalestep,worldbottom);
    bottomfrac = (centeryfrac>>4) - FixedMul (worldbottom, rw_scale);

    if (backsector)
    {
        worldhigh >>= 4;
        worldlow >>= 4;

        if (worldhigh < worldtop)
        {
            pixhigh = (centeryfrac>>4) - FixedMul (worldhigh, rw_scale);
            pixhighstep = -FixedMul (rw_scalestep,worldhigh);
        }
        if (worldlow > worldbottom)
        {
            pixlow = (centeryfrac>>4) - FixedMul (worldlow, rw_scale);
            pixlowstep = -FixedMul (rw_scalestep,worldlow);
        }
    }

    // render it
    if (markceiling)
    {
        if (ceilingplane)   // killough 4/11/98: add NULL ptr checks
            ceilingplane = R_CheckPlane (ceilingplane, rw_x, rw_stopx-1);
        else
            markceiling = 0;
    }

    if (markfloor)
    {
        if (floorplane)     // killough 4/11/98: add NULL ptr checks
            /* cph 2003/04/18  - ceilingplane and floorplane might be the same
       * visplane (e.g. if both skies); R_CheckPlane doesn't know about
       * modifications to the plane that might happen in parallel with the check
       * being made, so we have to override it and split them anyway if that is
       * a possibility, otherwise the floor marking would overwrite the ceiling
       * marking, resulting in HOM. */
            if (markceiling && ceilingplane == floorplane)
                floorplane = R_DupPlane (floorplane, rw_x, rw_stopx-1);
            else
                floorplane = R_CheckPlane (floorplane, rw_x, rw_stopx-1);
        else
            markfloor = 0;
    }

    didsolidcol = 0;
    /* Everything above is per-range setup; the seg loop and post-loop sprite
     * clipping have their own buckets. */
    RPROF_STORE_FINISH();
#ifdef SERIAL_LOG
    { uint64_t seg_t0 = itu_ticks();
      R_RenderSegLoop(rw_x);
      g_rp_seg += (uint32_t)(itu_ticks() - seg_t0); }
#else
    R_RenderSegLoop(rw_x);
#endif

    /* cph - if a column was made solid by this wall, we _must_ save full clipping info */
    if (backsector && didsolidcol)
    {
        if (!(ds_p->silhouette & SIL_BOTTOM))
        {
            ds_p->silhouette |= SIL_BOTTOM;
            ds_p->bsilheight = backsector->floorheight;
        }
        if (!(ds_p->silhouette & SIL_TOP))
        {
            ds_p->silhouette |= SIL_TOP;
            ds_p->tsilheight = backsector->ceilingheight;
        }
    }

    // save sprite clipping info
    if ((ds_p->silhouette & SIL_TOP || maskedtexture) && !ds_p->sprtopclip)
    {
        ByteCopy((byte*)_g->lastopening, (const byte*)(ceilingclip+start), sizeof(short)*(rw_stopx-start));
        ds_p->sprtopclip = _g->lastopening - start;
        _g->lastopening += rw_stopx - start;
    }

    if ((ds_p->silhouette & SIL_BOTTOM || maskedtexture) && !ds_p->sprbottomclip)
    {
        ByteCopy((byte*)_g->lastopening, (const byte*)(floorclip+start), sizeof(short)*(rw_stopx-start));
        ds_p->sprbottomclip = _g->lastopening - start;
        _g->lastopening += rw_stopx - start;
    }

    if (maskedtexture && !(ds_p->silhouette & SIL_TOP))
    {
        ds_p->silhouette |= SIL_TOP;
        ds_p->tsilheight = INT_MIN;
    }
#undef RPROF_STORE_FINISH

    if (maskedtexture && !(ds_p->silhouette & SIL_BOTTOM))
    {
        ds_p->silhouette |= SIL_BOTTOM;
        ds_p->bsilheight = INT_MAX;
    }

    ds_p++;
}


// killough 1/18/98 -- This function is used to fix the automap bug which
// showed lines behind closed doors simply because the door had a dropoff.
//
// cph - converted to R_RecalcLineFlags. This recalculates all the flags for
// a line, including closure and texture tiling.

static void R_RecalcLineFlags(void)
{
    linedata_t* linedata = &_g->linedata[linedef->lineno];

    const side_t* side = &_g->sides[curline->sidenum];

    linedata->r_validcount = (_g->gametic & 0xffff);

    /* First decide if the line is closed, normal, or invisible */
    if (!(linedef->flags & ML_TWOSIDED)
            || backsector->ceilingheight <= frontsector->floorheight
            || backsector->floorheight >= frontsector->ceilingheight
            || (
                // if door is closed because back is shut:
                backsector->ceilingheight <= backsector->floorheight

                // preserve a kind of transparent door/lift special effect:
                && (backsector->ceilingheight >= frontsector->ceilingheight ||
                    side->toptexture)

                && (backsector->floorheight <= frontsector->floorheight ||
                    side->bottomtexture)

                // properly render skies (consider door "open" if both ceilings are sky):
                && (backsector->ceilingpic !=_g->skyflatnum ||
                    frontsector->ceilingpic!=_g->skyflatnum)
                )
            )
        linedata->r_flags = (RF_CLOSED | (linedata->r_flags & ML_MAPPED));
    else
    {
        // Reject empty lines used for triggers
        //  and special events.
        // Identical floor and ceiling on both sides,
        // identical light levels on both sides,
        // and no middle texture.
        // CPhipps - recode for speed, not certain if this is portable though
        if (backsector->ceilingheight != frontsector->ceilingheight
                || backsector->floorheight != frontsector->floorheight
                || side->midtexture
                || backsector->ceilingpic != frontsector->ceilingpic
                || backsector->floorpic != frontsector->floorpic
                || backsector->lightlevel != frontsector->lightlevel)
        {
            linedata->r_flags = (linedata->r_flags & ML_MAPPED); return;
        } else
            linedata->r_flags = (RF_IGNORE | (linedata->r_flags & ML_MAPPED));
    }
}



// CPhipps -
// R_ClipWallSegment
//
// Replaces the old R_Clip*WallSegment functions. It draws bits of walls in those
// columns which aren't solid, and updates the solidcol[] array appropriately

static __attribute__((noinline)) void
R_ClipWallSegment(int first, int last, boolean solid)
{
#ifdef SERIAL_LOG
    g_rp_clip_calls++;
    int profile_clip = R_ProfileSample(&rprof_clip_sample_seq);
    uint64_t profile_clip_t0 = profile_clip ? itu_ticks() : 0;
#define RPROF_CLIP_FINISH() do { \
    if (profile_clip) { \
        g_rp_clip += (uint32_t)(itu_ticks() - profile_clip_t0); \
        g_rp_clip_samples++; \
    } \
} while (0)
#else
#define RPROF_CLIP_FINISH() do { } while (0)
#endif
#ifdef SOLIDCOL_BYTE_REFERENCE
    byte *p;
    while (first < last)
    {
        if (solidcol[first])
        {
            if (!(p = ByteFind(solidcol + first, 0, last - first)))
            {
                RPROF_CLIP_FINISH();
                return;
            }
            first = p - solidcol;
        }
        else
        {
            int to;
            if (!(p = ByteFind(solidcol + first, 1, last - first)))
                to = last;
            else
                to = p - solidcol;
#ifdef SERIAL_LOG
            if (profile_clip)
                g_rp_clip += (uint32_t)(itu_ticks() - profile_clip_t0);
#endif
            R_StoreWallRange(first, to - 1);
#ifdef SERIAL_LOG
            if (profile_clip)
                profile_clip_t0 = itu_ticks();
#endif
            if (solid)
                ByteSet(solidcol + first, 1, to - first);
            first = to;
        }
    }
#else
    while (first < last)
    {
        if (R_SolidColumn(first))
        {
            int open = R_FindSolidColumn(first, last, false);
            if (open == last)
            {
                RPROF_CLIP_FINISH();
                return; // All solid
            }
            first = open;
        }
        else
        {
            int to = R_FindSolidColumn(first, last, true);

#ifdef SERIAL_LOG
            if (profile_clip)
                g_rp_clip += (uint32_t)(itu_ticks() - profile_clip_t0);
#endif
            R_StoreWallRange(first, to-1);
#ifdef SERIAL_LOG
            if (profile_clip)
                profile_clip_t0 = itu_ticks();
#endif

            if (solid)
                R_SetSolidColumns(first, to);

            first = to;
        }
    }
#endif
    RPROF_CLIP_FINISH();
#undef RPROF_CLIP_FINISH
}

//
// R_ClearClipSegs
//

//
// R_AddLine
// Clips the given segment
// and adds any visible pieces to the line list.
//

static void R_AddLine (const seg_t *line)
{
#ifdef SERIAL_LOG
    g_rp_addline_calls++;
    int profile_addline = R_ProfileSample(&rprof_addline_sample_seq);
    uint64_t profile_addline_t0 = profile_addline ? itu_ticks() : 0;
#define RPROF_ADDLINE_FINISH() do { \
    if (profile_addline) { \
        g_rp_addline += (uint32_t)(itu_ticks() - profile_addline_t0); \
        g_rp_addline_samples++; \
        profile_addline = 0; \
    } \
} while (0)
#else
#define RPROF_ADDLINE_FINISH() do { } while (0)
#endif
    int      x1;
    int      x2;
    angle_t  angle1;
    angle_t  angle2;
    angle_t  span;
    angle_t  tspan;

    curline = line;

    angle1 = R_PointToAngle (line->v1.x, line->v1.y);
    angle2 = R_PointToAngle (line->v2.x, line->v2.y);

    // Clip to view edges.
    span = angle1 - angle2;

    // Back side, i.e. backface culling
    if (span >= ANG180)
    {
        RPROF_ADDLINE_FINISH();
        return;
    }

    // Global angle needed by segcalc.
    rw_angle1 = angle1;
    angle1 -= viewangle;
    angle2 -= viewangle;

    tspan = angle1 + clipangle;
    if (tspan > 2*clipangle)
    {
        tspan -= 2*clipangle;

        // Totally off the left edge?
        if (tspan >= span)
        {
            RPROF_ADDLINE_FINISH();
            return;
        }

        angle1 = clipangle;
    }

    tspan = clipangle - angle2;
    if (tspan > 2*clipangle)
    {
        tspan -= 2*clipangle;

        // Totally off the left edge?
        if (tspan >= span)
        {
            RPROF_ADDLINE_FINISH();
            return;
        }
        angle2 = 0-clipangle;
    }

    // The seg is in the view range,
    // but not necessarily visible.

    angle1 = (angle1+ANG90)>>ANGLETOFINESHIFT;
    angle2 = (angle2+ANG90)>>ANGLETOFINESHIFT;

    // killough 1/31/98: Here is where "slime trails" can SOMETIMES occur:
    x1 = viewangletox[angle1];
    x2 = viewangletox[angle2];
    RPROF_ADDLINE_FINISH();

    // Does not cross a pixel?
    if (x1 >= x2)       // killough 1/31/98 -- change == to >= for robustness
        return;

    backsector = SG_BACKSECTOR(line);
#ifdef PCFX_TEXTURE_PAGES
    R_FrameBgMarkSector(backsector, false);
#endif

    /* cph - roll up linedef properties in flags */
    linedef = &_g->lines[curline->linenum];
    linedata_t* linedata = &_g->linedata[linedef->lineno];

    if (linedata->r_validcount != (_g->gametic & 0xffff))
        R_RecalcLineFlags();

    if (linedata->r_flags & RF_IGNORE)
    {
        return;
    }
    else
    {
        R_ClipWallSegment (x1, x2, linedata->r_flags & RF_CLOSED);
    }
#undef RPROF_ADDLINE_FINISH
}

//
// R_Subsector
// Determine floor/ceiling planes.
// Add sprites of things in sector.
// Draw one or more line segments.
//
// killough 1/31/98 -- made static, polished

static void R_Subsector(int num)
{
    int         count;
    const seg_t       *line;
    subsector_t *sub;

    /* Between subsectors no KING KRAM cursor run is open — the cheapest safe
     * place to service a deferred page flip inside the long walls phase. */
    pcfx_present_tick();

    sub = &_g->subsectors[num];
    frontsector = sub->sector;
#ifdef PCFX_TEXTURE_PAGES
    R_FrameBgMarkSector(frontsector, true);
#endif
    count = sub->numlines;
    line = &_g->segs[sub->firstline];

    if(frontsector->floorheight < viewz)
    {
        floorplane = R_FindPlane(frontsector->floorheight,
                                     frontsector->floorpic,
                                     frontsector->lightlevel                // killough 3/16/98
                                     );
    }
    else
    {
        floorplane = NULL;
    }


    if(frontsector->ceilingheight > viewz || (frontsector->ceilingpic == _g->skyflatnum))
    {
        ceilingplane = R_FindPlane(frontsector->ceilingheight,     // killough 3/8/98
                                       frontsector->ceilingpic,
                                       frontsector->lightlevel
                                       );
    }
    else
    {
        ceilingplane = NULL;
    }

    R_AddSprites(sub, frontsector->lightlevel);
    while (count--)
    {
        R_AddLine (line);
        line++;
        curline = NULL; /* cph 2001/11/18 - must clear curline now we're done with it, so R_ColourMap doesn't try using it for other things */
    }
}

/* R_RenderSegLoop used to be inlined into R_Subsector.  Keep a
 * live-but-never-called pad equal to the removed code and section-packing
 * change so later hot functions and platform objects retain their proven link
 * addresses.  The linker keeps this symbol explicitly; it is never called. */
void __attribute__((noinline,used,section(".text.R_Subsector_pad")))
pcfx_r_subsector_layout_pad(void)
{
    __asm__ volatile (".space 0x5e6");
}

//
// R_CheckBBox
// Checks BSP node/subtree bounding box.
// Returns true
//  if some part of the bbox might be visible.
//

static const byte checkcoord[12][4] = // killough -- static const
{
  {3,0,2,1},
  {3,0,2,0},
  {3,1,2,0},
  {0},
  {2,0,2,1},
  {0,0,0,0},
  {3,1,3,0},
  {0},
  {2,0,3,1},
  {2,1,3,1},
  {2,1,3,0}
};

// killough 1/28/98: static // CPhipps - const parameter, reformatted
static boolean R_CheckBBox(const short *bspcoord_raw)
{
#ifdef SERIAL_LOG
    g_rp_bbox_calls++;
    int profile_bbox = R_ProfileSample(&rprof_bbox_sample_seq);
    uint64_t profile_bbox_t0 = profile_bbox ? itu_ticks() : 0;
#define RPROF_BBOX_RETURN(result) do { \
    if (profile_bbox) { \
        g_rp_bbox += (uint32_t)(itu_ticks() - profile_bbox_t0); \
        g_rp_bbox_samples++; \
    } \
    return (result); \
} while (0)
#else
#define RPROF_BBOX_RETURN(result) return (result)
#endif
    angle_t angle1, angle2;

    // Node bbox is stored on-disk (LE); swap the 4 edges into native locals.
    const short bspcoord[4] = {
        (short)SHORT(bspcoord_raw[0]), (short)SHORT(bspcoord_raw[1]),
        (short)SHORT(bspcoord_raw[2]), (short)SHORT(bspcoord_raw[3])
    };

    {
        int        boxpos;
        const byte* check;

        // Find the corners of the box
        // that define the edges from current viewpoint.
        boxpos = (viewx <= ((fixed_t)bspcoord[BOXLEFT]<<FRACBITS) ? 0 : viewx < ((fixed_t)bspcoord[BOXRIGHT]<<FRACBITS) ? 1 : 2) +
                (viewy >= ((fixed_t)bspcoord[BOXTOP]<<FRACBITS) ? 0 : viewy > ((fixed_t)bspcoord[BOXBOTTOM]<<FRACBITS) ? 4 : 8);

        if (boxpos == 5)
            RPROF_BBOX_RETURN(true);

        check = checkcoord[boxpos];
        angle1 = R_PointToAngle (((fixed_t)bspcoord[check[0]]<<FRACBITS), ((fixed_t)bspcoord[check[1]]<<FRACBITS)) - viewangle;
        angle2 = R_PointToAngle (((fixed_t)bspcoord[check[2]]<<FRACBITS), ((fixed_t)bspcoord[check[3]]<<FRACBITS)) - viewangle;
    }

    // cph - replaced old code, which was unclear and badly commented
    // Much more efficient code now
    if ((signed)angle1 < (signed)angle2)
    { /* it's "behind" us */
        /* Either angle1 or angle2 is behind us, so it doesn't matter if we
     * change it to the corect sign
     */
        if ((angle1 >= ANG180) && (angle1 < ANG270))
            angle1 = INT_MAX; /* which is ANG180-1 */
        else
            angle2 = INT_MIN;
    }

    if ((signed)angle2 >= (signed)clipangle) RPROF_BBOX_RETURN(false); // Both off left edge
    if ((signed)angle1 <= -(signed)clipangle) RPROF_BBOX_RETURN(false); // Both off right edge
    if ((signed)angle1 >= (signed)clipangle) angle1 = clipangle; // Clip at left edge
    if ((signed)angle2 <= -(signed)clipangle) angle2 = 0-clipangle; // Clip at right edge

    // Find the first clippost
    //  that touches the source post
    //  (adjacent pixels are touching).
    angle1 = (angle1+ANG90)>>ANGLETOFINESHIFT;
    angle2 = (angle2+ANG90)>>ANGLETOFINESHIFT;
    {
        int sx1 = viewangletox[angle1];
        int sx2 = viewangletox[angle2];
        //    const cliprange_t *start;

        // Does not cross a pixel.
        if (sx1 == sx2)
            RPROF_BBOX_RETURN(false);

#ifdef SOLIDCOL_BYTE_REFERENCE
        if (!ByteFind(solidcol + sx1, 0, sx2 - sx1)) RPROF_BBOX_RETURN(false);
#else
        if (R_FindSolidColumn(sx1, sx2, false) == sx2) RPROF_BBOX_RETURN(false);
#endif
        // All columns it covers are already solidly covered
    }

    RPROF_BBOX_RETURN(true);
#undef RPROF_BBOX_RETURN
}

//Render a BSP subsector if bspnum is a leaf node.
//Return false if bspnum is frame node.





static boolean R_RenderBspSubsector(int bspnum)
{
    // Found a subsector?
    if (bspnum & NF_SUBSECTOR)
    {
#ifdef SERIAL_LOG
        g_rp_bsp_subsectors++;
#endif
        if (bspnum == -1)
            R_Subsector (0);
        else
            R_Subsector (bspnum & (~NF_SUBSECTOR));

        return true;
    }

    return false;
}

// RenderBSPNode
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.

//Non recursive version.
//constant stack space used and easier to
//performance profile.
#define MAX_BSP_DEPTH 128

static void R_RenderBSPNode(int bspnum)
{
    int stack[MAX_BSP_DEPTH];
    int sp = 0;

    const mapnode_t* bsp;
    int side = 0;

    while(true)
    {
        //Front sides.
        while (!R_RenderBspSubsector(bspnum))
        {
            if(sp == MAX_BSP_DEPTH)
                break;

            bsp = &nodes[bspnum];
#ifdef SERIAL_LOG
            g_rp_bsp_nodes++;
#endif
            side = R_PointOnSide (viewx, viewy, bsp);

            stack[sp++] = bspnum;
            stack[sp++] = side;

            bspnum = (unsigned short)SHORT(bsp->children[side]);
        }

#ifndef SOLIDCOL_BYTE_REFERENCE
        /* Nothing behind the already-rendered front space can contribute once
         * every logical column is blocked.  Stop before walking parents and
         * projecting more back-child bounding boxes. */
        if (R_AllColumnsSolid())
        {
#ifdef SERIAL_LOG
            g_rp_bsp_earlyouts++;
#endif
            return;
        }
#endif

        if(sp == 0)
        {
            //back at root node and not visible. All done!
            return;
        }

        //Back sides.
        side = stack[--sp];
        bspnum = stack[--sp];
        bsp = &nodes[bspnum];

        // Possibly divide back space.
        //Walk back up the tree until we find
        //a node that has a visible backspace.
        while(!R_CheckBBox (bsp->bbox[side^1]))
        {
            if(sp == 0)
            {
                //back at root node and not visible. All done!
                return;
            }

            //Back side next.
            side = stack[--sp];
            bspnum = stack[--sp];

            bsp = &nodes[bspnum];
        }

        bspnum = (unsigned short)SHORT(bsp->children[side^1]);
    }
}


static void R_ClearDrawSegs(void)
{
    ds_p = _g->drawsegs;
}

static void R_ClearClipSegs (void)
{
#ifdef SOLIDCOL_BYTE_REFERENCE
    BlockSet(solidcol, 0, SCREENWIDTH);
#else
    BlockSet(solidcol, 0, sizeof(solidcol));
#endif
}

//
// R_ClearSprites
// Called at frame start.
//

static void R_ClearSprites(void)
{
    num_vissprite = 0;            // killough
}

//
// RDrawPlanes
// At the end of each frame.
//

static void R_DrawPlanes (void)
{
#ifdef DEV_FRAME_TRACE_COUNTS
    g_rp_n_visplanes = 0;
#endif
    for (int i=0; i<MAXVISPLANES; i++)
    {
        visplane_t *pl = _g->visplanes[i];

        while(pl)
        {
            if(pl->modified)
            {
                pcfx_present_tick();
                R_DoDrawPlane(pl);
#ifdef DEV_FRAME_TRACE_COUNTS
                g_rp_n_visplanes++;
#endif
            }

            pl = pl->next;
        }
    }
}

//
// R_ClearPlanes
// At begining of frame.
//

static void R_ClearPlanes(void)
{
    int i;

    // opening / clipping determination
    for (i=0 ; i<SCREENWIDTH ; i++)
        floorclip[i] = viewheight, ceilingclip[i] = -1;


    for (i=0;i<MAXVISPLANES;i++)    // new code -- killough
        for (*_g->freehead = _g->visplanes[i], _g->visplanes[i] = NULL; *_g->freehead; )
            _g->freehead = &(*_g->freehead)->next;

    _g->lastopening = _g->openings;

    basexscale = FixedMul(viewsin,iprojection);
    baseyscale = FixedMul(viewcos,iprojection);
}

//
// R_RenderView
//
void R_RenderPlayerView (player_t* player)
{
#ifdef SERIAL_LOG
    R_ProfileBeginFrame();
#endif
    if (!lighttables_built)   // COLORMAP is loaded by the time we first render
        R_InitLightTables();

#ifdef PCFX_TEXTURE_PAGES
    if (R_FrameBgProbe(player))
    {
        /* Re-establish lighting/view globals and advance validcount exactly as a
         * normal frame, then restore the clean opaque world and draw live
         * actors/masked walls over the retained drawseg clipping data. */
        R_SetupFrame(player);
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        uint64_t replay_t0 = itu_ticks();
#endif
        R_FrameBgReplay();
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        uint64_t replay_t1 = itu_ticks();
#endif
        R_ClearSprites();
        for (unsigned i = 0; i < frame_bg_num_sprite_sectors; i++)
        {
            sector_t *sec = &_g->sectors[frame_bg_sprite_sectors[i]];
            for (mobj_t *thing = sec->thinglist; thing; thing = thing->snext)
                R_ProjectSprite(thing, sec->lightlevel);
        }
        R_DrawMasked();
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
        g_rp_bsp = 0;
        g_rp_plane = (uint32_t)(replay_t1 - replay_t0);
        g_rp_spr = (uint32_t)(itu_ticks() - replay_t1);
#endif
        return;
    }
    R_FrameBgPrepareRender(player);
#endif

    R_SetupFrame (player);

    // Clear buffers.
    R_ClearClipSegs ();
    R_ClearDrawSegs ();
    R_ClearPlanes ();
    R_ClearSprites ();

#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    uint64_t t0 = itu_ticks();
#endif
    // The head node is the last node output.
    R_RenderBSPNode (numnodes-1);       /* walls (BSP traversal + column draw) */
    pcfx_present_tick();
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    uint64_t t1 = itu_ticks();
#endif
#ifdef PCFX_TEXTURE_PAGES
    R_PlaneSpanCacheBegin();
#endif
    R_DrawPlanes ();                     /* floors + ceilings (spans)           */
#ifdef PCFX_TEXTURE_PAGES
    R_PlaneSpanCacheEnd();
    R_FrameBgCapture(player);
#endif
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    uint64_t t2 = itu_ticks();
#endif
    R_DrawMasked ();                     /* sprites + masked/transparent columns */
#ifndef GBA
    R_LitFlatFrameCheck();               /* disable the lit-flat cache if thrashing */
#ifdef PCFX_TEXTURE_PAGES
    R_LitWallFrameCheck();               /* likewise for the pre-lit wall cache */
#endif
#endif
#if defined(SERIAL_LOG) || defined(COARSE_RENDER_PROFILE)
    g_rp_bsp   = (uint32_t)(t1 - t0);
    g_rp_plane = (uint32_t)(t2 - t1);
    g_rp_spr   = (uint32_t)(itu_ticks() - t2);
#endif
#ifdef DEV_FRAME_TRACE_COUNTS
    g_rp_n_drawsegs   = (uint32_t)(ds_p - _g->drawsegs);
    g_rp_n_vissprites = (uint32_t)num_vissprite;
#endif
#ifdef DEV_RCHECK
    {
        unsigned nds = (unsigned)(ds_p - _g->drawsegs);
        if (nds > g_peak_ds) g_peak_ds = nds;
    }
#endif
}

/* Unscaled patch draw (menu text via M_WriteText, HUD messages, finale). These
 * pass native screen-space coords (e.g. hu_lib guards at x+w>240), so route them
 * to the native 1:1 full-resolution drawer — the old path wrote FAT pixels in
 * word space (0..127), doubling every glyph and running text off-screen. */
void V_DrawPatchNoScale(int x, int y, const patch_t* patch)
{
    V_DrawPatchFull(x, y, patch);
}

//
// P_DivlineSide
// Returns side 0 (front), 1 (back), or 2 (on).
//
// killough 4/19/98: made static, cleaned up

static int P_DivlineSide(fixed_t x, fixed_t y, const divline_t *node)
{
  fixed_t left, right;
  return
    !node->dx ? x == node->x ? 2 : x <= node->x ? node->dy > 0 : node->dy < 0 :
    !node->dy ? (y) == node->y ? 2 : y <= node->y ? node->dx < 0 : node->dx > 0 :
    (right = ((y - node->y) >> FRACBITS) * (node->dx >> FRACBITS)) <
    (left  = ((x - node->x) >> FRACBITS) * (node->dy >> FRACBITS)) ? 0 :
    right == left ? 2 : 1;
}

//
// P_CrossSubsector
// Returns true
//  if strace crosses the given subsector successfully.
//
// killough 4/19/98: made static and cleaned up

static boolean P_CrossSubsector(int num)
{
    const seg_t *seg = _g->segs + _g->subsectors[num].firstline;
    int count;
    fixed_t opentop = 0, openbottom = 0;
    const sector_t *front = NULL, *back = NULL;

    for (count = _g->subsectors[num].numlines; --count >= 0; seg++)
    { // check lines
        int linenum = seg->linenum;

        const line_t *line = &_g->lines[linenum];
        divline_t divl;

        // allready checked other side?
        if(_g->linedata[linenum].validcount == _g->validcount)
            continue;

        _g->linedata[linenum].validcount = _g->validcount;

        if (line->bbox[BOXLEFT] > _g->los.bbox[BOXRIGHT ] ||
                line->bbox[BOXRIGHT] < _g->los.bbox[BOXLEFT  ] ||
                line->bbox[BOXBOTTOM] > _g->los.bbox[BOXTOP   ] ||
                line->bbox[BOXTOP]    < _g->los.bbox[BOXBOTTOM])
            continue;

        // cph - do what we can before forced to check intersection
        if (line->flags & ML_TWOSIDED)
        {

            // no wall to block sight with?
            if ((front = SG_FRONTSECTOR(seg))->floorheight == (back = SG_BACKSECTOR(seg))->floorheight && front->ceilingheight == back->ceilingheight)
                continue;

            // possible occluder
            // because of ceiling height differences
            opentop = front->ceilingheight < back->ceilingheight ?
                        front->ceilingheight : back->ceilingheight ;

            // because of floor height differences
            openbottom = front->floorheight > back->floorheight ?
                        front->floorheight : back->floorheight ;

            // cph - reject if does not intrude in the z-space of the possible LOS
            if ((opentop >= _g->los.maxz) && (openbottom <= _g->los.minz))
                continue;
        }

        // Forget this line if it doesn't cross the line of sight
        const vertex_t *v1,*v2;

        v1 = &line->v1;
        v2 = &line->v2;

        if (P_DivlineSide(v1->x, v1->y, &_g->los.strace) == P_DivlineSide(v2->x, v2->y, &_g->los.strace))
            continue;

        divl.dx = v2->x - (divl.x = v1->x);
        divl.dy = v2->y - (divl.y = v1->y);

        // line isn't crossed?
        if (P_DivlineSide(_g->los.strace.x, _g->los.strace.y, &divl) == P_DivlineSide(_g->los.t2x, _g->los.t2y, &divl))
            continue;


        // cph - if bottom >= top or top < minz or bottom > maxz then it must be
        // solid wrt this LOS
        if (!(line->flags & ML_TWOSIDED) || (openbottom >= opentop) ||
                (opentop < _g->los.minz) || (openbottom > _g->los.maxz))
            return false;

        // crosses a two sided line
        /* cph 2006/07/15 - oops, we missed this in 2.4.0 & .1;
       *  use P_InterceptVector2 for those compat levels only. */
        fixed_t frac = P_InterceptVector2(&_g->los.strace, &divl);

        if (front->floorheight != back->floorheight)
        {
            fixed_t slope = FixedDiv(openbottom - _g->los.sightzstart , frac);
            if (slope > _g->los.bottomslope)
                _g->los.bottomslope = slope;
        }

        if (front->ceilingheight != back->ceilingheight)
        {
            fixed_t slope = FixedDiv(opentop - _g->los.sightzstart , frac);
            if (slope < _g->los.topslope)
                _g->los.topslope = slope;
        }

        if (_g->los.topslope <= _g->los.bottomslope)
            return false;               // stop

    }
    // passed the subsector ok
    return true;
}

boolean P_CrossBSPNode(int bspnum)
{
    while (!(bspnum & NF_SUBSECTOR))
    {
        const mapnode_t *bsp = nodes + bspnum;

        divline_t dl;
        dl.x = ((fixed_t)SHORT(bsp->x) << FRACBITS);
        dl.y = ((fixed_t)SHORT(bsp->y) << FRACBITS);
        dl.dx = ((fixed_t)SHORT(bsp->dx) << FRACBITS);
        dl.dy = ((fixed_t)SHORT(bsp->dy) << FRACBITS);

        int side,side2;
        side = P_DivlineSide(_g->los.strace.x,_g->los.strace.y,&dl)&1;
        side2= P_DivlineSide(_g->los.t2x, _g->los.t2y, &dl);

        if (side == side2)
            bspnum = (unsigned short)SHORT(bsp->children[side]); // doesn't touch the other side
        else         // the partition plane is crossed here
            if (!P_CrossBSPNode((unsigned short)SHORT(bsp->children[side])))
                return 0;  // cross the starting side
            else
                bspnum = (unsigned short)SHORT(bsp->children[side^1]);  // cross the ending side
    }
    return P_CrossSubsector(bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR);
}



//
// P_MobjThinker
//

void P_NightmareRespawn(mobj_t* mobj);
void P_XYMovement (mobj_t* mo);
void P_ZMovement (mobj_t* mo);


//
// P_SetMobjState
// Returns true if the mobj is still present.
//

boolean P_SetMobjState(mobj_t* mobj, statenum_t state)
{
    const state_t*	st;

    do
    {
        if (state == S_NULL)
        {
            mobj->state = (state_t *) S_NULL;
            P_RemoveMobj (mobj);
            return false;
        }

        st = &states[state];
        mobj->state = st;
        mobj->tics = st->tics;
        mobj->sprite = st->sprite;
        mobj->frame = st->frame;

        // Modified handling.
        // Call action functions when the state is set
        if(st->action)
        {
            if(!(_g->player.cheats & CF_ENEMY_ROCKETS))
            {
                st->action(mobj);
            }
            else
            {
                if(mobjinfo[mobj->type].missilestate && (state >= mobjinfo[mobj->type].missilestate) && (state < mobjinfo[mobj->type].painstate))
                    A_CyberAttack(mobj);
                else
                    st->action(mobj);
            }
        }

        state = st->nextstate;

    } while (!mobj->tics);

    return true;
}



void P_MobjThinker (mobj_t* mobj)
{
    // killough 11/98:
    // removed old code which looked at target references
    // (we use pointer reference counting now)

    // momentum movement
    if (mobj->momx | mobj->momy || mobj->flags & MF_SKULLFLY)
    {
        P_XYMovement(mobj);
        if (mobj->thinker.function != P_MobjThinker) // cph - Must've been removed
            return;       // killough - mobj was removed
    }

    if (mobj->z != mobj->floorz || mobj->momz)
    {
        P_ZMovement(mobj);
        if (mobj->thinker.function != P_MobjThinker) // cph - Must've been removed
            return;       // killough - mobj was removed
    }

    // cycle through states,
    // calling action functions at transitions

    if (mobj->tics != -1)
    {
        mobj->tics--;

        // you can cycle through multiple states in a tic

        if (!mobj->tics)
            if (!P_SetMobjState (mobj, mobj->state->nextstate) )
                return;     // freed itself
    }
    else
    {

        // check for nightmare respawn

        if (! (mobj->flags & MF_COUNTKILL) )
            return;

        if (!_g->respawnmonsters)
            return;

        mobj->movecount++;

        if (mobj->movecount < 12*35)
            return;

        if (_g->leveltime & 31)
            return;

        if (P_Random () > 4)
            return;

        P_NightmareRespawn (mobj);
    }

}


//
// P_RunThinkers
//
// killough 4/25/98:
//
// Fix deallocator to stop using "next" pointer after node has been freed
// (a Doom bug).
//
// Process each thinker. For thinkers which are marked deleted, we must
// load the "next" pointer prior to freeing the node. In Doom, the "next"
// pointer was loaded AFTER the thinker was freed, which could have caused
// crashes.
//
// But if we are not deleting the thinker, we should reload the "next"
// pointer after calling the function, in case additional thinkers are
// added at the end of the list.
//
// killough 11/98:
//
// Rewritten to delete nodes implicitly, by making currentthinker
// external and using P_RemoveThinkerDelayed() implicitly.
//

void P_RunThinkers (void)
{
    thinker_t* th = thinkercap.next;
    thinker_t* th_end = &thinkercap;

    while(th != th_end)
    {
        thinker_t* th_next = th->next;
        think_t fn = th->function;
        if(fn)
        {
#ifdef DEV_TIC_PROFILE
            g_rpa_th_count++;
#endif
            /* Inline the two hot, trivial per-frame thinkers so the common
             * "just advance the animation" case pays no indirect call +
             * out-of-line prologue/epilogue (-mprolog-function) overhead. On a
             * busy map (E1M6) these run ~280x/tic and are otherwise memory-bound
             * dispatch, not real work. Behaviour is byte-identical to calling
             * the functions: the fast path is the exact body, and anything that
             * cycles state (tics hits 0) or is not a bare mobj still routes to
             * the original function. */
            if(fn == P_MobjBrainlessThinker)
            {
                mobj_t* m = (mobj_t*)th;
                if(m->tics != -1 && !--m->tics)
                    P_SetMobjState(m, m->state->nextstate);
            }
            else if(fn == P_MobjThinker)
            {
                mobj_t* m = (mobj_t*)th;
                /* Idle mobj fast path: no momentum, resting on its floor, still
                 * animating (tics != -1). With both the momentum and z-motion
                 * blocks skipped, P_MobjThinker's body reduces to exactly this
                 * tics/state advance. Anything moving / with z-motion / at rest
                 * (tics==-1, nightmare-respawn branch) takes the full function. */
                if(!(m->momx | m->momy) && !(m->flags & MF_SKULLFLY)
                   && m->z == m->floorz && !m->momz && m->tics != -1)
                {
                    if(!--m->tics)
                        P_SetMobjState(m, m->state->nextstate);
                }
                else
                    P_MobjThinker(th);
            }
            else
                fn(th);
        }

        th = th_next;
    }
}



// PC-FX: monotonic 35 Hz DOOM tic from the vblank counter. timer_tics()
// computes frames*7/12 in 32-bit (== the old (frames*50/3)*TICRATE/1000) so no
// 64-bit multiply/divide libcall is linked. Monotonic; no basetime/wrap fixups.
int I_GetTime(void)
{
    return timer_tics();
}
